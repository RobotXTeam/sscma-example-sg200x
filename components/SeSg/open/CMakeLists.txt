# This component is a thin shim that provides OpenCV objdetect headers/libs
# from a local drop-in directory under components/SeSg/opencv_objdetect.
#
# Layout (recommended):
#   components/SeSg/opencv_objdetect/
#     include/...
#     lib/...
#
# Then in a solution's main/CMakeLists.txt add:
#   REQUIREDS ... opencv_objdetect
#
# The linker will resolve -lopencv_objdetect from lib/.

set(_OPENCV_OBJDETECT_DIR ${CMAKE_CURRENT_LIST_DIR})

component_register(
    COMPONENT_NAME opencv_objdetect
    INCLUDE_DIRS
        ${_OPENCV_OBJDETECT_DIR}/include
        ${_OPENCV_OBJDETECT_DIR}/include/opencv4
    LIBRARY_DIRS ${_OPENCV_OBJDETECT_DIR}/lib
)
