cmake_minimum_required(VERSION 2.8)
cmake_policy(SET CMP0042 NEW)
cmake_policy(SET CMP0046 NEW)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING
        "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

project(libgroove C CXX)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

set(LIBGROOVE_VERSION_MAJOR 5)
set(LIBGROOVE_VERSION_MINOR 0)
set(LIBGROOVE_VERSION_PATCH 0)
set(LIBGROOVE_VERSION "${LIBGROOVE_VERSION_MAJOR}.${LIBGROOVE_VERSION_MINOR}.${LIBGROOVE_VERSION_PATCH}")
message("Configuring libgroove version ${LIBGROOVE_VERSION}")

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
)

find_package(Threads)
if(Threads_FOUND)
    set(STATUS_THREADS "OK")
else(Threads_FOUND)
    set(STATUS_THREADS "not found")
endif(Threads_FOUND)

# TODO find_library(RT_LIBRARY NAMES rt)
# TODO find_library(M_LIBRARY NAMES m)

find_package(ebur128)
if(EBUR128_FOUND)
    set(STATUS_EBUR128 "OK")
    include_directories(${EBUR128_INCLUDE_DIR})
else(EBUR128_FOUND)
    set(STATUS_EBUR128 "not found")
endif(EBUR128_FOUND)

find_package(Chromaprint)
if(CHROMAPRINT_FOUND)
    set(STATUS_CHROMAPRINT "OK")
    include_directories(${CHROMAPRINT_INCLUDE_DIR})
else(CHROMAPRINT_FOUND)
    set(STATUS_CHROMAPRINT "not found")
endif(CHROMAPRINT_FOUND)

find_package(soundio)
if(SOUNDIO_FOUND)
    set(STATUS_SOUNDIO "OK")
    include_directories(${SOUNDIO_INCLUDE_DIR})
else(SOUNDIO_FOUND)
    set(STATUS_SOUNDIO "not found")
endif(SOUNDIO_FOUND)

find_package(ffmpeg)
if(AVCODEC_FOUND)
    set(STATUS_LIBAVCODEC "OK")
    include_directories(${AVCODEC_INCLUDE_DIRS})
else()
    set(STATUS_LIBAVCODEC "not found")
endif()
if(AVFILTER_FOUND)
    set(STATUS_LIBAVFILTER "OK")
    include_directories(${AVFILTER_INCLUDE_DIRS})
else()
    set(STATUS_LIBAVFILTER "not found")
endif()
if(AVFORMAT_FOUND)
    set(STATUS_LIBAVFORMAT "OK")
    include_directories(${AVFORMAT_INCLUDE_DIRS})
else()
    set(STATUS_LIBAVFORMAT "not found")
endif()
if(AVUTIL_FOUND)
    set(STATUS_LIBAVUTIL "OK")
    include_directories(${AVUTIL_INCLUDE_DIRS})
else()
    set(STATUS_LIBAVUTIL "not found")
endif()

set(LIBGROOVE_SOURCES
    "${CMAKE_SOURCE_DIR}/src/buffer.cpp"
    "${CMAKE_SOURCE_DIR}/src/file.cpp"
    "${CMAKE_SOURCE_DIR}/src/global.cpp"
    "${CMAKE_SOURCE_DIR}/src/player.cpp"
    "${CMAKE_SOURCE_DIR}/src/queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/encoder.cpp"
    "${CMAKE_SOURCE_DIR}/src/fingerprinter.cpp"
    "${CMAKE_SOURCE_DIR}/src/loudness.cpp"
    "${CMAKE_SOURCE_DIR}/src/playlist.cpp"
    "${CMAKE_SOURCE_DIR}/src/util.cpp"
)

set(CONFIGURE_OUT_FILE "${CMAKE_BINARY_DIR}/config.h")
set(LIBGROOVE_HEADERS
    "${CMAKE_SOURCE_DIR}/groove/encoder.h"
    "${CMAKE_SOURCE_DIR}/groove/fingerprinter.h"
    "${CMAKE_SOURCE_DIR}/groove/groove.h"
    "${CMAKE_SOURCE_DIR}/groove/loudness.h"
    "${CMAKE_SOURCE_DIR}/groove/player.h"
    ${CONFIGURE_OUT_FILE}
)

# GTFO, -lstdc++ !!
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "")

set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Wno-unused-variable -Wno-unused-but-set-variable")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wno-unused-variable -Wno-unused-but-set-variable")

set(LIB_CFLAGS "-std=c++11 -fno-exceptions -fno-rtti -Werror -Wall -Werror=strict-prototypes -Werror=old-style-definition -Werror=missing-prototypes")
set(EXAMPLE_CFLAGS "-std=c99 -pedantic -Werror -Wall")

configure_file (
    "${CMAKE_SOURCE_DIR}/src/config.h.in"
    ${CONFIGURE_OUT_FILE}
)

add_library(libgroove_shared SHARED ${LIBGROOVE_SOURCES})
set_target_properties(libgroove_shared PROPERTIES
    OUTPUT_NAME groove
    SOVERSION ${LIBGROOVE_VERSION_MAJOR}
    VERSION ${LIBGROOVE_VERSION}
    COMPILE_FLAGS ${LIB_CFLAGS}
    LINKER_LANGUAGE C
)

target_link_libraries(libgroove_shared LINK_PUBLIC
    ${CHROMAPRINT_LIBRARY}
    ${AVCODEC_LIBRARIES}
    ${AVFILTER_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SOUNDIO_LIBRARY}
    ${EBUR128_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    m
)

install(TARGETS libgroove_shared DESTINATION lib)


add_library(libgroove_static STATIC ${LIBGROOVE_SOURCES})
set_target_properties(libgroove_static PROPERTIES
    OUTPUT_NAME groove
    COMPILE_FLAGS ${LIB_CFLAGS}
    LINKER_LANGUAGE C
)
install(TARGETS libgroove_static DESTINATION lib)

install(FILES ${LIBGROOVE_HEADERS} DESTINATION "include/groove")

# Example Programs
add_executable(metadata example/metadata.c)
set_target_properties(metadata PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
target_link_libraries(metadata libgroove_shared)
add_dependencies(metadata libgroove_shared)

add_executable(metadata_checksum example/metadata_checksum.c)
set_target_properties(metadata_checksum PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
target_link_libraries(metadata_checksum libgroove_shared)
add_dependencies(metadata_checksum libgroove_shared)

add_executable(transcode example/transcode.c)
set_target_properties(transcode PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
target_link_libraries(transcode libgroove_shared)
add_dependencies(transcode libgroove_shared)

add_executable(playlist example/playlist.c)
set_target_properties(playlist PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
target_link_libraries(playlist libgroove_shared)
add_dependencies(playlist libgroove_shared)

add_executable(replaygain example/replaygain.c)
set_target_properties(replaygain PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
target_link_libraries(replaygain libgroove_shared)
add_dependencies(replaygain libgroove_shared)

add_executable(fingerprint example/fingerprint.c)
set_target_properties(fingerprint PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
target_link_libraries(fingerprint libgroove_shared)
add_dependencies(fingerprint libgroove_shared)

message("\n"
    "Installation Summary\n"
    "--------------------\n"
    "* Install Directory            : ${CMAKE_INSTALL_PREFIX}\n"
)

message(
    "System Dependencies\n"
    "-------------------\n"
    "* threads                      : ${STATUS_THREADS}\n"
    "* libsoundio                   : ${STATUS_SOUNDIO}\n"
    "* ebur128                      : ${STATUS_EBUR128}\n"
    "* chromaprint                  : ${STATUS_CHROMAPRINT}\n"
    "* libavformat                  : ${STATUS_LIBAVFORMAT}\n"
    "* libavcodec                   : ${STATUS_LIBAVCODEC}\n"
    "* libavfilter                  : ${STATUS_LIBAVFILTER}\n"
    "* libavutil                    : ${STATUS_LIBAVUTIL}\n"
)
