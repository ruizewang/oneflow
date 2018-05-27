#include "oneflow/core/control/ctrl_server.h"
#include "oneflow/core/actor/act_event_logger.h"
#include "oneflow/core/job/profiler.h"
#include "grpc/grpc_posix.h"

DEFINE_bool(grpc_use_no_signal, false, "prevent GRPC library from using any signals");

namespace oneflow {

namespace {

int ExtractPortFromAddr(const std::string& addr) {
  size_t pos = addr.find(':');
  return oneflow_cast<int>(addr.substr(pos + 1));
}

}  // namespace

CtrlServer::~CtrlServer() {
  grpc::Alarm alarm(cq_.get(), gpr_now(GPR_CLOCK_MONOTONIC), nullptr);
  loop_thread_.join();
  grpc_server_->Shutdown();
  cq_->Shutdown();
}

CtrlServer::CtrlServer(const std::string& server_addr) {
  init(&CtrlServer::LoadServerHandler, &CtrlServer::BarrierHandler, &CtrlServer::TryLockHandler,
       &CtrlServer::NotifyDoneHandler, &CtrlServer::WaitUntilDoneHandler,
       &CtrlServer::PushKVHandler, &CtrlServer::ClearKVHandler, &CtrlServer::PullKVHandler,
       &CtrlServer::PushActEventHandler, &CtrlServer::ClearHandler,
       &CtrlServer::IncreaseCountHandler, &CtrlServer::EraseCountHandler,
       &CtrlServer::PushAvgActIntervalHandler);

  if (FLAGS_grpc_use_no_signal) { grpc_use_signal(-1); }
  int port = ExtractPortFromAddr(server_addr);
  grpc::ServerBuilder server_builder;
  int bound_port = 0;
  server_builder.AddListeningPort(server_addr, grpc::InsecureServerCredentials(), &bound_port);
  grpc_service_.reset(new CtrlService::AsyncService);
  server_builder.RegisterService(grpc_service_.get());
  cq_ = server_builder.AddCompletionQueue();
  grpc_server_ = server_builder.BuildAndStart();
  CHECK_EQ(port, bound_port) << "Port " << port << " is unavailable";
  LOG(INFO) << "CtrlServer listening on " << server_addr;
  loop_thread_ = std::thread(&CtrlServer::HandleRpcs, this);
}

void CtrlServer::HandleRpcs() {
  EnqueueRequests(arr_);
  void* tag = nullptr;
  bool ok = false;
  while (true) {
    CHECK(cq_->Next(&tag, &ok));
    CHECK(ok);
    auto call = static_cast<CtrlCallIf*>(tag);
    if (call) {
      call->Process();
    } else {
      break;
    }
  }
}

template<size_t I>
void CtrlServer::EnqueueRequest() {
  using req = typename std::tuple_element<I, RequestType>::type;
  using res = typename std::tuple_element<I, ResponseType>::type;
  typedef void (CtrlServer::*TMember)(CtrlCall<req, res>*);
  auto thandler = reinterpret_cast<TMember>(arr_[I]);
  auto call = new CtrlCall<req, res>();
  call->set_request_handler(std::bind(thandler, this, call));
  grpc_service_->RequestAsyncUnary(I, call->mut_server_ctx(), call->mut_request(),
                                   call->mut_responder(), cq_.get(), cq_.get(), call);
}

void CtrlServer::LoadServerHandler(CtrlCall<LoadServerRequest, LoadServerResponse>* call) {
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kLoadServer>();
}

void CtrlServer::BarrierHandler(CtrlCall<BarrierRequest, BarrierResponse>* call) {
  const std::string& barrier_name = call->request().name();
  int32_t barrier_num = call->request().num();
  auto barrier_call_it = barrier_calls_.find(barrier_name);
  if (barrier_call_it == barrier_calls_.end()) {
    barrier_call_it =
        barrier_calls_.emplace(barrier_name, std::make_pair(std::list<CtrlCallIf*>{}, barrier_num))
            .first;
  }
  CHECK_EQ(barrier_num, barrier_call_it->second.second);
  barrier_call_it->second.first.push_back(call);
  if (barrier_call_it->second.first.size() == barrier_call_it->second.second) {
    for (CtrlCallIf* pending_call : barrier_call_it->second.first) { pending_call->SendResponse(); }
    barrier_calls_.erase(barrier_call_it);
  }

  EnqueueRequest<CtrlMethod::kBarrier>();
}

void CtrlServer::TryLockHandler(CtrlCall<TryLockRequest, TryLockResponse>* call) {
  const std::string& lock_name = call->request().name();
  auto name2lock_status_it = name2lock_status_.find(lock_name);
  if (name2lock_status_it == name2lock_status_.end()) {
    call->mut_response()->set_result(TryLockResult::kLocked);
    auto waiting_until_done_calls = new std::list<CtrlCallIf*>;
    CHECK(name2lock_status_.emplace(lock_name, waiting_until_done_calls).second);
  } else {
    if (name2lock_status_it->second) {
      call->mut_response()->set_result(TryLockResult::kDoing);
    } else {
      call->mut_response()->set_result(TryLockResult::kDone);
    }
  }
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kTryLock>();
}

void CtrlServer::NotifyDoneHandler(CtrlCall<NotifyDoneRequest, NotifyDoneResponse>* call) {
  const std::string& lock_name = call->request().name();
  auto name2lock_status_it = name2lock_status_.find(lock_name);
  auto waiting_calls = static_cast<std::list<CtrlCallIf*>*>(name2lock_status_it->second);
  for (CtrlCallIf* waiting_call : *waiting_calls) { waiting_call->SendResponse(); }
  delete waiting_calls;
  name2lock_status_it->second = nullptr;
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kNotifyDone>();
}

void CtrlServer::WaitUntilDoneHandler(CtrlCall<WaitUntilDoneRequest, WaitUntilDoneResponse>* call) {
  const std::string& lock_name = call->request().name();
  void* lock_status = name2lock_status_.at(lock_name);
  if (lock_status) {
    auto waiting_calls = static_cast<std::list<CtrlCallIf*>*>(lock_status);
    waiting_calls->push_back(call);
  } else {
    call->SendResponse();
  }
  EnqueueRequest<CtrlMethod::kWaitUntilDone>();
}

void CtrlServer::PushKVHandler(CtrlCall<PushKVRequest, PushKVResponse>* call) {
  const std::string& k = call->request().key();
  const std::string& v = call->request().val();
  CHECK(kv_.emplace(k, v).second);

  auto pending_kv_calls_it = pending_kv_calls_.find(k);
  if (pending_kv_calls_it != pending_kv_calls_.end()) {
    for (auto pending_call : pending_kv_calls_it->second) {
      pending_call->mut_response()->set_val(v);
      pending_call->SendResponse();
    }
    pending_kv_calls_.erase(pending_kv_calls_it);
  }
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kPushKV>();
}

void CtrlServer::ClearKVHandler(CtrlCall<ClearKVRequest, ClearKVResponse>* call) {
  const std::string& k = call->request().key();
  CHECK_EQ(kv_.erase(k), 1);
  CHECK(pending_kv_calls_.find(k) == pending_kv_calls_.end());
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kClearKV>();
}

void CtrlServer::PullKVHandler(CtrlCall<PullKVRequest, PullKVResponse>* call) {
  const std::string& k = call->request().key();
  auto kv_it = kv_.find(k);
  if (kv_it != kv_.end()) {
    call->mut_response()->set_val(kv_it->second);
    call->SendResponse();
  } else {
    pending_kv_calls_[k].push_back(call);
  }
  EnqueueRequest<CtrlMethod::kPullKV>();
}

void CtrlServer::PushActEventHandler(CtrlCall<PushActEventRequest, PushActEventResponse>* call) {
  ActEvent act_event = call->request().act_event();
  call->SendResponse();
  Global<ActEventLogger>::Get()->PrintActEventToLogDir(act_event);
  EnqueueRequest<CtrlMethod::kPushActEvent>();
}

void CtrlServer::ClearHandler(CtrlCall<ClearRequest, ClearResponse>* call) {
  name2lock_status_.clear();
  kv_.clear();
  CHECK(pending_kv_calls_.empty());
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kClear>();
}

void CtrlServer::IncreaseCountHandler(CtrlCall<IncreaseCountRequest, IncreaseCountResponse>* call) {
  int32_t& count = count_[call->request().key()];
  count += call->request().val();
  call->mut_response()->set_val(count);
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kIncreaseCount>();
}

void CtrlServer::EraseCountHandler(CtrlCall<EraseCountRequest, EraseCountResponse>* call) {
  CHECK_EQ(count_.erase(call->request().key()), 1);
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kEraseCount>();
}

void CtrlServer::PushAvgActIntervalHandler(
    CtrlCall<PushAvgActIntervalRequest, PushAvgActIntervalResponse>* call) {
  Global<Profiler>::Get()->PushAvgActInterval(call->request().actor_id(),
                                              call->request().avg_act_interval());
  call->SendResponse();
  EnqueueRequest<CtrlMethod::kPushAvgActInterval>();
}

}  // namespace oneflow
