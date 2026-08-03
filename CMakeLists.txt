cmake_minimum_required(VERSION 3.24)
project(atlas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Serialize linking (Ninja job pool). Every executable here statically links gRPC + protobuf +
# RocksDB, and a Debug link of one of those peaks well over a gigabyte. Ninja otherwise runs
# `cores + 2` links at once, which is what OOM-killed the CI runner once the Phase 2 and Phase 3
# targets landed in the same tree (14 executables). Compilation stays fully parallel — this
# throttles the link step only, and costs a few seconds locally.
set(ATLAS_LINK_JOBS "1" CACHE STRING "Concurrent link jobs (Ninja only); raise if you have RAM")
if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL PROPERTY JOB_POOLS atlas_link=${ATLAS_LINK_JOBS})
  set(CMAKE_JOB_POOL_LINK atlas_link)
endif()

# Sanitizers (ADR-0001 mitigation). Opt-in, and selectable — because AddressSanitizer hangs in
# dyld's initializers before main() on macOS 26 / Apple clang 17 (reproducible with an empty
# main), so a macOS dev needs UBSan without ASan:
#   -DATLAS_SANITIZE=ON         -> address,undefined  (Linux CI / Docker)
#   -DATLAS_SANITIZE=undefined  -> UBSan only         (macOS)
#   -DATLAS_SANITIZE=OFF        -> default
set(ATLAS_SANITIZE "OFF" CACHE STRING
    "Sanitizers: OFF, ON (=address,undefined), or an explicit -fsanitize list")
if(ATLAS_SANITIZE)
  if(ATLAS_SANITIZE STREQUAL "ON")
    set(atlas_sanitizers "address,undefined")
  else()
    set(atlas_sanitizers "${ATLAS_SANITIZE}")
  endif()
  message(STATUS "Atlas: sanitizers enabled -> ${atlas_sanitizers}")
  add_compile_options(-fsanitize=${atlas_sanitizers} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${atlas_sanitizers})
endif()

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(RocksDB CONFIG REQUIRED)

# ---- generated gRPC/protobuf stubs from proto/ (the co-designed contracts) ----
set(ATLAS_PROTOS
  proto/common.proto
  proto/storage.proto
  proto/metadata.proto
  proto/search.proto
  proto/coordinator.proto
)

add_library(atlas_proto STATIC ${ATLAS_PROTOS})
target_link_libraries(atlas_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_include_directories(atlas_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

protobuf_generate(
  TARGET atlas_proto
  LANGUAGE cpp
  IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

protobuf_generate(
  TARGET atlas_proto
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# ============================ shared primitives ============================

add_library(atlas_common STATIC src/common/sha256.cpp src/common/hash_ring.cpp)
target_include_directories(atlas_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---- shared query parser ----
# Not part of atlas_search on purpose: Phase 4's coordinator parses a query once and ships the
# tree to every shard, so parsing must not live inside the shard. Zero dependencies — the term
# analyzer is injected by the caller.
add_library(atlas_query STATIC src/common/query/parser.cpp)
target_include_directories(atlas_query PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ======================= Phase 1: distributed file system =======================

add_library(atlas_dfs STATIC src/dfs/chunking.cpp)
target_link_libraries(atlas_dfs PUBLIC atlas_common)

add_library(atlas_storage STATIC src/storage/chunk_store.cpp)
target_link_libraries(atlas_storage PUBLIC atlas_common RocksDB::rocksdb)

add_library(atlas_storage_svc STATIC src/storage/storage_service.cpp)
target_link_libraries(atlas_storage_svc PUBLIC atlas_storage atlas_proto gRPC::grpc++)

# Chunk streaming over StorageService, shared by the ingestion client and the healer.
add_library(atlas_chunk_transfer STATIC src/storage/chunk_transfer.cpp)
target_include_directories(atlas_chunk_transfer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(atlas_chunk_transfer PUBLIC atlas_proto gRPC::grpc++)

add_library(atlas_metadata STATIC src/metadata/metadata_store.cpp)
target_include_directories(atlas_metadata PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(atlas_metadata PUBLIC atlas_proto RocksDB::rocksdb)

add_library(atlas_metadata_svc STATIC src/metadata/metadata_service.cpp)
target_link_libraries(atlas_metadata_svc PUBLIC atlas_metadata atlas_common gRPC::grpc++)

# ---- Phase 2: cluster health (failure detection) + self-healing ----
add_library(atlas_cluster STATIC
  src/cluster/health_tracker.cpp
  src/cluster/prober.cpp
  src/cluster/healer.cpp
  src/cluster/maintenance.cpp
)
target_link_libraries(atlas_cluster
  PUBLIC atlas_common atlas_proto atlas_metadata atlas_chunk_transfer gRPC::grpc++)

# ---- ingestion / read client: chunk -> replicate via ring -> register; read-around on download ----
add_library(atlas_client STATIC src/client/client.cpp)
target_link_libraries(atlas_client PUBLIC atlas_dfs atlas_chunk_transfer atlas_proto gRPC::grpc++)
target_include_directories(atlas_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# =========================== Phase 3: search engine ===========================

# Deliberately independent of atlas_proto: the index/ranking core is testable against a local
# corpus, so it doesn't wait on the storage track (see docs/plan/phase-3-kickoff.md).
add_library(atlas_search STATIC
  src/search/tokenizer.cpp
  src/search/stopwords.cpp
  src/search/stemmer.cpp
  src/search/text_pipeline.cpp
  src/search/inverted_index.cpp
  src/search/postings.cpp
  src/search/posting_codec.cpp
  src/search/ranker.cpp
  src/search/trie.cpp
  src/search/bk_tree.cpp
  src/search/search_engine.cpp
  src/search/corpus.cpp
)
target_include_directories(atlas_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(atlas_search PUBLIC atlas_query)

# The gRPC adapter is a separate target so atlas_search itself stays proto-free.
add_library(atlas_search_service STATIC src/search/search_service.cpp)
target_link_libraries(atlas_search_service PUBLIC atlas_search atlas_proto gRPC::grpc++)

# ====================== Phase 4: distributed query engine ======================

# Header-only: LRU/LFU caches and the thread/connection pools are templates. An INTERFACE target
# carries the include path and dependencies without producing an archive nothing would link.
add_library(atlas_concurrency INTERFACE)
target_include_directories(atlas_concurrency INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(atlas_concurrency INTERFACE gRPC::grpc++)

# Fan-out + merge + result cache. Depends on atlas_search only for the query analyzer, which the
# coordinator needs to ask every shard about the same term set.
add_library(atlas_coordinator STATIC src/coordinator/coordinator.cpp)
target_link_libraries(atlas_coordinator
  PUBLIC atlas_concurrency atlas_search atlas_proto gRPC::grpc++)

# gRPC adapter kept separate, mirroring atlas_search / atlas_search_service.
add_library(atlas_coordinator_service STATIC src/coordinator/coordinator_service.cpp)
target_link_libraries(atlas_coordinator_service PUBLIC atlas_coordinator atlas_proto gRPC::grpc++)

# ============================== executables ==============================

# One binary, two roles (ATLAS_ROLE=storage|metadata). A storage node also serves this shard's
# search service (Phase 3); a metadata node runs the control plane + the maintenance loop.
add_executable(atlas_node src/node/main.cpp)
# atlas_storage_svc / atlas_search_service / atlas_cluster already propagate atlas_proto and
# gRPC::grpc++ publicly; naming them again here makes the linker warn about duplicates.
target_link_libraries(atlas_node PRIVATE atlas_storage_svc atlas_search_service
                                         atlas_metadata_svc atlas_cluster
                                         atlas_coordinator_service
                                         gRPC::grpc++_reflection)

# ---- atlas: command-line client (put / get / info / nodes) ----
add_executable(atlas_cli tools/atlas_cli.cpp)
set_target_properties(atlas_cli PROPERTIES OUTPUT_NAME atlas)
target_link_libraries(atlas_cli PRIVATE atlas_client gRPC::grpc++)

# ---- Phase 3 demo: index a corpus, run ranked queries, print timings ----
add_executable(atlas_search_demo tools/search_demo.cpp)
target_link_libraries(atlas_search_demo PRIVATE atlas_search)

# ================================= tests =================================

enable_testing()

add_executable(atlas_tests tests/smoke_test.cpp)
target_link_libraries(atlas_tests PRIVATE atlas_proto GTest::gtest)
add_test(NAME atlas_tests COMMAND atlas_tests)

# Dependency-free (no gRPC/GTest): builds and runs even without the vcpkg deps.
add_executable(chunking_test tests/chunking_test.cpp)
target_link_libraries(chunking_test PRIVATE atlas_dfs)
add_test(NAME chunking_test COMMAND chunking_test)

add_executable(hash_ring_test tests/hash_ring_test.cpp)
target_link_libraries(hash_ring_test PRIVATE atlas_common)
add_test(NAME hash_ring_test COMMAND hash_ring_test)

add_executable(chunk_store_test tests/chunk_store_test.cpp)
target_link_libraries(chunk_store_test PRIVATE atlas_storage RocksDB::rocksdb)
add_test(NAME chunk_store_test COMMAND chunk_store_test)

add_executable(storage_service_test tests/storage_service_test.cpp)
target_link_libraries(storage_service_test PRIVATE atlas_storage_svc gRPC::grpc++)
add_test(NAME storage_service_test COMMAND storage_service_test)

add_executable(metadata_store_test tests/metadata_store_test.cpp)
target_link_libraries(metadata_store_test PRIVATE atlas_metadata RocksDB::rocksdb)
add_test(NAME metadata_store_test COMMAND metadata_store_test)

add_executable(metadata_service_test tests/metadata_service_test.cpp)
target_link_libraries(metadata_service_test PRIVATE atlas_metadata_svc gRPC::grpc++)
add_test(NAME metadata_service_test COMMAND metadata_service_test)

add_executable(health_tracker_test tests/health_tracker_test.cpp)
target_link_libraries(health_tracker_test PRIVATE atlas_cluster atlas_storage_svc gRPC::grpc++)
add_test(NAME health_tracker_test COMMAND health_tracker_test)

# Node-join migration: adding a node moves only its share (the Phase 1 DoD's last clause).
add_executable(rebalance_test tests/rebalance_test.cpp)
target_link_libraries(rebalance_test
  PRIVATE atlas_cluster atlas_client atlas_storage_svc atlas_metadata_svc gRPC::grpc++)
add_test(NAME rebalance_test COMMAND rebalance_test)

add_executable(maintenance_test tests/maintenance_test.cpp)
target_link_libraries(maintenance_test
  PRIVATE atlas_cluster atlas_client atlas_storage_svc atlas_metadata_svc gRPC::grpc++)
add_test(NAME maintenance_test COMMAND maintenance_test)

# Phase 2 headline: kill a node, watch the cluster restore the replication factor.
add_executable(self_healing_test tests/self_healing_test.cpp)
target_link_libraries(self_healing_test
  PRIVATE atlas_cluster atlas_client atlas_storage_svc atlas_metadata_svc gRPC::grpc++)
add_test(NAME self_healing_test COMMAND self_healing_test)

# End-to-end Milestone-1 test: metadata + storage nodes + the ingestion client, all in-process.
add_executable(phase1_e2e_test tests/phase1_e2e_test.cpp)
target_link_libraries(phase1_e2e_test PRIVATE atlas_client atlas_storage_svc atlas_metadata_svc
                                              gRPC::grpc++)
add_test(NAME phase1_e2e_test COMMAND phase1_e2e_test)

# ---- Phase 4: distributed query engine ----

add_executable(cache_test tests/cache_test.cpp)
target_link_libraries(cache_test PRIVATE atlas_concurrency)
add_test(NAME cache_test COMMAND cache_test)

add_executable(pool_test tests/pool_test.cpp)
target_link_libraries(pool_test PRIVATE atlas_concurrency atlas_search_service gRPC::grpc++)
add_test(NAME pool_test COMMAND pool_test)

# Phase 4 headline: fan a query across 4 real shards, merge, cache, survive a shard loss.
add_executable(coordinator_test tests/coordinator_test.cpp)
target_link_libraries(coordinator_test
  PRIVATE atlas_coordinator atlas_search_service gRPC::grpc++)
add_test(NAME coordinator_test COMMAND coordinator_test)

# End-to-end: real cluster, `atlas put` indexes, coordinator finds it across shards.
add_executable(phase4_e2e_test tests/phase4_e2e_test.cpp)
target_link_libraries(phase4_e2e_test
  PRIVATE atlas_coordinator_service atlas_client atlas_storage_svc atlas_metadata_svc
          atlas_search_service gRPC::grpc++)
add_test(NAME phase4_e2e_test COMMAND phase4_e2e_test)

add_executable(atlas_search_tests
  tests/search/tokenizer_test.cpp
  tests/search/stemmer_test.cpp
  tests/search/text_pipeline_test.cpp
  tests/search/inverted_index_test.cpp
  tests/search/postings_test.cpp
  tests/search/ranker_test.cpp
  tests/search/search_engine_test.cpp
  tests/search/trie_test.cpp
  tests/search/bk_tree_test.cpp
  tests/search/posting_codec_test.cpp
  tests/search/incremental_index_test.cpp
  tests/search/phrase_filter_test.cpp
  tests/common/query/parser_test.cpp
)
# gtest_main pulls in gtest transitively; naming both makes the linker warn about duplicates.
target_link_libraries(atlas_search_tests PRIVATE atlas_search GTest::gtest_main)
add_test(NAME atlas_search_tests COMMAND atlas_search_tests)
