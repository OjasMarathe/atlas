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

# ---- Phase 0 node skeleton ----
add_executable(atlas_node src/node/main.cpp)
target_link_libraries(atlas_node PRIVATE atlas_storage_svc gRPC::grpc++_reflection)

# ---- Phase 1: distributed file system ----
add_library(atlas_common STATIC src/common/sha256.cpp src/common/hash_ring.cpp)
target_include_directories(atlas_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(atlas_dfs STATIC src/dfs/chunking.cpp)
target_link_libraries(atlas_dfs PUBLIC atlas_common)

add_library(atlas_storage STATIC src/storage/chunk_store.cpp)
target_link_libraries(atlas_storage PUBLIC atlas_common RocksDB::rocksdb)

add_library(atlas_storage_svc STATIC src/storage/storage_service.cpp)
target_link_libraries(atlas_storage_svc PUBLIC atlas_storage atlas_proto gRPC::grpc++)

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
