cmake_minimum_required(VERSION 2.8)
project(groove C)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

include(ExternalProject)


set(VERSION_MAJOR 2)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
message("Configuring libgroove version ${VERSION}")

# check for C99
find_package(C99)
if(C99_FLAG_DETECTED)
  set(HAVE_C99 TRUE)
  set(STATUS_C99 "OK")
else(C99_FLAG_DETECTED)
  set(STATUS_C99 "not found")
endif(C99_FLAG_DETECTED)

# check for ebur128
find_package(ebur128)
if(EBUR128_FOUND)
  set(STATUS_EBUR128 "OK (system)")
  set(EBUR128_IS_BUNDLED FALSE)
else(EBUR128_FOUND)
  set(STATUS_EBUR128 "OK (bundled)")
  set(EBUR128_IS_BUNDLED TRUE)

  set(EBUR128_SRC "${PROJECT_SOURCE_DIR}/deps/ebur128")
  add_subdirectory(${EBUR128_SRC})
  set(EBUR128_INCLUDE_DIR ${EBUR128_SRC})
endif(EBUR128_FOUND)

# check for SDL2
find_package(SDL2)
if(SDL2_FOUND)
  set(STATUS_SDL2 "OK (system)")
  set(SDL2_IS_BUNDLED FALSE)
else(SDL2_FOUND)
  set(STATUS_SDL2 "OK (bundled)")
  set(SDL2_IS_BUNDLED TRUE)

  set(SDL_SHARED OFF CACHE BOOL "test")
  set(SDL_VIDEO OFF CACHE BOOL "test")
  set(SDL_RENDER OFF CACHE BOOL "test")
  set(SDL_EVENTS OFF CACHE BOOL "test")
  set(SDL_JOYSTICK OFF CACHE BOOL "test")
  set(SDL_HAPTIC OFF CACHE BOOL "test")
  set(SDL_POWER OFF CACHE BOOL "test")
  set(SDL_TIMERS OFF CACHE BOOL "test")
  set(SDL_LOADSO OFF CACHE BOOL "test")
  set(SDL_CPUINFO OFF CACHE BOOL "test")
  set(SDL_FILESYSTEM OFF CACHE BOOL "test")

  set(EXTRA_CFLAGS "-fPIC" CACHE STRING "test")

  set(SDL2_SRC "${PROJECT_SOURCE_DIR}/deps/SDL2")
  add_subdirectory(${SDL2_SRC})
  set(SDL2_INCLUDE_DIR ${SDL2_SRC})
endif(SDL2_FOUND)

# check for bz2
find_package(BZip2)
if(BZIP2_FOUND)
  set(STATUS_BZIP2 "OK")
else(BZIP2_FOUND)
  set(STATUS_BZIP2 "not found")
endif(BZIP2_FOUND)

# check for pthreads
find_package(Threads)
if(Threads_FOUND)
  set(STATUS_THREADS "OK")
else(Threads_FOUND)
  set(STATUS_THREADS "not found")
endif(Threads_FOUND)

# check for yasm
find_package(Yasm)
if(${YASM_EXECUTABLE} STREQUAL "YASM_EXECUTABLE-NOTFOUND")
  set(STATUS_YASM "not found")
else()
  set(STATUS_YASM "OK")
endif()

# check for libmp3lame
find_package(mp3lame)
if(MP3LAME_FOUND)
  set(STATUS_MP3LAME "OK")
else(MP3LAME_FOUND)
  set(STATUS_MP3LAME "not found")
endif(MP3LAME_FOUND)

# check for zlib
find_package(ZLIB)
if(ZLIB_FOUND)
  set(STATUS_ZLIB "OK")
else(ZLIB_FOUND)
  set(STATUS_ZLIB "not found")
endif(ZLIB_FOUND)

configure_file (
  "${PROJECT_SOURCE_DIR}/src/config.h.in"
  "${PROJECT_BINARY_DIR}/config.h"
  )

file(GLOB_RECURSE SOURCES ${CMAKE_SOURCE_DIR}/src/*.c)
file(GLOB_RECURSE HEADERS ${CMAKE_SOURCE_DIR}/src/*.h)

set(LIBAV_SRC "${PROJECT_SOURCE_DIR}/deps/libav")
set(LIBAV_BUILD "${PROJECT_BINARY_DIR}/deps/libav")
set(LIBAV_INSTALL "${LIBAV_BUILD}/install")
set(LIB_CFLAGS "${C99_C_FLAGS} -pedantic -Werror -Wall -Werror=strict-prototypes -Werror=old-style-definition -Werror=missing-prototypes -g -D_REENTRANT")
if(APPLE)
  set(LIB_LDFLAGS " ")
else(APPLE)
  set(LIB_LDFLAGS "-Wl,-Bsymbolic")
endif(APPLE)
set(LIB_INCLUDES ${PROJECT_BINARY_DIR} ${PROJECT_SOURCE_DIR}/include ${SDL2_INCLUDE_DIR} ${LIBAV_INSTALL}/include ${EBUR128_INCLUDE_DIR})
set(EXAMPLE_CFLAGS "${C99_C_FLAGS} -pedantic -Werror -Wall -g")
set(EXAMPLE_INCLUDES "${PROJECT_SOURCE_DIR}/include")

ExternalProject_Add(libav
  SOURCE_DIR ${LIBAV_SRC}
  BINARY_DIR ${LIBAV_BUILD}
  INSTALL_DIR ${LIBAV_INSTALL}
  CONFIGURE_COMMAND ${LIBAV_SRC}/configure --prefix=${LIBAV_INSTALL} --enable-pic --enable-gpl --enable-libmp3lame
  )
# until libav makes a release we're stuck bundling it.
set(STATUS_LIBAV "OK (bundled)")


add_library(groove_static STATIC ${SOURCES} ${HEADERS})
set_target_properties(groove_static PROPERTIES
    OUTPUT_NAME groove
    COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
include_directories(${LIB_INCLUDES})
add_dependencies(groove_static libav)
if(SDL2_IS_BUNDLED)
  add_dependencies(groove_static SDL2-static)
endif(SDL2_IS_BUNDLED)
if(EBUR128_IS_BUNDLED)
  add_dependencies(groove_static ebur128_static)
endif(EBUR128_IS_BUNDLED)


add_library(groove SHARED ${SOURCES} ${HEADERS})
set_target_properties(groove PROPERTIES
    SOVERSION ${VERSION_MAJOR}
    VERSION ${VERSION}
    COMPILE_FLAGS ${LIB_CFLAGS}
    LINK_FLAGS ${LIB_LDFLAGS}
    )
target_link_libraries(groove
  ${LIBAV_INSTALL}/lib/libavfilter.a
  ${LIBAV_INSTALL}/lib/libavformat.a
  ${LIBAV_INSTALL}/lib/libavcodec.a
  ${LIBAV_INSTALL}/lib/libavresample.a
  ${LIBAV_INSTALL}/lib/libswscale.a
  ${LIBAV_INSTALL}/lib/libavutil.a
  ${BZIP2_LIBRARIES}
  ${ZLIB_LIBRARIES}
  ${MP3LAME_LIBRARY}
  ${CMAKE_THREAD_LIBS_INIT})
if(SDL2_IS_BUNDLED)
  target_link_libraries(groove SDL2-static)
else()
  target_link_libraries(groove ${SDL2_LIBRARY})
endif()
if(EBUR128_IS_BUNDLED)
  target_link_libraries(groove ebur128_static)
  if(SPEEXDSP_FOUND)
    target_link_libraries(groove ${SPEEXDSP_LIBRARIES})
  endif(SPEEXDSP_FOUND)
else(EBUR128_IS_BUNDLED)
  target_link_libraries(groove ${EBUR128_LIBRARY})
endif(EBUR128_IS_BUNDLED)
include_directories(${LIB_INCLUDES})
add_dependencies(groove ebur128_static libav)
if(SDL2_IS_BUNDLED)
  add_dependencies(groove SDL2-static)
endif(SDL2_IS_BUNDLED)

add_executable(metadata example/metadata.c)
set_target_properties(metadata PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(metadata groove)
add_dependencies(metadata groove)

add_executable(playlist example/playlist.c)
set_target_properties(playlist PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(playlist groove)
add_dependencies(playlist groove)

add_executable(transcode example/transcode.c)
set_target_properties(transcode PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(transcode groove)
add_dependencies(transcode groove)

add_executable(replaygain example/replaygain.c)
set_target_properties(replaygain PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(replaygain groove)
add_dependencies(replaygain groove)

install(FILES "include/groove.h" DESTINATION include)
install(TARGETS groove groove_static DESTINATION lib)


message("\n"
"Installation Summary\n"
"--------------------\n"
"* Install Directory            : ${CMAKE_INSTALL_PREFIX}\n"
)

message(
"Required Dependencies\n"
"------------------\n"
"* C99 Compiler                 : ${STATUS_C99}\n"
"* yasm                         : ${STATUS_YASM}\n"
"* threads                      : ${STATUS_THREADS}\n"
"* SDL2                         : ${STATUS_SDL2}\n"
"* ebur128                      : ${STATUS_EBUR128}\n"
"* libav                        : ${STATUS_LIBAV}\n"
"* bzip2                        : ${STATUS_BZIP2}\n"
"* mp3lame                      : ${STATUS_MP3LAME}\n"
"* zlib                         : ${STATUS_ZLIB}\n"
)

message(
"If everything is OK, proceed with\n"
"make\n"
)
