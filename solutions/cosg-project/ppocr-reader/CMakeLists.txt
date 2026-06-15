cmake_minimum_required(VERSION 3.5.0)

project(ppocr-reader VERSION 0.3.0 LANGUAGES C CXX)

set(PROJECT_DESCRIPTION "PP-OCRv3 Text Detection & Recognition for ReCamera")

get_filename_component(PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17")

include(${ROOT_DIR}/cmake/toolchain-riscv64-linux-musl-x86_64.cmake)
include(${ROOT_DIR}/cmake/project.cmake)
