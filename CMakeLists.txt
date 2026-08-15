cmake_minimum_required(VERSION 4.4)

project(avrays)

add_library(avrays STATIC)
target_sources(avrays PUBLIC avlib.h PRIVATE avlib.c)
target_link_libraries(avrays PRIVATE avcodec avutil avformat swresample)

add_executable(player)
target_sources(player PRIVATE main.c)
target_link_libraries(player PRIVATE avrays raylib m X11)
