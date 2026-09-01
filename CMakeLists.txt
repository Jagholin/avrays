# 4.3 is what VS ships with now, we use it as a baseline
# (but will probably work with lower versions, too)
cmake_minimum_required(VERSION 4.3)

# The options logic has to be made before project(...) 
# since that's when vcpkg install runs.
option(USE_FFMPEG_FROM_VCPKG "Install ffmpeg from vcpkg" ON)
option(USE_FFMPEG_FROM_PKGCONFIG "Use ffmpeg installed on the system" OFF)
if("${USE_FFMPEG_FROM_VCPKG}")
  list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
endif()
option(USE_INCLUDED_RAYLIB "Use raylib from /vendor directory" ON)
option(USE_PTHREADS4W "Use pthreads4w lib" OFF)
option(RAYLIB_DIR "raylib base directory to use" "")
if ("${USE_INCLUDED_RAYLIB}")
  set(RAYLIB_DIR "vendor/raylib")
endif()

if(USE_PTHREADS4W)
  message(STATUS "Activating pthreads4w")
  list(APPEND VCPKG_MANIFEST_FEATURES "pthreads")
endif()

project(avrays)

# We want a release build of Raylib no matter what.
set(OLD_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
set(CMAKE_BUILD_TYPE Release)
add_subdirectory("${RAYLIB_DIR}")
set(CMAKE_BUILD_TYPE "${OLD_BUILD_TYPE}")

# Pthreads4w is only for microsoft's compiler, mingw has its own implementation of 
# pthreads calls.
if(USE_PTHREADS4W)
  find_package(PThreads4W REQUIRED)
endif()

# Now try to find FFMPEG library. If CMAKE_TOOLCHAIN_FILE is defined and contains vcpkg,
# assume that user wants to use it to build ffmpeg
if("${USE_FFMPEG_FROM_VCPKG}")
  find_package(FFMPEG REQUIRED)
elseif("${USE_FFMPEG_FROM_PKGCONFIG}")
  message(STATUS "Using ffmpeg from pkgconfig")
  cmake_pkg_config(IMPORT libavcodec REQUIRED)
  cmake_pkg_config(IMPORT libavutil REQUIRED)
  cmake_pkg_config(IMPORT libavformat REQUIRED)
  cmake_pkg_config(IMPORT libswresample REQUIRED)
  list(APPEND FFMPEG_LIBRARIES PkgConfig::libavcodec PkgConfig::libavutil PkgConfig::libavformat PkgConfig::libswresample)
else()
  message(FATAL_ERROR "ffmpeg's libav libraries are required to build this.
  Set USE_FFMPEG_FROM_PKGCONFIG or USE_FFMPEG_FROM_VCPKG variable to ON.")
endif()

add_library(avrays STATIC)
target_sources(avrays PUBLIC avlib.h PRIVATE avlib.c)
target_include_directories(avrays PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(avrays PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(avrays PRIVATE raylib ${FFMPEG_LIBRARIES})
if(USE_PTHREADS4W)
  target_link_libraries(avrays PRIVATE PThreads4W::PThreads4W)
endif()

add_executable(player)
target_sources(player PRIVATE main.c)
if(MSVC)
  target_sources(player PRIVATE winstuff.c)
endif()
target_include_directories(player PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(player PRIVATE avrays raylib)
