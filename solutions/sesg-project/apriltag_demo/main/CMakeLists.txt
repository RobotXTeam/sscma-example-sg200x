# AprilTag library source files
set(APRILTAG_DIR ${ROOT_DIR}/../apriltag_lib)

file(GLOB APRILTAG_COMMON_SRCS ${APRILTAG_DIR}/common/*.c)
set(APRILTAG_SRCS 
    ${APRILTAG_DIR}/apriltag.c
    ${APRILTAG_DIR}/apriltag_pose.c
    ${APRILTAG_DIR}/apriltag_quad_thresh.c
    ${APRILTAG_COMMON_SRCS}
)

# Tag family files
file(GLOB TAG_FILES ${APRILTAG_DIR}/tag*.c)

# Build AprilTag as static library
add_library(apriltag_static STATIC ${APRILTAG_SRCS} ${TAG_FILES})
target_include_directories(apriltag_static PUBLIC ${APRILTAG_DIR})
target_compile_options(apriltag_static PRIVATE -Wno-error)
set_target_properties(apriltag_static PROPERTIES C_STANDARD 99)
target_link_libraries(apriltag_static PUBLIC m pthread)

# Demo source files
file(GLOB_RECURSE srcs ${CMAKE_CURRENT_LIST_DIR}/*.c ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(incs ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${APRILTAG_DIR}
)

component_register(
    COMPONENT_NAME main
    SRCS ${srcs}
    INCLUDE_DIRS ${incs}
    PRIVATE_REQUIREDS sscma-micro stream_udp
    REQUIREDS
        apriltag_static
)
