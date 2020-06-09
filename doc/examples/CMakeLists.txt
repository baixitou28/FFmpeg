cmake_minimum_required(VERSION 2.8)

set(RPOJECT_NAME      "avio")
project(${RPOJECT_NAME} C )

configure_file(
	"another.sdp"
	"another.sdp"
)
configure_file(
	"my_audio.sdp"
	"my_audio.sdp"
)
configure_file(
	"my_video.sdp"
	"my_video.sdp"
)
configure_file(
	"my_all.sdp"
	"my_all.sdp"
)
configure_file(
	"out.al"
	"out.al"
)
configure_file(
	"ChID-BLITS-EBU-Narration441AOT2.mp4"
	"ChID-BLITS-EBU-Narration441AOT2.mp4"
) 
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_FLAGS "-D__STDC_CONSTANT_MACROS")
 
find_package(PkgConfig REQUIRED)

pkg_check_modules(ffmpeg REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libavfilter libswscale libswresample libpostproc libavresample)

add_executable(avio_reading avio_reading.c)
target_link_libraries(avio_reading PRIVATE PkgConfig::ffmpeg postproc z lzma  rt m)

add_executable(demuxing_decoding demuxing_decoding.c)
target_link_libraries(demuxing_decoding PRIVATE PkgConfig::ffmpeg postproc z lzma  rt m)

add_executable(muxing muxing.c)
target_link_libraries(muxing PRIVATE PkgConfig::ffmpeg postproc z lzma  rt m)

add_executable(remuxing remuxing.c)
target_link_libraries(remuxing PRIVATE PkgConfig::ffmpeg postproc z lzma  rt m)

add_executable(transcode_aac transcode_aac.c)
target_link_libraries(transcode_aac PRIVATE PkgConfig::ffmpeg postproc z lzma  rt m)

add_executable(transcoding transcoding.c)
target_link_libraries(transcoding PRIVATE PkgConfig::ffmpeg postproc z lzma  rt m)