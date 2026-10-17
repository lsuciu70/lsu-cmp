cmake_minimum_required(VERSION 3.16)
project(cmp_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Shared with the firmware: CMP codec and the subscription protocol.
set(FIRMWARE_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

# Reusable part, for later clients: CmpSubscriber + CMP decoder.
add_library(cmp_subscriber STATIC
  cmp_subscriber.cpp
  ${FIRMWARE_LIB_DIR}/CmpCodec/src/cmp_decoder.cpp)
target_include_directories(cmp_subscriber PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FIRMWARE_LIB_DIR}/CmpCodec/src
  ${FIRMWARE_LIB_DIR}/LsuProtocol/src)
target_compile_options(cmp_subscriber PRIVATE -Wall -Wextra)

# Recording and playback (pcap), shared by the client and the player.
add_library(pcap_file STATIC pcap_file.cpp)
target_include_directories(pcap_file PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pcap_file PRIVATE -Wall -Wextra)

# Test client, with the OBD2 response decoder.
add_executable(cmp_client main.cpp obd_decoder.cpp)
target_include_directories(cmp_client PRIVATE ${FIRMWARE_LIB_DIR}/LsuObd/src)
target_link_libraries(cmp_client PRIVATE cmp_subscriber pcap_file)
target_compile_options(cmp_client PRIVATE -Wall -Wextra)

# Plays a recording back as if it were the ESP32.
add_executable(cmp_replay replay.cpp)
target_link_libraries(cmp_replay PRIVATE cmp_subscriber pcap_file)
target_compile_options(cmp_replay PRIVATE -Wall -Wextra)

# Host test of the pcap reader and writer: ctest --test-dir tools/cmp_client/build
enable_testing()
add_executable(test_pcap test_pcap.cpp)
target_link_libraries(test_pcap PRIVATE pcap_file)
target_compile_options(test_pcap PRIVATE -Wall -Wextra)
add_test(NAME pcap COMMAND test_pcap)
