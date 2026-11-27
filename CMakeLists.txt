cmake_minimum_required(VERSION 3.20)
project(polyarb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(POLYARB_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

# --- core: pure pricing math, no I/O -------------------------------------
add_library(polyarb_core src/arb.cpp src/book.cpp)
target_include_directories(polyarb_core PUBLIC include)
target_compile_options(polyarb_core PRIVATE -Wall -Wextra -Wpedantic)

# --- client: Polymarket HTTP + JSON ------------------------------------
find_package(CURL REQUIRED)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(polyarb_client src/client.cpp)
target_link_libraries(polyarb_client PUBLIC polyarb_core PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
target_compile_options(polyarb_client PRIVATE -Wall -Wextra -Wpedantic)

# --- feed: WebSocket message -> order books --------------------------------
add_library(polyarb_feed src/feed.cpp)
target_link_libraries(polyarb_feed PUBLIC polyarb_core PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(polyarb_feed PRIVATE -Wall -Wextra -Wpedantic)

# --- log: compressed append-only recording ----------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
add_library(polyarb_log src/log.cpp)
target_include_directories(polyarb_log PUBLIC include)
target_link_libraries(polyarb_log PRIVATE PkgConfig::ZSTD)
target_compile_options(polyarb_log PRIVATE -Wall -Wextra -Wpedantic)

# --- scanner CLI ----------------------------------------------------------
add_executable(polyarb_scan src/main.cpp)
target_link_libraries(polyarb_scan PRIVATE polyarb_client)
target_compile_options(polyarb_scan PRIVATE -Wall -Wextra -Wpedantic)

# --- tests ----------------------------------------------------------------
if(POLYARB_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)

  enable_testing()
  add_executable(polyarb_tests tests/test_arb.cpp tests/test_book.cpp tests/test_client.cpp tests/test_feed.cpp tests/test_log.cpp)
  target_link_libraries(polyarb_tests PRIVATE polyarb_core polyarb_client polyarb_feed polyarb_log Catch2::Catch2WithMain)
  target_compile_definitions(polyarb_tests PRIVATE POLYARB_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  include(Catch)
  catch_discover_tests(polyarb_tests)
endif()
