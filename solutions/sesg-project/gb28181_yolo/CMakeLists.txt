cmake_minimum_required(VERSION 3.5.0)

project(gb28181_engine C CXX)

get_filename_component(PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
message(STATUS "PROJECT_DIR: ${PROJECT_DIR}")

get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

include(${ROOT_DIR}/cmake/toolchain-riscv64-linux-musl-x86_64.cmake)
message(STATUS "ROOT_DIR: ${ROOT_DIR}")

# 启用 SG2002 RGN/OSD 硬件叠加（检测框直接画在视频通道上）。
set(SESG_STREAM_RTSP_ENABLE_RGN ON CACHE BOOL "Enable RGN overlay" FORCE)

# 该工具链 gcc 10.2 默认 gnu++14，但 sscma-micro 等组件用了 C++17。
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17")

include(${ROOT_DIR}/cmake/project.cmake)
