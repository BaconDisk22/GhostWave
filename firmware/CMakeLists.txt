cmake_minimum_required(VERSION 3.16)
project(ghostwave C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(GHOSTWAVE_BUILD_TESTS "Build host unit tests" ON)
option(GHOSTWAVE_BUILD_PI "Build the Raspberry Pi daemon (needs lgpio)" OFF)
option(GHOSTWAVE_BUILD_VIRT "Build ghostwaved with only the UDP virtual radio (no lgpio/RadioLib)" OFF)

add_library(monocypher STATIC
  third_party/monocypher/monocypher.c
  third_party/monocypher/monocypher-ed25519.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99)

add_library(ghostwave_core
  core/src/packet.cpp
  core/src/crypto.cpp
  core/src/util.cpp
  core/src/router.cpp
  core/src/client_proto.cpp)
target_include_directories(ghostwave_core PUBLIC core/include)
target_link_libraries(ghostwave_core PRIVATE monocypher)
target_compile_options(ghostwave_core PRIVATE -Wall -Wextra)

if(GHOSTWAVE_BUILD_TESTS)
  enable_testing()
  add_executable(ghostwave_tests tests/test_main.cpp pi/vchannel.cpp)
  target_include_directories(ghostwave_tests PRIVATE pi)
  target_link_libraries(ghostwave_tests ghostwave_core monocypher)
  target_compile_options(ghostwave_tests PRIVATE -Wall -Wextra)
  add_test(NAME ghostwave_tests COMMAND ghostwave_tests)
  add_executable(ghostwave_vchannel_tests tests/test_vchannel.cpp pi/vchannel.cpp)
  target_include_directories(ghostwave_vchannel_tests PRIVATE pi)
  target_link_libraries(ghostwave_vchannel_tests ghostwave_core)
  target_compile_options(ghostwave_vchannel_tests PRIVATE -Wall -Wextra)
  add_test(NAME ghostwave_vchannel_tests COMMAND ghostwave_vchannel_tests)
  add_executable(ghostwave_client_proto_tests tests/test_client_proto.cpp)
  target_link_libraries(ghostwave_client_proto_tests ghostwave_core)
  target_compile_options(ghostwave_client_proto_tests PRIVATE -Wall -Wextra)
  add_test(NAME ghostwave_client_proto_tests COMMAND ghostwave_client_proto_tests)
  add_executable(ghostwave_radio_port_contract_tests tests/test_radio_port_contract.cpp)
  target_link_libraries(ghostwave_radio_port_contract_tests ghostwave_core)
  target_compile_options(ghostwave_radio_port_contract_tests PRIVATE -Wall -Wextra)
  add_test(NAME ghostwave_radio_port_contract_tests COMMAND ghostwave_radio_port_contract_tests)
  add_executable(ghostwave_sx1262_adapter_tests tests/test_sx1262_adapter.cpp)
  target_link_libraries(ghostwave_sx1262_adapter_tests ghostwave_core)
  target_compile_options(ghostwave_sx1262_adapter_tests PRIVATE -Wall -Wextra)
  add_test(NAME ghostwave_sx1262_adapter_tests COMMAND ghostwave_sx1262_adapter_tests)
endif()

if(GHOSTWAVE_BUILD_PI OR GHOSTWAVE_BUILD_VIRT)
  add_executable(ghostwaved pi/main.cpp pi/radio_udp.cpp pi/vchannel.cpp)
  target_link_libraries(ghostwaved ghostwave_core)
  set_source_files_properties(pi/main.cpp pi/radio_udp.cpp pi/vchannel.cpp
    TARGET_DIRECTORY ghostwaved PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
endif()

if(GHOSTWAVE_BUILD_PI)
  include(FetchContent)
  FetchContent_Declare(RadioLib
    GIT_REPOSITORY https://github.com/jgromes/RadioLib.git
    GIT_TAG 7.8.1)
  FetchContent_MakeAvailable(RadioLib)
  find_library(LGPIO lgpio REQUIRED)
  target_sources(ghostwaved PRIVATE pi/radio_e22p.cpp)
  target_compile_definitions(ghostwaved PRIVATE GHOSTWAVE_HAVE_E22P)
  target_link_libraries(ghostwaved RadioLib ${LGPIO} pthread)
endif()
