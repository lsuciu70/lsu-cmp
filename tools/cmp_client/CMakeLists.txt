cmake_minimum_required(VERSION 3.16)
project(cmp_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Shares the CMP layout constants with the firmware encoder.
set(CMP_ENCODER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/CmpEncoder/src)

add_executable(cmp_client cmp_client.cpp)
target_include_directories(cmp_client PRIVATE ${CMP_ENCODER_DIR})
target_compile_options(cmp_client PRIVATE -Wall -Wextra)
