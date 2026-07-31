#include "metadata/metadata_service.h"

#include <utility>

namespace atlas {

grpc::Status MetadataServiceImpl::RegisterFile(grpc::ServerContext* /*context*/,
                                               const RegisterFileRequest* request,
                                               FileMetadata* response) {
  FileMetadata meta;
  meta.set_file_id(request->file_id());
  meta.set_owner(request->owner());
  meta.mutable_chunks()->CopyFrom(request->chunks());
  meta.set_replication_factor(request->replication_factor());
  meta.set_sha256(request->sha256());

  const std::lock_guard<std::mutex> lock(mutex_);
  *response = store_->RegisterFile(std::move(meta));  // assigns version + timestamps
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::GetFile(grpc::ServerContext* /*context*/,
                                          const GetFileRequest* request, FileMetadata* response) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!store_->GetFile(request->file_id(), request->version(), response)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "no such file or version");
  }
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::ListVersions(grpc::ServerContext* /*context*/,
                                               const ListVersionsRequest* request,
                                               ListVersionsResponse* response) {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const FileMetadata& version : store_->ListVersions(request->file_id())) {
    *response->add_versions() = version;
  }
  return grpc::Status::OK;
}

void MetadataServiceImpl::FillRingState(RingState* out) const {
  out->set_version(ring_version_);
  out->set_virtual_nodes_per_node(static_cast<std::uint32_t>(vnodes_));
  for (const auto& [id, info] : nodes_) *out->add_nodes() = info;
}

grpc::Status MetadataServiceImpl::GetRing(grpc::ServerContext* /*context*/,
                                          const GetRingRequest* /*request*/, RingState* response) {
  const std::lock_guard<std::mutex> lock(mutex_);
  FillRingState(response);
  return grpc::Status::OK;
}

grpc::Status MetadataServiceImpl::UpdateMembership(grpc::ServerContext* /*context*/,
                                                   const UpdateMembershipRequest* request,
                                                   RingState* response) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const std::string& id = request->node().node_id();
  if (request->change() == UpdateMembershipRequest::LEAVE) {
    ring_.RemoveNode(id);
    nodes_.erase(id);
  } else {  // JOIN (CHANGE_UNSPECIFIED is treated as a join)
    ring_.AddNode(id);
    nodes_[id] = request->node();
  }
  ++ring_version_;
  FillRingState(response);
  return grpc::Status::OK;
}

RingState MetadataServiceImpl::SnapshotRing() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  RingState state;
  FillRingState(&state);
  return state;
}

grpc::Status MetadataServiceImpl::ReportFailure(grpc::ServerContext* /*context*/,
                                                const FailureReport* /*request*/,
                                                Status* response) {
  // M1 records the report and acks; acting on it (replica promotion + re-replication) is Phase 2.
  response->set_code(Status::OK);
  return grpc::Status::OK;
}

}  // namespace atlas
