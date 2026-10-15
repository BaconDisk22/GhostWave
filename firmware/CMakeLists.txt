cmake_minimum_required(VERSION 3.16)
project(ghostwave C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(GHOSTWAVE_BUILD_TESTS "Build host unit tests" ON)
option(GHOSTWAVE_BUILD_PI "Build the Raspberry Pi daemon (needs lgpio)" OFF)

# Monocypher 4.0.2, vendored and pinned (third_party/monocypher/README.md)
add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99)

add_library(ghostwave_core
  core/src/packet.cpp
  core/src/crypto.cpp
  core/src/util.cpp
  core/src/router.cpp)
target_include_directories(ghostwave_core PUBLIC core/include)
target_link_libraries(ghostwave_core PRIVATE monocypher)
target_compile_options(ghostwave_core PRIVATE -Wall -Wextra)

if(GHOSTWAVE_BUILD_TESTS)
  enable_testing()
  add_executable(ghostwave_tests tests/test_main.cpp)
  target_link_libraries(ghostwave_tests ghostwave_core monocypher)
  target_compile_options(ghostwave_tests PRIVATE -Wall -Wextra)
  add_test(NAME ghostwave_tests COMMAND ghostwave_tests)
endif()

if(GHOSTWAVE_BUILD_PI)
  include(FetchContent)
  FetchContent_Declare(RadioLib
    GIT_REPOSITORY https://github.com/jgromes/RadioLib.git
    GIT_TAG 7.8.1)
  FetchContent_MakeAvailable(RadioLib)
  find_library(LGPIO lgpio REQUIRED)
  add_executable(ghostwaved pi/main.cpp)
  target_link_libraries(ghostwaved ghostwave_core RadioLib ${LGPIO} pthread)
endif()
