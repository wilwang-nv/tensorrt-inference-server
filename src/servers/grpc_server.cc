// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/servers/grpc_server.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include "grpc++/security/server_credentials.h"
#include "grpc++/server.h"
#include "grpc++/server_builder.h"
#include "grpc++/server_context.h"
#include "grpc++/support/status.h"
#include "grpc/grpc.h"
#include "src/core/constants.h"
#include "src/core/trtserver.h"
#include "src/servers/common.h"

namespace nvidia { namespace inferenceserver {

namespace {

//
// C++11 doesn't have a barrier so we implement our own.
//
class Barrier {
 public:
  explicit Barrier(size_t cnt) : threshold_(cnt), count_(cnt), generation_(0) {}

  void Wait()
  {
    std::unique_lock<std::mutex> lock(mu_);
    auto lgen = generation_;
    if (--count_ == 0) {
      generation_++;
      count_ = threshold_;
      cv_.notify_all();
    } else {
      cv_.wait(lock, [this, lgen] { return lgen != generation_; });
    }
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  const size_t threshold_;
  size_t count_;
  size_t generation_;
};

// The step of processing that the state is in. Every state must
// recognize START, COMPLETE and FINISH and the others are optional.
typedef enum { START, COMPLETE, FINISH, ISSUED, READ, WRITE } Steps;

std::ostream&
operator<<(std::ostream& out, const Steps& step)
{
  switch (step) {
    case START:
      out << "START";
      break;
    case COMPLETE:
      out << "COMPLETE";
      break;
    case FINISH:
      out << "FINISH";
      break;
    case ISSUED:
      out << "ISSUED";
      break;
    case READ:
      out << "READ";
      break;
    case WRITE:
      out << "WRITE";
      break;
  }

  return out;
}

//
// HandlerState
//
template <
    typename ServerResponderType, typename RequestType, typename ResponseType>
class HandlerState {
 public:
  struct RequestResponse {
    RequestType request_;
    ResponseType response_;
  };

  HandlerState(const char* server_id, RequestResponse* req_resp)
      : server_id_(server_id), step_(Steps::START)
  {
    unique_id_ = RequestStatusUtil::NextUniqueRequestId();

    ctx_.reset(new grpc::ServerContext());
    responder_.reset(new ServerResponderType(ctx_.get()));

    req_resp_.emplace(req_resp);
  }

  const char* const server_id_;
  uint64_t unique_id_;
  Steps step_;

  // Context for the rpc, allowing to tweak aspects of it such as
  // the use of compression, authentication, as well as to send
  // metadata back to the client.
  std::unique_ptr<grpc::ServerContext> ctx_;
  std::unique_ptr<ServerResponderType> responder_;

  // The request/response pairs associated with this state. For
  // non-streaming there will only be a single pair. For streaming
  // case can be multiple pairs since there can be many inflight
  // requests for the stream represented by this state. The mutex is
  // only needed if the state can be accessed by only a single thread
  // at a time (e.g. Infer has callback thread different from thread
  // handling GRPC request).
  std::mutex mu_;
  std::queue<std::unique_ptr<RequestResponse>> req_resp_;
};

//
// Handler
//
template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
class Handler : public GRPCServer::HandlerBase {
 public:
  Handler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      ServiceType* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count);
  virtual ~Handler();

  // Descriptive name of of the handler.
  const std::string& Name() const { return name_; }

  // Start handling requests using 'thread_cnt' threads.
  void Start(int thread_cnt);

  // Stop handling requests.
  void Stop();

 protected:
  using State = HandlerState<ServerResponderType, RequestType, ResponseType>;
  using RequestResponse = typename State::RequestResponse;

  State* StateNew()
  {
    RequestResponse* reqresp = nullptr;

    {
      std::unique_lock<std::mutex> lock(alloc_mu_);
      if (!reqresp_bucket_.empty()) {
        reqresp = reqresp_bucket_.back().release();
        reqresp_bucket_.pop_back();
      }
    }

    if (reqresp == nullptr) {
      reqresp = new RequestResponse();
    }

    return new State(server_id_, reqresp);
  }

  void StateRelease(State* state)
  {
    std::unique_lock<std::mutex> lock(alloc_mu_);

    while (!state->req_resp_.empty() &&
           (reqresp_bucket_.size() < max_reqresp_bucket_count_)) {
      std::unique_ptr<RequestResponse> rr = std::move(state->req_resp_.front());
      state->req_resp_.pop();

      rr->request_.Clear();
      rr->response_.Clear();
      reqresp_bucket_.push_back(std::move(rr));
    }

    delete state;
  }

  virtual void StartNewRequest() = 0;
  virtual Steps Process(State* state, bool rpc_ok) = 0;

  const std::string name_;
  std::shared_ptr<TRTSERVER_Server> trtserver_;
  const char* const server_id_;
  std::shared_ptr<SharedMemoryBlockManager> smb_manager_;

  ServiceType* service_;
  grpc::ServerCompletionQueue* cq_;
  std::vector<std::unique_ptr<std::thread>> threads_;

  // Mutex to serialize State allocation
  std::mutex alloc_mu_;

  // Keep some number of state objects for reuse to avoid the overhead
  // of creating a state for every new request.
  const size_t max_reqresp_bucket_count_;
  std::vector<std::unique_ptr<RequestResponse>> reqresp_bucket_;
};

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
Handler<ServiceType, ServerResponderType, RequestType, ResponseType>::Handler(
    const std::string& name, const std::shared_ptr<TRTSERVER_Server>& trtserver,
    const char* server_id,
    const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
    ServiceType* service, grpc::ServerCompletionQueue* cq,
    size_t max_reqresp_bucket_count)
    : name_(name), trtserver_(trtserver), server_id_(server_id),
      smb_manager_(smb_manager), service_(service), cq_(cq),
      max_reqresp_bucket_count_(max_reqresp_bucket_count)
{
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
Handler<ServiceType, ServerResponderType, RequestType, ResponseType>::~Handler()
{
  LOG_VERBOSE(1) << "Destructed " << Name();
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
void
Handler<ServiceType, ServerResponderType, RequestType, ResponseType>::Start(
    int thread_cnt)
{
  // Use a barrier to make sure we don't return until all threads have
  // started.
  auto barrier = std::make_shared<Barrier>(thread_cnt + 1);

  for (int t = 0; t < thread_cnt; ++t) {
    threads_.emplace_back(new std::thread([this, barrier] {
      StartNewRequest();
      barrier->Wait();

      void* tag;
      bool ok;

      while (cq_->Next(&tag, &ok)) {
        State* state = static_cast<State*>(tag);
        Steps step = Process(state, ok);
        if (step == Steps::FINISH) {
          StateRelease(state);
        }
      }
    }));
  }

  barrier->Wait();
  LOG_VERBOSE(1) << "Threads started for " << Name();
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
void
Handler<ServiceType, ServerResponderType, RequestType, ResponseType>::Stop()
{
  for (const auto& thread : threads_) {
    thread->join();
  }

  LOG_VERBOSE(1) << "Threads exited for " << Name();
}

//
// HealthHandler
//
class HealthHandler : public Handler<
                          GRPCService::AsyncService,
                          grpc::ServerAsyncResponseWriter<HealthResponse>,
                          HealthRequest, HealthResponse> {
 public:
  HealthHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);
};

void
HealthHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  HealthRequest& request = req_resp->request_;

  service_->RequestHealth(
      state->ctx_.get(), &request, state->responder_.get(), cq_, cq_, state);
}

Steps
HealthHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const HealthRequest& request = req_resp->request_;
  HealthResponse& response = req_resp->response_;

  if (state->step_ == Steps::START) {
    TRTSERVER_Error* err = nullptr;
    bool health = false;

    if (request.mode() == "live") {
      err = TRTSERVER_ServerIsLive(trtserver_.get(), &health);
    } else if (request.mode() == "ready") {
      err = TRTSERVER_ServerIsReady(trtserver_.get(), &health);
    } else {
      err = TRTSERVER_ErrorNew(
          TRTSERVER_ERROR_UNKNOWN,
          std::string("unknown health mode '" + request.mode() + "'").c_str());
    }

    response.set_health((err == nullptr) && health);

    RequestStatusUtil::Create(
        response.mutable_request_status(), err, state->unique_id_, server_id_);

    TRTSERVER_ErrorDelete(err);

    state->step_ = Steps::COMPLETE;
    state->responder_->Finish(response, grpc::Status::OK, state);
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  // Only handle one status request at a time (to avoid having status
  // request cause too much load on server), so register for next
  // request only after this one finished.
  if (!shutdown && (state->step_ == Steps::FINISH)) {
    StartNewRequest();
  }

  return state->step_;
}

//
// StatusHandler
//
class StatusHandler : public Handler<
                          GRPCService::AsyncService,
                          grpc::ServerAsyncResponseWriter<StatusResponse>,
                          StatusRequest, StatusResponse> {
 public:
  StatusHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);
};

void
StatusHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  StatusRequest& request = req_resp->request_;

  service_->RequestStatus(
      state->ctx_.get(), &request, state->responder_.get(), cq_, cq_, state);
}

Steps
StatusHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const StatusRequest& request = req_resp->request_;
  StatusResponse& response = req_resp->response_;

  if (state->step_ == Steps::START) {
    TRTSERVER_Protobuf* server_status_protobuf = nullptr;
    TRTSERVER_Error* err =
        (request.model_name().empty())
            ? TRTSERVER_ServerStatus(trtserver_.get(), &server_status_protobuf)
            : TRTSERVER_ServerModelStatus(
                  trtserver_.get(), request.model_name().c_str(),
                  &server_status_protobuf);
    if (err == nullptr) {
      const char* status_buffer;
      size_t status_byte_size;
      err = TRTSERVER_ProtobufSerialize(
          server_status_protobuf, &status_buffer, &status_byte_size);
      if (err == nullptr) {
        if (!response.mutable_server_status()->ParseFromArray(
                status_buffer, status_byte_size)) {
          err = TRTSERVER_ErrorNew(
              TRTSERVER_ERROR_UNKNOWN, "failed to parse server status");
        }
      }
    }

    TRTSERVER_ProtobufDelete(server_status_protobuf);

    RequestStatusUtil::Create(
        response.mutable_request_status(), err, state->unique_id_, server_id_);

    TRTSERVER_ErrorDelete(err);

    state->step_ = Steps::COMPLETE;
    state->responder_->Finish(response, grpc::Status::OK, state);
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  // Only handle one status request at a time (to avoid having status
  // request cause too much load on server), so register for next
  // request only after this one finished.
  if (!shutdown && (state->step_ == Steps::FINISH)) {
    StartNewRequest();
  }

  return state->step_;
}

//
// Infer utilities
//
TRTSERVER_Error*
InferResponseAlloc(
    TRTSERVER_ResponseAllocator* allocator, void** buffer, void** buffer_userp,
    const char* tensor_name, size_t byte_size,
    TRTSERVER_Memory_Type memory_type, int64_t memory_type_id, void* userp)
{
  InferResponse* response = reinterpret_cast<InferResponse*>(userp);

  *buffer = nullptr;
  *buffer_userp = nullptr;

  // Can't allocate for any memory type other than CPU. If byte size is 0,
  // proceed regardless of memory type as no allocation is required.
  if ((memory_type != TRTSERVER_MEMORY_CPU) && (byte_size != 0)) {
    LOG_VERBOSE(1) << "GRPC allocation failed for type " << memory_type
                   << " for " << tensor_name;
    return nullptr;  // Success
  }

  // Called once for each result tensor in the inference request. Must
  // always add a raw output into the response's list of outputs so
  // that the number and order of raw output entries equals the output
  // meta-data.
  std::string* raw_output = response->add_raw_output();
  if (byte_size > 0) {
    raw_output->resize(byte_size);
    *buffer = static_cast<void*>(&((*raw_output)[0]));
  }

  LOG_VERBOSE(1) << "GRPC allocation: " << tensor_name << ", size " << byte_size
                 << ", addr " << *buffer;

  return nullptr;  // Success
}

TRTSERVER_Error*
InferResponseRelease(
    TRTSERVER_ResponseAllocator* allocator, void* buffer, void* buffer_userp,
    size_t byte_size, TRTSERVER_Memory_Type memory_type, int64_t memory_type_id)
{
  LOG_VERBOSE(1) << "GRPC release: "
                 << "size " << byte_size << ", addr " << buffer;

  // Don't do anything when releasing a buffer since ResponseAlloc
  // wrote directly into the response protobuf.
  return nullptr;  // Success
}

TRTSERVER_Error*
InferGRPCToInput(
    const std::shared_ptr<TRTSERVER_Server>& trtserver,
    const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
    const InferRequestHeader& request_header, const InferRequest& request,
    TRTSERVER_InferenceRequestProvider* request_provider)
{
  // Make sure that the request is providing the same number of raw
  // + shared memory input tensor data.
  int shared_memory_input_count = 0;
  for (const auto& io : request_header.input()) {
    if (io.has_shared_memory()) {
      shared_memory_input_count++;
    }
  }
  if (request_header.input_size() !=
      (request.raw_input_size() + shared_memory_input_count)) {
    return TRTSERVER_ErrorNew(
        TRTSERVER_ERROR_INVALID_ARG,
        std::string(
            "expected tensor data for " +
            std::to_string(request_header.input_size()) + " inputs but got " +
            std::to_string(request.raw_input_size()) +
            " sets of data for model '" + request.model_name() + "'")
            .c_str());
  }

  // Verify that the batch-byte-size of each input matches the size of
  // the provided tensor data (provided raw or from shared memory)
  size_t idx = 0;
  for (const auto& io : request_header.input()) {
    const void* base;
    size_t byte_size;
    if (io.has_shared_memory()) {
      LOG_VERBOSE(1) << io.name() << " has shared memory";
      TRTSERVER_SharedMemoryBlock* smb = nullptr;
      RETURN_IF_ERR(smb_manager->Get(&smb, io.shared_memory().name()));
      RETURN_IF_ERR(TRTSERVER_ServerSharedMemoryAddress(
          trtserver.get(), smb, io.shared_memory().offset(),
          io.shared_memory().byte_size(), const_cast<void**>(&base)));
      byte_size = io.shared_memory().byte_size();
    } else {
      const std::string& raw = request.raw_input(idx++);
      base = raw.c_str();
      byte_size = raw.size();
    }

    uint64_t expected_byte_size = 0;
    RETURN_IF_ERR(TRTSERVER_InferenceRequestProviderInputBatchByteSize(
        request_provider, io.name().c_str(), &expected_byte_size));

    if (byte_size != expected_byte_size) {
      return TRTSERVER_ErrorNew(
          TRTSERVER_ERROR_INVALID_ARG,
          std::string(
              "unexpected size " + std::to_string(byte_size) + " for input '" +
              io.name() + "', expecting " + std::to_string(expected_byte_size) +
              " for model '" + request.model_name() + "'")
              .c_str());
    }

    RETURN_IF_ERR(TRTSERVER_InferenceRequestProviderSetInputData(
        request_provider, io.name().c_str(), base, byte_size));
  }

  return nullptr;  // success
}

//
// InferHandler
//
class InferHandler : public Handler<
                         GRPCService::AsyncService,
                         grpc::ServerAsyncResponseWriter<InferResponse>,
                         InferRequest, InferResponse> {
 public:
  InferHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
    // Create the allocator that will be used to allocate buffers for
    // the result tensors.
    FAIL_IF_ERR(
        TRTSERVER_ResponseAllocatorNew(
            &allocator_, InferResponseAlloc, InferResponseRelease),
        "creating response allocator");
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);

 private:
  static void InferComplete(
      TRTSERVER_Server* server, TRTSERVER_InferenceResponse* response,
      void* userp);

  TRTSERVER_ResponseAllocator* allocator_;
};

void
InferHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  InferRequest& request = req_resp->request_;

  service_->RequestInfer(
      state->ctx_.get(), &request, state->responder_.get(), cq_, cq_, state);
}

Steps
InferHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const InferRequest& request = req_resp->request_;
  InferResponse& response = req_resp->response_;

  if (state->step_ == Steps::START) {
    // Start a new request to replace this one...
    if (!shutdown) {
      StartNewRequest();
    }

    TRTSERVER_Error* err = nullptr;

    std::string request_header_serialized;
    if (!request.meta_data().SerializeToString(&request_header_serialized)) {
      err = TRTSERVER_ErrorNew(
          TRTSERVER_ERROR_UNKNOWN, "failed to serialize request header");
    } else {
      // Create the inference request provider which provides all the
      // input information needed for an inference.
      TRTSERVER_InferenceRequestProvider* request_provider = nullptr;
      err = TRTSERVER_InferenceRequestProviderNew(
          &request_provider, trtserver_.get(), request.model_name().c_str(),
          request.model_version(), request_header_serialized.c_str(),
          request_header_serialized.size());
      if (err == nullptr) {
        err = InferGRPCToInput(
            trtserver_, smb_manager_, request.meta_data(), request,
            request_provider);
        if (err == nullptr) {
          state->step_ = ISSUED;
          err = TRTSERVER_ServerInferAsync(
              trtserver_.get(), request_provider, allocator_,
              &response /* response_allocator_userp */, InferComplete,
              reinterpret_cast<void*>(state));

          // The request provider can be deleted immediately after the
          // ServerInferAsync call returns.
          TRTSERVER_InferenceRequestProviderDelete(request_provider);
        }
      }
    }

    // If not error then state->step_ == ISSUED and inference request
    // has initiated... completion callback will transition to
    // COMPLETE. If error go immediately to COMPLETE.
    if (err != nullptr) {
      RequestStatusUtil::Create(
          response.mutable_request_status(), err, state->unique_id_,
          server_id_);

      LOG_VERBOSE(1) << "Infer failed: " << TRTSERVER_ErrorMessage(err);
      TRTSERVER_ErrorDelete(err);

      // Clear the meta-data and raw output as they may be partially
      // or un-initialized.
      response.mutable_meta_data()->Clear();
      response.mutable_raw_output()->Clear();

      response.mutable_meta_data()->set_id(request.meta_data().id());

      state->step_ = COMPLETE;
      state->responder_->Finish(response, grpc::Status::OK, state);
    }
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  return state->step_;
}

void
InferHandler::InferComplete(
    TRTSERVER_Server* server, TRTSERVER_InferenceResponse* trtserver_response,
    void* userp)
{
  HandlerState<
      grpc::ServerAsyncResponseWriter<InferResponse>, InferRequest,
      InferResponse>* state =
      reinterpret_cast<HandlerState<
          grpc::ServerAsyncResponseWriter<InferResponse>, InferRequest,
          InferResponse>*>(userp);

  LOG_VERBOSE(1) << "InferHandler::InferComplete, step " << state->step_;

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const InferRequest& request = req_resp->request_;
  InferResponse& response = req_resp->response_;

  TRTSERVER_Error* response_status =
      TRTSERVER_InferenceResponseStatus(trtserver_response);
  if ((response_status == nullptr) && (response.ByteSizeLong() > INT_MAX)) {
    response_status = TRTSERVER_ErrorNew(
        TRTSERVER_ERROR_INVALID_ARG,
        std::string(
            "Response has byte size " +
            std::to_string(response.ByteSizeLong()) +
            " which exceeds gRPC's byte size limit " + std::to_string(INT_MAX) +
            ".")
            .c_str());
  }

  if (response_status == nullptr) {
    TRTSERVER_Protobuf* response_protobuf = nullptr;
    response_status = TRTSERVER_InferenceResponseHeader(
        trtserver_response, &response_protobuf);
    if (response_status == nullptr) {
      const char* buffer;
      size_t byte_size;
      response_status =
          TRTSERVER_ProtobufSerialize(response_protobuf, &buffer, &byte_size);
      if (response_status == nullptr) {
        if (!response.mutable_meta_data()->ParseFromArray(buffer, byte_size)) {
          response_status = TRTSERVER_ErrorNew(
              TRTSERVER_ERROR_INTERNAL, "failed to parse response header");
        }
      }

      TRTSERVER_ProtobufDelete(response_protobuf);
    }
  }

  // If the response is an error then clear the meta-data and raw
  // output as they may be partially or un-initialized.
  if (response_status != nullptr) {
    response.mutable_meta_data()->Clear();
    response.mutable_raw_output()->Clear();
  }

  RequestStatusUtil::Create(
      response.mutable_request_status(), response_status, state->unique_id_,
      state->server_id_);

  LOG_IF_ERR(
      TRTSERVER_InferenceResponseDelete(trtserver_response),
      "deleting GRPC response");
  TRTSERVER_ErrorDelete(response_status);

  response.mutable_meta_data()->set_id(request.meta_data().id());

  state->step_ = COMPLETE;
  state->responder_->Finish(response, grpc::Status::OK, state);
}

//
// StreamInferHandler
//
class StreamInferHandler
    : public Handler<
          GRPCService::AsyncService,
          grpc::ServerAsyncReaderWriter<InferResponse, InferRequest>,
          InferRequest, InferResponse> {
 public:
  StreamInferHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
    // Create the allocator that will be used to allocate buffers for
    // the result tensors.
    FAIL_IF_ERR(
        TRTSERVER_ResponseAllocatorNew(
            &allocator_, InferResponseAlloc, InferResponseRelease),
        "creating response allocator");
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);

 private:
  static void StreamInferComplete(
      TRTSERVER_Server* server, TRTSERVER_InferenceResponse* response,
      void* userp);

  TRTSERVER_ResponseAllocator* allocator_;
};

void
StreamInferHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  service_->RequestStreamInfer(
      state->ctx_.get(), state->responder_.get(), cq_, cq_, state);
}

Steps
StreamInferHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  InferRequest& request = req_resp->request_;
  InferResponse& response = req_resp->response_;

  if (state->step_ == Steps::START) {
    // Start a new request to replace this one...
    if (!shutdown) {
      StartNewRequest();
    }

    state->step_ = Steps::READ;
    state->responder_->Read(&request, state);

  } else if (state->step_ == Steps::READ) {
    TRTSERVER_Error* err = nullptr;

    std::string request_header_serialized;
    if (!request.meta_data().SerializeToString(&request_header_serialized)) {
      err = TRTSERVER_ErrorNew(
          TRTSERVER_ERROR_UNKNOWN, "failed to serialize request header");
    } else {
      // Create the inference request provider which provides all the
      // input information needed for an inference.
      TRTSERVER_InferenceRequestProvider* request_provider = nullptr;
      err = TRTSERVER_InferenceRequestProviderNew(
          &request_provider, trtserver_.get(), request.model_name().c_str(),
          request.model_version(), request_header_serialized.c_str(),
          request_header_serialized.size());
      if (err == nullptr) {
        err = InferGRPCToInput(
            trtserver_, smb_manager_, request.meta_data(), request,
            request_provider);
        if (err == nullptr) {
          state->step_ = ISSUED;
          err = TRTSERVER_ServerInferAsync(
              trtserver_.get(), request_provider, allocator_,
              &response /* response_allocator_userp */, StreamInferComplete,
              reinterpret_cast<void*>(state));

          // The request provider can be deleted immediately after the
          // ServerInferAsync call returns.
          TRTSERVER_InferenceRequestProviderDelete(request_provider);
        }
      }
    }

    // If not error then state->step_ == ISSUED and inference request
    // has initiated... completion callback will transition to
    // COMPLETE. If error go immediately to COMPLETE.
    if (err != nullptr) {
      RequestStatusUtil::Create(
          response.mutable_request_status(), err, state->unique_id_,
          server_id_);

      LOG_VERBOSE(1) << "Infer failed: " << TRTSERVER_ErrorMessage(err);
      TRTSERVER_ErrorDelete(err);

      // Clear the meta-data and raw output as they may be partially
      // or un-initialized.
      response.mutable_meta_data()->Clear();
      response.mutable_raw_output()->Clear();

      response.mutable_meta_data()->set_id(request.meta_data().id());

      state->step_ = Steps::WRITE;
      state->responder_->Write(response, state);
    }
  } else if (state->step_ == Steps::WRITE) {
    state->step_ = Steps::COMPLETE;
    state->responder_->Finish(grpc::Status::OK, state);
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  return state->step_;
}

void
StreamInferHandler::StreamInferComplete(
    TRTSERVER_Server* server, TRTSERVER_InferenceResponse* trtserver_response,
    void* userp)
{
  HandlerState<
      grpc::ServerAsyncReaderWriter<InferResponse, InferRequest>, InferRequest,
      InferResponse>* state =
      reinterpret_cast<HandlerState<
          grpc::ServerAsyncReaderWriter<InferResponse, InferRequest>,
          InferRequest, InferResponse>*>(userp);

  LOG_VERBOSE(1) << "StreamInferHandler::StreamInferComplete, step "
                 << state->step_;

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const InferRequest& request = req_resp->request_;
  InferResponse& response = req_resp->response_;

  TRTSERVER_Error* response_status =
      TRTSERVER_InferenceResponseStatus(trtserver_response);
  if ((response_status == nullptr) && (response.ByteSizeLong() > INT_MAX)) {
    response_status = TRTSERVER_ErrorNew(
        TRTSERVER_ERROR_INVALID_ARG,
        std::string(
            "Response has byte size " +
            std::to_string(response.ByteSizeLong()) +
            " which exceeds gRPC's byte size limit " + std::to_string(INT_MAX) +
            ".")
            .c_str());
  }

  if (response_status == nullptr) {
    TRTSERVER_Protobuf* response_protobuf = nullptr;
    response_status = TRTSERVER_InferenceResponseHeader(
        trtserver_response, &response_protobuf);
    if (response_status == nullptr) {
      const char* buffer;
      size_t byte_size;
      response_status =
          TRTSERVER_ProtobufSerialize(response_protobuf, &buffer, &byte_size);
      if (response_status == nullptr) {
        if (!response.mutable_meta_data()->ParseFromArray(buffer, byte_size)) {
          response_status = TRTSERVER_ErrorNew(
              TRTSERVER_ERROR_INTERNAL, "failed to parse response header");
        }
      }

      TRTSERVER_ProtobufDelete(response_protobuf);
    }
  }

  // If the response is an error then clear the meta-data and raw
  // output as they may be partially or un-initialized.
  if (response_status != nullptr) {
    response.mutable_meta_data()->Clear();
    response.mutable_raw_output()->Clear();
  }

  RequestStatusUtil::Create(
      response.mutable_request_status(), response_status, state->unique_id_,
      state->server_id_);

  LOG_IF_ERR(
      TRTSERVER_InferenceResponseDelete(trtserver_response),
      "deleting GRPC response");
  TRTSERVER_ErrorDelete(response_status);

  response.mutable_meta_data()->set_id(request.meta_data().id());

  state->step_ = Steps::WRITE;
  state->responder_->Write(response, state);
}

//
// ProfileHandler
//
class ProfileHandler : public Handler<
                           GRPCService::AsyncService,
                           grpc::ServerAsyncResponseWriter<ProfileResponse>,
                           ProfileRequest, ProfileResponse> {
 public:
  ProfileHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);
};

void
ProfileHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  ProfileRequest& request = req_resp->request_;

  service_->RequestProfile(
      state->ctx_.get(), &request, state->responder_.get(), cq_, cq_, state);
}

Steps
ProfileHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  ProfileResponse& response = req_resp->response_;

  if (state->step_ == START) {
    // For now profile is a nop...

    RequestStatusUtil::Create(
        response.mutable_request_status(), nullptr /* success */,
        state->unique_id_, server_id_);

    state->step_ = Steps::COMPLETE;
    state->responder_->Finish(response, grpc::Status::OK, state);
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  // Only handle one status request at a time (to avoid having status
  // request cause too much load on server), so register for next
  // request only after this one finished.
  if (!shutdown && (state->step_ == Steps::FINISH)) {
    StartNewRequest();
  }

  return state->step_;
}

//
// ModelControlHandler
//
class ModelControlHandler
    : public Handler<
          GRPCService::AsyncService,
          grpc::ServerAsyncResponseWriter<ModelControlResponse>,
          ModelControlRequest, ModelControlResponse> {
 public:
  ModelControlHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);
};

void
ModelControlHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  ModelControlRequest& request = req_resp->request_;

  service_->RequestModelControl(
      state->ctx_.get(), &request, state->responder_.get(), cq_, cq_, state);
}

Steps
ModelControlHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const ModelControlRequest& request = req_resp->request_;
  ModelControlResponse& response = req_resp->response_;

  if (state->step_ == START) {
    TRTSERVER_Error* err = nullptr;
    if (request.type() == ModelControlRequest::LOAD) {
      err = TRTSERVER_ServerLoadModel(
          trtserver_.get(), request.model_name().c_str());
    } else {
      err = TRTSERVER_ServerUnloadModel(
          trtserver_.get(), request.model_name().c_str());
    }

    RequestStatusUtil::Create(
        response.mutable_request_status(), err, state->unique_id_, server_id_);

    TRTSERVER_ErrorDelete(err);

    state->step_ = Steps::COMPLETE;
    state->responder_->Finish(response, grpc::Status::OK, state);
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  // Only handle one status request at a time (to avoid having status
  // request cause too much load on server), so register for next
  // request only after this one finished.
  if (!shutdown && (state->step_ == Steps::FINISH)) {
    StartNewRequest();
  }

  return state->step_;
}

//
// SharedMemoryControlHandler
//
class SharedMemoryControlHandler
    : public Handler<
          GRPCService::AsyncService,
          grpc::ServerAsyncResponseWriter<SharedMemoryControlResponse>,
          SharedMemoryControlRequest, SharedMemoryControlResponse> {
 public:
  SharedMemoryControlHandler(
      const std::string& name,
      const std::shared_ptr<TRTSERVER_Server>& trtserver, const char* server_id,
      const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
      GRPCService::AsyncService* service, grpc::ServerCompletionQueue* cq,
      size_t max_reqresp_bucket_count)
      : Handler(
            name, trtserver, server_id, smb_manager, service, cq,
            max_reqresp_bucket_count)
  {
  }

 protected:
  void StartNewRequest();
  Steps Process(State* state, bool rpc_ok);
};

void
SharedMemoryControlHandler::StartNewRequest()
{
  LOG_VERBOSE(1) << "New request handler for " << Name();

  State* state = StateNew();
  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  SharedMemoryControlRequest& request = req_resp->request_;

  service_->RequestSharedMemoryControl(
      state->ctx_.get(), &request, state->responder_.get(), cq_, cq_, state);
}

Steps
SharedMemoryControlHandler::Process(Handler::State* state, bool rpc_ok)
{
  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", step " << state->step_;

  // If RPC failed on a new request then the server is shutting down
  // and so we should do nothing (including not registering for a new
  // request). If RPC failed on a non-START step then there is nothing
  // we can do since we one execute one step.
  const bool shutdown = (!rpc_ok && (state->step_ == Steps::START));
  if (shutdown) {
    state->step_ = Steps::FINISH;
  }

  const std::unique_ptr<RequestResponse>& req_resp = state->req_resp_.front();
  const SharedMemoryControlRequest& request = req_resp->request_;
  SharedMemoryControlResponse& response = req_resp->response_;

  if (state->step_ == START) {
    TRTSERVER_SharedMemoryBlock* smb = nullptr;

    TRTSERVER_Error* err = nullptr;
    switch (request.type()) {
      case SharedMemoryControlRequest::REGISTER:
        err = smb_manager_->Create(
            &smb, request.shared_memory_region().name(),
            request.shared_memory_region().shm_key(),
            request.shared_memory_region().offset(),
            request.shared_memory_region().byte_size());
        if (err == nullptr) {
          err = TRTSERVER_ServerRegisterSharedMemory(trtserver_.get(), smb);
        }
        break;
      case SharedMemoryControlRequest::UNREGISTER:
        err = smb_manager_->Remove(&smb, request.shared_memory_region().name());
        if ((err == nullptr) && (smb != nullptr)) {
          err = TRTSERVER_ServerUnregisterSharedMemory(trtserver_.get(), smb);
          TRTSERVER_Error* del_err = TRTSERVER_SharedMemoryBlockDelete(smb);
          if (del_err != nullptr) {
            LOG_ERROR << "failed to delete shared memory block: "
                      << TRTSERVER_ErrorMessage(del_err);
          }
        }
        break;
      case SharedMemoryControlRequest::UNREGISTER_ALL:
        err = smb_manager_->Clear();
        if (err == nullptr) {
          err = TRTSERVER_ServerUnregisterAllSharedMemory(trtserver_.get());
        }
        break;
      default:
        err = TRTSERVER_ErrorNew(
            TRTSERVER_ERROR_UNKNOWN,
            "unknown sharedmemorycontrol request type");
        break;
    }

    RequestStatusUtil::Create(
        response.mutable_request_status(), err, state->unique_id_, server_id_);

    TRTSERVER_ErrorDelete(err);

    state->step_ = Steps::COMPLETE;
    state->responder_->Finish(response, grpc::Status::OK, state);
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  }

  // Only handle one status request at a time (to avoid having status
  // request cause too much load on server), so register for next
  // request only after this one finished.
  if (!shutdown && (state->step_ == Steps::FINISH)) {
    StartNewRequest();
  }

  return state->step_;
}

}  // namespace

//
// GRPCServer
//
GRPCServer::GRPCServer(
    const std::shared_ptr<TRTSERVER_Server>& server,
    const std::shared_ptr<SharedMemoryBlockManager>& smb_manager,
    const char* server_id, const std::string& server_addr,
    const int infer_thread_cnt, const int stream_infer_thread_cnt)
    : server_(server), smb_manager_(smb_manager), server_id_(server_id),
      server_addr_(server_addr), infer_thread_cnt_(infer_thread_cnt),
      stream_infer_thread_cnt_(stream_infer_thread_cnt), running_(false)
{
}

GRPCServer::~GRPCServer()
{
  Stop();
}

TRTSERVER_Error*
GRPCServer::Create(
    const std::shared_ptr<TRTSERVER_Server>& server,
    const std::shared_ptr<SharedMemoryBlockManager>& smb_manager, int32_t port,
    int infer_thread_cnt, int stream_infer_thread_cnt,
    std::unique_ptr<GRPCServer>* grpc_server)
{
  const char* server_id = nullptr;
  TRTSERVER_Error* err = TRTSERVER_ServerId(server.get(), &server_id);
  if (err != nullptr) {
    server_id = "unknown:0";
    TRTSERVER_ErrorDelete(err);
  }

  const std::string addr = "0.0.0.0:" + std::to_string(port);
  grpc_server->reset(new GRPCServer(
      server, smb_manager, server_id, addr, infer_thread_cnt,
      stream_infer_thread_cnt));

  return nullptr;  // success
}

TRTSERVER_Error*
GRPCServer::Start()
{
  if (running_) {
    return TRTSERVER_ErrorNew(
        TRTSERVER_ERROR_ALREADY_EXISTS, "GRPC server is already running.");
  }

  grpc_builder_.AddListeningPort(
      server_addr_, grpc::InsecureServerCredentials());
  grpc_builder_.SetMaxMessageSize(MAX_GRPC_MESSAGE_SIZE);
  grpc_builder_.RegisterService(&service_);
  health_cq_ = grpc_builder_.AddCompletionQueue();
  status_cq_ = grpc_builder_.AddCompletionQueue();
  infer_cq_ = grpc_builder_.AddCompletionQueue();
  stream_infer_cq_ = grpc_builder_.AddCompletionQueue();
  profile_cq_ = grpc_builder_.AddCompletionQueue();
  modelcontrol_cq_ = grpc_builder_.AddCompletionQueue();
  shmcontrol_cq_ = grpc_builder_.AddCompletionQueue();
  grpc_server_ = grpc_builder_.BuildAndStart();

  // Handler for health requests. A single thread processes all of
  // these requests.
  HealthHandler* hhealth = new HealthHandler(
      "HealthHandler", server_, server_id_, smb_manager_, &service_,
      health_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hhealth->Start(1 /* thread_cnt */);
  health_handler_.reset(hhealth);

  // Handler for status requests. A single thread processes all of
  // these requests.
  StatusHandler* hstatus = new StatusHandler(
      "StatusHandler", server_, server_id_, smb_manager_, &service_,
      status_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hstatus->Start(1 /* thread_cnt */);
  status_handler_.reset(hstatus);

  // Handler for inference requests.
  InferHandler* hinfer = new InferHandler(
      "InferHandler", server_, server_id_, smb_manager_, &service_,
      infer_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hinfer->Start(infer_thread_cnt_);
  infer_handler_.reset(hinfer);

  // Handler for streaming inference requests.
  StreamInferHandler* hstreaminfer = new StreamInferHandler(
      "StreamInferHandler", server_, server_id_, smb_manager_, &service_,
      stream_infer_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hstreaminfer->Start(stream_infer_thread_cnt_);
  stream_infer_handler_.reset(hstreaminfer);

  // Handler for profile requests. A single thread processes all of
  // these requests.
  ProfileHandler* hprofile = new ProfileHandler(
      "ProfileHandler", server_, server_id_, smb_manager_, &service_,
      profile_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hprofile->Start(1 /* thread_cnt */);
  profile_handler_.reset(hprofile);

  // Handler for model-control requests. A single thread processes all
  // of these requests.
  ModelControlHandler* hmodelcontrol = new ModelControlHandler(
      "ModelControlHandler", server_, server_id_, smb_manager_, &service_,
      modelcontrol_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hmodelcontrol->Start(1 /* thread_cnt */);
  modelcontrol_handler_.reset(hmodelcontrol);

  // Handler for shared-memory-control requests. A single thread
  // processes all of these requests.
  SharedMemoryControlHandler* hshmcontrol = new SharedMemoryControlHandler(
      "SharedMemoryControlHandler", server_, server_id_, smb_manager_,
      &service_, shmcontrol_cq_.get(), 1 /* max_reqresp_bucket_count */);
  hshmcontrol->Start(1 /* thread_cnt */);
  shmcontrol_handler_.reset(hshmcontrol);

  running_ = true;
  LOG_INFO << "Started GRPCService at " << server_addr_;
  return nullptr;  // success
}

TRTSERVER_Error*
GRPCServer::Stop()
{
  if (!running_) {
    return TRTSERVER_ErrorNew(
        TRTSERVER_ERROR_UNAVAILABLE, "GRPC server is not running.");
  }

  // Always shutdown the completion queue after the server.
  grpc_server_->Shutdown();

  health_cq_->Shutdown();
  status_cq_->Shutdown();
  infer_cq_->Shutdown();
  stream_infer_cq_->Shutdown();
  profile_cq_->Shutdown();
  modelcontrol_cq_->Shutdown();
  shmcontrol_cq_->Shutdown();

  // Must stop all handlers explicitly to wait for all the handler
  // threads to join since they are referencing completion queue, etc.
  dynamic_cast<HealthHandler*>(health_handler_.get())->Stop();
  dynamic_cast<StatusHandler*>(status_handler_.get())->Stop();
  dynamic_cast<InferHandler*>(infer_handler_.get())->Stop();
  dynamic_cast<StreamInferHandler*>(stream_infer_handler_.get())->Stop();
  dynamic_cast<ProfileHandler*>(profile_handler_.get())->Stop();
  dynamic_cast<ModelControlHandler*>(modelcontrol_handler_.get())->Stop();
  dynamic_cast<SharedMemoryControlHandler*>(shmcontrol_handler_.get())->Stop();

  running_ = false;
  return nullptr;  // success
}

}}  // namespace nvidia::inferenceserver
