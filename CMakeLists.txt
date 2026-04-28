cmake_minimum_required(VERSION 3.16)
project(camera_pc_linux_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Qt5 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(camera_pc_gui
    src/main.cpp
    src/widget.cpp
    include/widget.h
    src/widget.ui
    src/cameraprobe.cpp
    include/cameraprobe.h
    src/video_packet_batcher.cpp
    include/video_packet_batcher.h
    src/XDMA_MoreB_linux.cc
    include/xdmaDLL_public_linux.h
)

target_include_directories(camera_pc_gui
    PRIVATE
        include
        src
)

target_link_libraries(camera_pc_gui
    PRIVATE
        Qt5::Widgets
        Qt5::Multimedia
        Qt5::MultimediaWidgets
)
