cmake_minimum_required(VERSION 3.0)
project(examples)
 
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_FLAGS "-D__STDC_CONSTANT_MACROS")
 
find_package(PkgConfig REQUIRED)

pkg_check_modules(ffmpeg REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)

add_executable(avio_reading avio_reading.c)

target_link_libraries(avio_reading PRIVATE PkgConfig::ffmpeg)
target_link_libraries(avio_reading PRIVATE postproc z lzma  rt)
