#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "common/hash_ring.h"
#include "metadata.grpc.pb.h"
#include "metadata/metadata_store.h"

namespace atlas {

// gRPC MetadataService — the authoritative control plane (single node for M1, ADR-0005). Owns the
// versioned file map (MetadataStore) and the consistent-hashing ring / cluster membership.
// Does not own the MetadataStore.
class MetadataServiceImpl final : public MetadataService::Service {
 public:
  explicit MetadataServiceImpl(MetadataStore* store, int vnodes_per_node = 128)
      : store_(store), ring_(vnodes_per_node), vnodes_(vnodes_per_node) {}

  grpc::Status RegisterFile(grpc::ServerContext* context, const RegisterFileRequest* request,
                            FileMetadata* response) override;
  grpc::Status GetFile(grpc::ServerContext* context, const GetFileRequest* request,
                       FileMetadata* response) override;
  grpc::Status ListVersions(grpc::ServerContext* context, const ListVersionsRequest* request,
                            ListVersionsResponse* response) override;
  grpc::Status GetRing(grpc::ServerContext* context, const GetRingRequest* request,
                       RingState* response) override;
  grpc::Status UpdateMembership(grpc::ServerContext* context,
                                const UpdateMembershipRequest* request,
                                RingState* response) override;
  grpc::Status ReportFailure(grpc::ServerContext* context, const FailureReport* request,
                             Status* response) override;

 private:
  void FillRingState(RingState* out) const;  // caller must hold mutex_

  MetadataStore* store_;
  mutable std::mutex mutex_;  // serializes the store + ring (single-threaded control plane for M1)
  HashRing ring_;
  int vnodes_;
  std::uint64_t ring_version_ = 0;
  std::unordered_map<std::string, NodeInfo> nodes_;  // node_id -> address/roles, for GetRing
};

}  // namespace atlas
