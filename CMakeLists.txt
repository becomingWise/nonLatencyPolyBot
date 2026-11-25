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
add_library(polyarb_core src/arb.cpp)
target_include_directories(polyarb_core PUBLIC include)
target_compile_options(polyarb_core PRIVATE -Wall -Wextra -Wpedantic)

# --- tests ----------------------------------------------------------------
if(POLYARB_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)

  enable_testing()
  add_executable(polyarb_tests tests/test_arb.cpp)
  target_link_libraries(polyarb_tests PRIVATE polyarb_core Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(polyarb_tests)
endif()
