cmake_minimum_required(VERSION 3.16)
project(XDMA_MoreB_Linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(XDMA_MoreB SHARED
    XDMA_MoreB_linux.cc
)
target_include_directories(XDMA_MoreB
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}"
)
target_compile_definitions(XDMA_MoreB PRIVATE K7CTRBOARDDRIVER_EXPORTS)
set_target_properties(XDMA_MoreB PROPERTIES
    OUTPUT_NAME "XDMA_MoreB"
)

add_executable(xdma_smoke_test
    smoke_test.cpp
)
target_include_directories(xdma_smoke_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(xdma_smoke_test PRIVATE XDMA_MoreB)
