// Phase 0 smoke test: proves the proto library compiles, links, and round-trips.
// Real unit tests arrive with Phase 1 (chunking, ring, replication).

#include <gtest/gtest.h>

#include "common.pb.h"

TEST(Proto, StatusRoundTrips) {
  atlas::Status s;
  s.set_code(atlas::Status::OK);
  s.set_message("ok");
  EXPECT_EQ(s.code(), atlas::Status::OK);
  EXPECT_EQ(s.message(), "ok");
}

TEST(Proto, ChunkPlacementHoldsReplicas) {
  atlas::ChunkPlacement p;
  p.mutable_chunk()->set_chunk_id("deadbeef");
  p.mutable_chunk()->set_size(4u * 1024u * 1024u);
  p.add_node_ids("storage-1");  // primary
  p.add_node_ids("storage-2");  // secondary
  p.add_node_ids("storage-3");  // tertiary
  EXPECT_EQ(p.node_ids_size(), 3);
  EXPECT_EQ(p.node_ids(0), "storage-1");
  EXPECT_EQ(p.chunk().size(), 4u * 1024u * 1024u);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, &argv);
  return RUN_ALL_TESTS();
}
