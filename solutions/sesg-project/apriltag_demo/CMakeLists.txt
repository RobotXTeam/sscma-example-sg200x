cmake_minimum_required(VERSION 3.5.0)

project(apriltag_demo C CXX)

get_filename_component(PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
message(STATUS "PROJECT_DIR: ${PROJECT_DIR}")

get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)
message(STATUS "ROOT_DIR: ${ROOT_DIR}")

# Set C++17 standard before including toolchain
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${ROOT_DIR}/cmake/toolchain-riscv64-linux-musl-x86_64.cmake)
include(${ROOT_DIR}/cmake/project.cmake)
