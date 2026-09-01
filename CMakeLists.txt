# 4.3 is what VS ships with now, we use it as a baseline
# (but will probably work with lower versions, too)
cmake_minimum_required(VERSION 4.3)

# The options logic has to be made before project(...) 
# since that's when vcpkg install runs.
option(USE_FFMPEG_FROM_VCPKG "Install ffmpeg from vcpkg" ON)
option(USE_FFMPEG_FROM_PKGCONFIG "Use ffmpeg installed on the system" OFF)
option(LIBRARY_ONLY "Don't build player exe" OFF)
if("${USE_FFMPEG_FROM_VCPKG}")
  list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
endif()
option(USE_PTHREADS4W "Use pthreads4w lib" OFF)
set(CACHE{RAYLIB_DIR} HELP "raylib base directory to use" TYPE PATH VALUE "./vendor/raylib")
set(CACHE{FFMPEG_DIR} HELP "base directory of libav pre-compiled files" TYPE PATH VALUE "")
message(STATUS "Raylib dir is set to ${RAYLIB_DIR}")
message(STATUS "FFMPEG base dir is set to ${FFMPEG_DIR}")

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

# Now try to find FFMPEG library.
if("${USE_FFMPEG_FROM_VCPKG}")
  find_package(FFMPEG REQUIRED)
elseif("${USE_FFMPEG_FROM_PKGCONFIG}")
  message(STATUS "Using ffmpeg from pkgconfig")
  cmake_pkg_config(IMPORT libavcodec REQUIRED)
  cmake_pkg_config(IMPORT libavutil REQUIRED)
  cmake_pkg_config(IMPORT libavformat REQUIRED)
  cmake_pkg_config(IMPORT libswresample REQUIRED)
  list(APPEND FFMPEG_LIBRARIES PkgConfig::libavcodec PkgConfig::libavutil PkgConfig::libavformat PkgConfig::libswresample)
elseif(FFMPEG_DIR)
  if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "ffmpeg's directory doesn't seem to have necessary files")
  endif()
  list(APPEND FFMPEG_INCLUDE_DIRS "${FFMPEG_DIR}/include")
  list(APPEND FFMPEG_LIBRARY_DIRS "${FFMPEG_DIR}/lib")
  list(APPEND FFMPEG_LIBRARIES "${FFMPEG_DIR}/lib/avcodec.lib" "${FFMPEG_DIR}/lib/avutil.lib" "${FFMPEG_DIR}/lib/avformat.lib" "${FFMPEG_DIR}/lib/swresample.lib")
else()
  message(FATAL_ERROR "ffmpeg's libav libraries are required to build this.
  Set USE_FFMPEG_FROM_PKGCONFIG or USE_FFMPEG_FROM_VCPKG variable to ON, or provide FFMPEG_DIR.")
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
if(USE_PTHREADS4W)
  target_link_libraries(player PRIVATE PThreads4W::PThreads4W)
endif()
