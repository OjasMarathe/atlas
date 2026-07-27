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
target_link_libraries(atlas_node PRIVATE atlas_proto gRPC::grpc++ gRPC::grpc++_reflection)

# ---- Phase 1: distributed file system ----
add_library(atlas_common STATIC src/common/sha256.cpp)
target_include_directories(atlas_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(atlas_dfs STATIC src/dfs/chunking.cpp)
target_link_libraries(atlas_dfs PUBLIC atlas_common)

# ---- tests ----
enable_testing()
add_executable(atlas_tests tests/smoke_test.cpp)
target_link_libraries(atlas_tests PRIVATE atlas_proto GTest::gtest)
add_test(NAME atlas_tests COMMAND atlas_tests)

# Dependency-free (no gRPC/GTest): builds and runs even without the vcpkg deps.
add_executable(chunking_test tests/chunking_test.cpp)
target_link_libraries(chunking_test PRIVATE atlas_dfs)
add_test(NAME chunking_test COMMAND chunking_test)
