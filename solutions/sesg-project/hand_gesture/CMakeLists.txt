cmake_minimum_required(VERSION 3.5.0)

project(hand_gesture C CXX)

get_filename_component(PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
message(STATUS "PROJECT_DIR: ${PROJECT_DIR}")

get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)
message(STATUS "ROOT_DIR: ${ROOT_DIR}")

include(${ROOT_DIR}/cmake/toolchain-riscv64-linux-musl-x86_64.cmake)

# 强制 C++17：sscma-micro 的 ma_storage.h 使用 if constexpr / std::is_same_v。
# CMAKE_CXX_STANDARD（在 project.cmake 内设置）对子组件 target 不一定生效
# （musl 10.2 的 GCC 若 CMAKE_CXX_FLAGS 被占用不会自动推导 -std=c++17），
# 因此在 include(project.cmake)（它会注册 sscma-micro 等子组件 target）之前，
# 用 add_compile_options 显式追加 -std=c++17，确保所有 target 生效。
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)

include(${ROOT_DIR}/cmake/project.cmake)
