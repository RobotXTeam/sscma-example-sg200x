set(srcs
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
)

set(incs ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/node
)

component_register(
    COMPONENT_NAME main
    SRCS ${srcs}
    INCLUDE_DIRS ${incs}
    PRIVATE_REQUIREDS sscma-micro cvi_rtsp udp_service
)