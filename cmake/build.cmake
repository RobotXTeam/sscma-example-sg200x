configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY
)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/dummy.c "")

add_executable(${PROJECT_NAME} ${CMAKE_CURRENT_BINARY_DIR}/dummy.c)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${PROJECT_NAME}.map")

target_link_options(${PROJECT_NAME} PRIVATE
    "-Wl,-rpath,\$ORIGIN/../../../../..//components/SeSg/open/lib"
    "-Wl,-rpath,/home/recamera/sscma-example-sg200x/components/SeSg/open/lib"
    "-Wl,-rpath,/home/recamera/sdk_libs"
    "-Wl,-rpath,/mnt/system/lib"
    "-Wl,-rpath,/mnt/system/usr/lib"
    "-Wl,-rpath,/mnt/system/usr/lib/3rd"
)

target_link_libraries(${PROJECT_NAME} PRIVATE main)
