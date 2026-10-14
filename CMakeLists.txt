cmake_minimum_required(VERSION 3.20)
project(OrderFlow VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ORDERFLOW_BUILD_TESTS  "Build unit + fuzz tests (GoogleTest)" ON)
option(ORDERFLOW_BUILD_BENCH  "Build benchmarks (Google Benchmark)" ON)
option(ORDERFLOW_NATIVE       "Optimise for this CPU (-march=native)" OFF)
option(ORDERFLOW_WERROR       "Treat warnings as errors" ON)
option(ORDERFLOW_SANITIZE     "Build with AddressSanitizer + UBSan" OFF)

# ---------------------------------------------------------------------------
# Common compile options, applied through an INTERFACE target
# ---------------------------------------------------------------------------
add_library(orderflow_options INTERFACE)
target_compile_options(orderflow_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(ORDERFLOW_WERROR)
  target_compile_options(orderflow_options INTERFACE -Werror)
endif()
if(ORDERFLOW_NATIVE)
  target_compile_options(orderflow_options INTERFACE -march=native)
endif()
if(ORDERFLOW_SANITIZE)
  target_compile_options(orderflow_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(orderflow_options INTERFACE -fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: the matching engine (no networking, no threads)
# ---------------------------------------------------------------------------
add_library(orderflow_core
  src/order_book.cpp
  src/matching_engine.cpp
  src/journal.cpp
)
target_include_directories(orderflow_core PUBLIC include)
target_link_libraries(orderflow_core PUBLIC orderflow_options)

# ---------------------------------------------------------------------------
# TCP gateway server + tools
# ---------------------------------------------------------------------------
add_library(orderflow_gateway gateway/gateway.cpp)
target_include_directories(orderflow_gateway PUBLIC gateway)
target_link_libraries(orderflow_gateway PUBLIC orderflow_core Threads::Threads)

add_executable(orderflow_server gateway/main.cpp)
target_link_libraries(orderflow_server PRIVATE orderflow_gateway)

add_executable(orderflow_client tools/client.cpp)
target_link_libraries(orderflow_client PRIVATE orderflow_core Threads::Threads)

add_executable(orderflow_gen tools/gen_workload.cpp)
target_link_libraries(orderflow_gen PRIVATE orderflow_core)

add_executable(orderflow_replay tools/replay.cpp)
target_link_libraries(orderflow_replay PRIVATE orderflow_core)

# ---------------------------------------------------------------------------
# Dependencies fetched at configure time (pinned versions)
# ---------------------------------------------------------------------------
include(FetchContent)

if(ORDERFLOW_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(orderflow_tests
    tests/test_order_book.cpp
    tests/test_order_types.cpp
    tests/test_engine.cpp
    tests/test_object_pool.cpp
    tests/test_spsc_ring.cpp
    tests/test_journal.cpp
    tests/test_fuzz.cpp
    tests/test_gateway.cpp
  )
  target_include_directories(orderflow_tests PRIVATE tests)
  target_link_libraries(orderflow_tests PRIVATE orderflow_gateway GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(orderflow_tests DISCOVERY_TIMEOUT 60)
endif()

if(ORDERFLOW_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(orderflow_bench bench/bench_engine.cpp)
  target_link_libraries(orderflow_bench PRIVATE orderflow_core benchmark::benchmark_main)

  add_executable(orderflow_latency bench/latency_bench.cpp)
  target_link_libraries(orderflow_latency PRIVATE orderflow_core)
endif()
