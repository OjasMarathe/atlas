cmake_minimum_required(VERSION 3.24)
project(atlas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizers (ADR-0001 mitigation). Opt-in: cmake --preset default -DATLAS_SANITIZE=ON
option(ATLAS_SANITIZE "Enable AddressSanitizer + UBSan" OFF)
if(ATLAS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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

# ---- node: storage or metadata role (see src/node/main.cpp) ----
add_executable(atlas_node src/node/main.cpp)
target_link_libraries(atlas_node PRIVATE atlas_storage_svc atlas_metadata_svc atlas_cluster
                                         gRPC::grpc++_reflection)

# ---- atlas: command-line client (put / get / info / nodes) ----
add_executable(atlas_cli tools/atlas_cli.cpp)
set_target_properties(atlas_cli PROPERTIES OUTPUT_NAME atlas)
target_link_libraries(atlas_cli PRIVATE atlas_client gRPC::grpc++)

# ---- Phase 1: distributed file system ----
add_library(atlas_common STATIC src/common/sha256.cpp src/common/hash_ring.cpp)
target_include_directories(atlas_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

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

# ---- tests ----
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
target_link_libraries(phase1_e2e_test PRIVATE atlas_client atlas_storage_svc atlas_metadata_svc gRPC::grpc++)
add_test(NAME phase1_e2e_test COMMAND phase1_e2e_test)
