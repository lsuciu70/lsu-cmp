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

# Test client.
add_executable(cmp_client main.cpp)
target_link_libraries(cmp_client PRIVATE cmp_subscriber)
target_compile_options(cmp_client PRIVATE -Wall -Wextra)
