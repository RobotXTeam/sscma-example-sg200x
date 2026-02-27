file(GLOB_RECURSE srcs ${CMAKE_CURRENT_LIST_DIR}/*.c ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(incs ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${ROOT_DIR}/components/SeSg/open/include/opencv4
)

set(opencv_lib_dir
    ${ROOT_DIR}/components/SeSg/open/lib
)

component_register(
    COMPONENT_NAME main
    SRCS ${srcs}
    INCLUDE_DIRS ${incs}
    LIBRARY_DIRS ${opencv_lib_dir}
    PRIVATE_REQUIREDS sscma-micro stream_udp
    REQUIREDS
        # OpenCV imgcodecs 仍沿用 SDK 自带（用于 imread）
        opencv_imgcodecs
        # 强制使用 SeSg/open/lib 下自编的 OpenCV 子库，避免与 SDK OpenCV core 混用
        ${opencv_lib_dir}/libopencv_objdetect.so.4.5.0
        ${opencv_lib_dir}/libopencv_calib3d.so.4.5.0
        ${opencv_lib_dir}/libopencv_features2d.so.4.5.0
        ${opencv_lib_dir}/libopencv_flann.so.4.5.0
        ${opencv_lib_dir}/libopencv_imgproc.so.4.5.0
        ${opencv_lib_dir}/libopencv_core.so.4.5.0
        z
)
