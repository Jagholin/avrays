cmake_minimum_required(VERSION 4.4)

project(avrays)
cmake_pkg_config(IMPORT libavcodec REQUIRED)
cmake_pkg_config(IMPORT libavutil REQUIRED)
cmake_pkg_config(IMPORT libavformat REQUIRED)
cmake_pkg_config(IMPORT libswresample REQUIRED)
add_library(avrays STATIC)
target_sources(avrays PUBLIC avlib.h PRIVATE avlib.c)
target_link_libraries(avrays PRIVATE PkgConfig::libavcodec PkgConfig::libavutil PkgConfig::libavformat PkgConfig::libswresample)

add_executable(player)
target_sources(player PRIVATE main.c)
target_link_libraries(player PRIVATE avrays raylib m X11)
