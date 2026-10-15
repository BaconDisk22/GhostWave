cmake_minimum_required(VERSION 3.16)
project(ridgeline C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(RIDGELINE_BUILD_TESTS "Build host unit tests" ON)
option(RIDGELINE_BUILD_PI "Build the Raspberry Pi daemon (needs lgpio)" OFF)

# Monocypher 4.0.2, vendored and pinned (third_party/monocypher/README.md)
add_library(monocypher STATIC
  third_party/monocypher/monocypher.c
  third_party/monocypher/monocypher-ed25519.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99)

add_library(ridgeline_core
  core/src/packet.cpp
  core/src/crypto.cpp
  core/src/util.cpp
  core/src/router.cpp)
target_include_directories(ridgeline_core PUBLIC core/include)
target_link_libraries(ridgeline_core PRIVATE monocypher)
target_compile_options(ridgeline_core PRIVATE -Wall -Wextra)

if(RIDGELINE_BUILD_TESTS)
  enable_testing()
  add_executable(ridgeline_tests tests/test_main.cpp)
  target_link_libraries(ridgeline_tests ridgeline_core monocypher)
  target_compile_options(ridgeline_tests PRIVATE -Wall -Wextra)
  add_test(NAME ridgeline_tests COMMAND ridgeline_tests)
endif()

if(RIDGELINE_BUILD_PI)
  include(FetchContent)
  FetchContent_Declare(RadioLib
    GIT_REPOSITORY https://github.com/jgromes/RadioLib.git
    GIT_TAG 7.8.1)
  FetchContent_MakeAvailable(RadioLib)
  find_library(LGPIO lgpio REQUIRED)
  add_executable(ridgelined pi/main.cpp)
  target_link_libraries(ridgelined ridgeline_core RadioLib ${LGPIO} pthread)
endif()
