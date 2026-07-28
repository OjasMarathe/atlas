cmake_minimum_required(VERSION 3.24)
project(atlas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# ---- shared query parser ----
# Not part of atlas_search on purpose: Phase 4's coordinator parses a query once and ships the
# tree to every shard, so parsing must not live inside the shard. Zero dependencies — the term
# analyzer is injected by the caller.
add_library(atlas_query STATIC src/common/query/parser.cpp)
target_include_directories(atlas_query PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---- search engine (Phase 3) ----
# Deliberately independent of atlas_proto: the index/ranking core is testable against a local
# corpus, so it doesn't wait on the storage track (see docs/plan/phase-3-kickoff.md).
add_library(atlas_search STATIC
  src/search/tokenizer.cpp
  src/search/stopwords.cpp
  src/search/stemmer.cpp
  src/search/text_pipeline.cpp
  src/search/inverted_index.cpp
  src/search/postings.cpp
  src/search/ranker.cpp
  src/search/search_engine.cpp
  src/search/corpus.cpp
)
target_include_directories(atlas_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(atlas_search PUBLIC atlas_query)

# The gRPC adapter is a separate target so atlas_search itself stays proto-free.
add_library(atlas_search_service STATIC src/search/search_service.cpp)
target_link_libraries(atlas_search_service PUBLIC atlas_search atlas_proto gRPC::grpc++)

# ---- Phase 0 node skeleton (also hosts this node's search shard) ----
add_executable(atlas_node src/node/main.cpp)
# atlas_search_service already propagates atlas_proto and gRPC::grpc++ publicly; naming them
# again here makes the linker warn about duplicates.
target_link_libraries(atlas_node PRIVATE atlas_search_service gRPC::grpc++_reflection)

# ---- Phase 3a demo: index a corpus, run ranked queries, print timings ----
add_executable(atlas_search_demo tools/search_demo.cpp)
target_link_libraries(atlas_search_demo PRIVATE atlas_search)

# ---- tests ----
enable_testing()
add_executable(atlas_tests tests/smoke_test.cpp)
target_link_libraries(atlas_tests PRIVATE atlas_proto GTest::gtest)
add_test(NAME atlas_tests COMMAND atlas_tests)

add_executable(atlas_search_tests
  tests/search/tokenizer_test.cpp
  tests/search/stemmer_test.cpp
  tests/search/text_pipeline_test.cpp
  tests/search/inverted_index_test.cpp
  tests/search/postings_test.cpp
  tests/search/ranker_test.cpp
  tests/search/search_engine_test.cpp
  tests/common/query/parser_test.cpp
)
# gtest_main pulls in gtest transitively; naming both makes the linker warn about duplicates.
target_link_libraries(atlas_search_tests PRIVATE atlas_search GTest::gtest_main)
add_test(NAME atlas_search_tests COMMAND atlas_search_tests)
