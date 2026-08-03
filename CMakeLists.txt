cmake_minimum_required(VERSION 3.16)

project(rock2a_rtsp_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

add_executable(rock2a_rtsp_receiver
    src/main.cpp
    src/rtsp_receiver.cpp
)

target_include_directories(rock2a_rtsp_receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(rock2a_rtsp_receiver PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
)

target_compile_options(rock2a_rtsp_receiver PRIVATE -Wall -Wextra -Wpedantic)
