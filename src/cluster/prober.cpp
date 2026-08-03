#include "cluster/prober.h"

namespace atlas {

StorageService::Stub* Prober::StubFor(const std::string& address) {
  auto it = stubs_.find(address);
  if (it == stubs_.end()) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    it = stubs_.emplace(address, StorageService::NewStub(channel)).first;
  }
  return it->second.get();
}

int Prober::ProbeOnce(const std::vector<ProbeTarget>& targets) {
  int answered = 0;
  for (const ProbeTarget& target : targets) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
    HeartbeatRequest request;
    request.set_from_node_id("metadata");
    HeartbeatResponse response;

    if (StubFor(target.address)->Heartbeat(&ctx, request, &response).ok()) {
      tracker_->RecordSuccess(target.node_id);
      ++answered;
    } else {
      tracker_->RecordFailure(target.node_id);
    }
  }
  return answered;
}

}  // namespace atlas
