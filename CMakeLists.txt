cmake_minimum_required(VERSION 2.8)
project(groove C)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})
include(ExternalProject)
include(GNUInstallDirs)

set(LIBGROOVE_STATUS "yes")
set(LIBGROOVE_VERSION_MAJOR 3)
set(LIBGROOVE_VERSION_MINOR 0)
set(LIBGROOVE_VERSION_PATCH 7)
set(LIBGROOVE_VERSION "${LIBGROOVE_VERSION_MAJOR}.${LIBGROOVE_VERSION_MINOR}.${LIBGROOVE_VERSION_PATCH}")
message("Configuring libgroove version ${LIBGROOVE_VERSION}")
file(GLOB_RECURSE LIBGROOVE_SOURCES ${CMAKE_SOURCE_DIR}/groove/*.c)
file(GLOB_RECURSE LIBGROOVE_HEADERS ${CMAKE_SOURCE_DIR}/groove/*.h)

if(DISABLE_PLAYER)
  set(LIBGROOVE_PLAYER_STATUS "no")
else(DISABLE_PLAYER)
  set(LIBGROOVE_PLAYER_STATUS "yes")
  message("Configuring libgrooveplayer version ${LIBGROOVE_VERSION}")
  file(GLOB_RECURSE LIBGROOVE_PLAYER_SOURCES ${CMAKE_SOURCE_DIR}/grooveplayer/*.c)
  file(GLOB_RECURSE LIBGROOVE_PLAYER_HEADERS ${CMAKE_SOURCE_DIR}/grooveplayer/*.h)
endif(DISABLE_PLAYER)

if(DISABLE_LOUDNESS)
  set(LIBGROOVE_LOUDNESS_STATUS "no")
else(DISABLE_LOUDNESS)
  set(LIBGROOVE_LOUDNESS_STATUS "yes")
  message("Configuring libgrooveloudness version ${LIBGROOVE_VERSION}")
  file(GLOB_RECURSE LIBGROOVE_LOUDNESS_SOURCES ${CMAKE_SOURCE_DIR}/grooveloudness/*.c)
  file(GLOB_RECURSE LIBGROOVE_LOUDNESS_HEADERS ${CMAKE_SOURCE_DIR}/grooveloudness/*.h)
endif(DISABLE_LOUDNESS)

# check for C99
find_package(C99)
if(C99_FLAG_DETECTED)
  set(HAVE_C99 TRUE)
  set(STATUS_C99 "OK")
else(C99_FLAG_DETECTED)
  set(STATUS_C99 "not found")
  set(LIBGROOVE_STATUS "missing dependencies")
  set(LIBGROOVE_PLAYER_STATUS "missing dependencies")
  set(LIBGROOVE_LOUDNESS_STATUS "missing dependencies")
endif(C99_FLAG_DETECTED)

# check for ebur128
if(DISABLE_LOUDNESS)
  set(STATUS_EBUR128 "not needed")
  set(STATUS_DEP_LIBEBUR128 "not needed")
else(DISABLE_LOUDNESS)
  find_package(ebur128)
  if(EBUR128_FOUND)
    set(STATUS_EBUR128 "OK")
    set(STATUS_DEP_LIBEBUR128 "using system library")
    set(EBUR128_IS_BUNDLED FALSE)
  else(EBUR128_FOUND)
    set(STATUS_EBUR128 "not found - will use bundled version")
    set(STATUS_DEP_LIBEBUR128 "ready to build")
    set(EBUR128_IS_BUNDLED TRUE)

    set(EBUR128_SRC "${PROJECT_SOURCE_DIR}/deps/ebur128")
    add_subdirectory(${EBUR128_SRC})
    set(EBUR128_INCLUDE_DIR ${EBUR128_SRC})
  endif(EBUR128_FOUND)
endif(DISABLE_LOUDNESS)

# check for SDL2
if(DISABLE_PLAYER)
  set(STATUS_SDL2 "not needed")
  set(STATUS_DEP_SDL2 "not needed")
else(DISABLE_PLAYER)
  find_package(SDL2)
  if(SDL2_FOUND)
    set(STATUS_SDL2 "OK")
    set(STATUS_DEP_SDL2 "using system library")
    set(SDL2_IS_BUNDLED FALSE)
  else(SDL2_FOUND)
    set(STATUS_SDL2 "not found - will use bundled version")
    set(STATUS_DEP_SDL2 "ready to build")
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
endif(DISABLE_PLAYER)

# check for libav
find_package(LibAV 10.0)
if(LIBAV_AVCODEC_FOUND AND LIBAV_AVFILTER_FOUND AND LIBAV_AVFORMAT_FOUND AND LIBAV_AVUTIL_FOUND)
  set(LIBAV_IS_BUNDLED FALSE)
  set(STATUS_DEP_LIBAV "using system libraries")
  set(STATUS_BZIP2 "not needed")
  set(STATUS_YASM "not needed")
  set(STATUS_MP3LAME "not needed")
  set(STATUS_ZLIB "not needed")
  set(STATUS_LIBAVCODEC "OK")
  set(STATUS_LIBAVFILTER "OK")
  set(STATUS_LIBAVFORMAT "OK")
  set(STATUS_LIBAVUTIL "OK")
else()
  set(LIBAV_IS_BUNDLED TRUE)
  set(STATUS_DEP_LIBAV "ready to build")

  if(LIBAV_AVCODEC_FOUND)
    set(STATUS_LIBAVCODEC "OK")
  else()
    set(STATUS_LIBAVCODEC "not found - will use bundled version")
  endif()
  if(LIBAV_AVFILTER_FOUND)
    set(STATUS_LIBAVFILTER "OK")
  else()
    set(STATUS_LIBAVFILTER "not found - will use bundled version")
  endif()
  if(LIBAV_AVFORMAT_FOUND)
    set(STATUS_LIBAVFORMAT "OK")
  else()
    set(STATUS_LIBAVFORMAT "not found - will use bundled version")
  endif()
  if(LIBAV_AVUTIL_FOUND)
    set(STATUS_LIBAVUTIL "OK")
  else()
    set(STATUS_LIBAVUTIL "not found - will use bundled version")
  endif()

  # check for bz2
  find_package(BZip2)
  if(BZIP2_FOUND)
    set(STATUS_BZIP2 "OK")
  else(BZIP2_FOUND)
    set(STATUS_BZIP2 "not found")
    set(STATUS_DEP_LIBAV "missing dependencies, see below")
    set(LIBGROOVE_STATUS "missing dependencies")
    set(LIBGROOVE_PLAYER_STATUS "missing dependencies")
    set(LIBGROOVE_LOUDNESS_STATUS "missing dependencies")
  endif(BZIP2_FOUND)

  # check for yasm
  find_package(Yasm)
  if(${YASM_EXECUTABLE} STREQUAL "YASM_EXECUTABLE-NOTFOUND")
    set(STATUS_YASM "not found")
    set(STATUS_DEP_LIBAV "missing dependencies, see below")
    set(LIBGROOVE_STATUS "missing dependencies")
    set(LIBGROOVE_PLAYER_STATUS "missing dependencies")
    set(LIBGROOVE_LOUDNESS_STATUS "missing dependencies")
  else()
    set(STATUS_YASM "OK")
  endif()
  
  # check for libmp3lame
  find_package(mp3lame)
  if(MP3LAME_FOUND)
    set(STATUS_MP3LAME "OK")
  else(MP3LAME_FOUND)
    set(STATUS_DEP_LIBAV "missing dependencies, see below")
    set(STATUS_MP3LAME "not found")
  endif(MP3LAME_FOUND)

  # check for zlib
  find_package(ZLIB)
  if(ZLIB_FOUND)
    set(STATUS_ZLIB "OK")
  else(ZLIB_FOUND)
    set(STATUS_ZLIB "not found")
    set(STATUS_DEP_LIBAV "missing dependencies, see below")
    set(LIBGROOVE_STATUS "missing dependencies")
    set(LIBGROOVE_PLAYER_STATUS "missing dependencies")
    set(LIBGROOVE_LOUDNESS_STATUS "missing dependencies")
  endif(ZLIB_FOUND)

  set(LIBAV_SRC "${PROJECT_SOURCE_DIR}/deps/libav")
  set(LIBAV_BUILD "${PROJECT_BINARY_DIR}/deps/libav")
  set(LIBAV_INSTALL "${LIBAV_BUILD}/install")

  if(APPLE)
  else(APPLE)
    set(LIBGROOVE_LDFLAGS "-Wl,-Bsymbolic")
  endif(APPLE)

  ExternalProject_Add(libav
    SOURCE_DIR ${LIBAV_SRC}
    BINARY_DIR ${LIBAV_BUILD}
    INSTALL_DIR ${LIBAV_INSTALL}
    CONFIGURE_COMMAND ${LIBAV_SRC}/configure --prefix=${LIBAV_INSTALL} --enable-pic --enable-gpl --enable-libmp3lame
    )

endif()

# check for pthreads
find_package(Threads)
if(Threads_FOUND)
  set(STATUS_THREADS "OK")
else(Threads_FOUND)
  set(STATUS_THREADS "not found")
  set(LIBGROOVE_STATUS "missing dependencies")
  set(LIBGROOVE_PLAYER_STATUS "missing dependencies")
  set(LIBGROOVE_LOUDNESS_STATUS "missing dependencies")
endif(Threads_FOUND)

configure_file (
  "${PROJECT_SOURCE_DIR}/groove/config.h.in"
  "${PROJECT_BINARY_DIR}/config.h"
  )

set(LIB_CFLAGS "${C99_C_FLAGS} -pedantic -Werror -Wall -Werror=strict-prototypes -Werror=old-style-definition -Werror=missing-prototypes -D_REENTRANT")
set(EXAMPLE_CFLAGS "${C99_C_FLAGS} -pedantic -Werror -Wall -g")
set(EXAMPLE_INCLUDES "${PROJECT_SOURCE_DIR}")

add_library(groove SHARED ${LIBGROOVE_SOURCES} ${LIBGROOVE_HEADERS})
set_target_properties(groove PROPERTIES
  SOVERSION ${LIBGROOVE_VERSION_MAJOR}
  VERSION ${LIBGROOVE_VERSION}
  COMPILE_FLAGS ${LIB_CFLAGS}
  )
if(LIBGROOVE_LDFLAGS)
  set_target_properties(groove PROPERTIES LINK_FLAGS ${LIBGROOVE_LDFLAGS})
endif()
include_directories(${PROJECT_BINARY_DIR})
if(LIBAV_IS_BUNDLED)
  target_link_libraries(groove
    ${LIBAV_INSTALL}/lib/libavfilter.a
    ${LIBAV_INSTALL}/lib/libavformat.a
    ${LIBAV_INSTALL}/lib/libavcodec.a
    ${LIBAV_INSTALL}/lib/libavresample.a
    ${LIBAV_INSTALL}/lib/libswscale.a
    ${LIBAV_INSTALL}/lib/libavutil.a
    ${BZIP2_LIBRARIES}
    ${ZLIB_LIBRARIES})
  if(MP3LAME_FOUND)
    target_link_libraries(groove ${MP3LAME_LIBRARY})
  endif()
  include_directories(${LIBAV_INSTALL}/include)
  add_dependencies(groove libav)
else()
  target_link_libraries(groove LINK_PRIVATE
    ${LIBAV_AVCODEC_LIBRARIES}
    ${LIBAV_AVFILTER_LIBRARIES}
    ${LIBAV_AVFORMAT_LIBRARIES})
  target_link_libraries(groove LINK_PUBLIC ${LIBAV_AVUTIL_LIBRARIES})
  include_directories(${LIBAV_INCLUDE_DIRS})
endif()
target_link_libraries(groove LINK_PUBLIC ${CMAKE_THREAD_LIBS_INIT})


add_library(groove_static STATIC ${LIBGROOVE_SOURCES} ${LIBGROOVE_HEADERS})
set_target_properties(groove_static PROPERTIES
  OUTPUT_NAME groove
  COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
if(LIBAV_IS_BUNDLED)
  add_dependencies(groove_static libav)
endif()
install(TARGETS groove_static DESTINATION ${CMAKE_INSTALL_LIBDIR})


install(FILES
  "groove/groove.h"
  "groove/queue.h"
  "groove/encoder.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/groove")
install(TARGETS groove DESTINATION ${CMAKE_INSTALL_LIBDIR})

add_executable(metadata example/metadata.c)
set_target_properties(metadata PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(metadata groove)
add_dependencies(metadata groove)


add_executable(transcode example/transcode.c)
set_target_properties(transcode PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(transcode groove)
add_dependencies(transcode groove)


if(DISABLE_PLAYER)
else()
  add_library(grooveplayer SHARED
    ${LIBGROOVE_PLAYER_SOURCES}
    ${LIBGROOVE_PLAYER_HEADERS})
  set_target_properties(grooveplayer PROPERTIES
    SOVERSION ${LIBGROOVE_VERSION_MAJOR}
    VERSION ${LIBGROOVE_VERSION}
    COMPILE_FLAGS ${LIB_CFLAGS}
    )
  target_link_libraries(grooveplayer LINK_PUBLIC groove)
  add_dependencies(grooveplayer groove)
  if(SDL2_IS_BUNDLED)
    target_link_libraries(grooveplayer SDL2-static)
    add_dependencies(grooveplayer SDL2-static)
  else()
    target_link_libraries(grooveplayer LINK_PRIVATE ${SDL2_LIBRARY})
  endif()
  include_directories(${SDL2_INCLUDE_DIR})

  install(FILES "grooveplayer/player.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/grooveplayer")
  install(TARGETS grooveplayer DESTINATION ${CMAKE_INSTALL_LIBDIR})

  add_library(grooveplayer_static STATIC
    ${LIBGROOVE_PLAYER_SOURCES}
    ${LIBGROOVE_PLAYER_HEADERS})
  set_target_properties(grooveplayer_static PROPERTIES
    OUTPUT_NAME grooveplayer
    COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
  if(SDL2_IS_BUNDLED)
    add_dependencies(grooveplayer_static SDL2-static)
  endif()
  install(TARGETS grooveplayer_static DESTINATION ${CMAKE_INSTALL_LIBDIR})


  add_executable(playlist example/playlist.c ${PROJECT_SOURCE_DIR}/grooveplayer/player.h)
  set_target_properties(playlist PROPERTIES
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
  include_directories(${EXAMPLE_INCLUDES})
  target_link_libraries(playlist groove grooveplayer)
  add_dependencies(playlist groove grooveplayer)
endif()

if(DISABLE_LOUDNESS)
else()
  add_library(grooveloudness SHARED
    ${LIBGROOVE_LOUDNESS_SOURCES}
    ${LIBGROOVE_LOUDNESS_HEADERS})
  set_target_properties(grooveloudness PROPERTIES
    SOVERSION ${LIBGROOVE_VERSION_MAJOR}
    VERSION ${LIBGROOVE_VERSION}
    COMPILE_FLAGS ${LIB_CFLAGS}
    )
  target_link_libraries(grooveloudness LINK_PUBLIC groove ${CMAKE_THREAD_LIBS_INIT})
  add_dependencies(grooveloudness groove)
  if(EBUR128_IS_BUNDLED)
    target_link_libraries(grooveloudness ebur128_static)
    add_dependencies(grooveloudness ebur128_static)
    if(SPEEXDSP_FOUND)
      target_link_libraries(grooveloudness ${SPEEXDSP_LIBRARIES})
    endif(SPEEXDSP_FOUND)
  else()
    target_link_libraries(grooveloudness LINK_PRIVATE ${EBUR128_LIBRARY})
  endif()
  include_directories(${EBUR128_INCLUDE_DIR})

  install(FILES "grooveloudness/loudness.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/grooveloudness")
  install(TARGETS grooveloudness DESTINATION ${CMAKE_INSTALL_LIBDIR})


  add_library(grooveloudness_static STATIC
    ${LIBGROOVE_LOUDNESS_SOURCES}
    ${LIBGROOVE_LOUDNESS_HEADERS})
  set_target_properties(grooveloudness_static PROPERTIES
    OUTPUT_NAME grooveloudness
    COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
  if(EBUR128_IS_BUNDLED)
    add_dependencies(grooveloudness_static ebur128_static)
  endif()
  install(TARGETS grooveloudness_static DESTINATION ${CMAKE_INSTALL_LIBDIR})


  add_executable(replaygain example/replaygain.c)
  set_target_properties(replaygain PROPERTIES
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
  include_directories(${EXAMPLE_INCLUDES})
  target_link_libraries(replaygain groove grooveloudness)
  add_dependencies(replaygain groove grooveloudness)
endif()

message("\n"
"Installation Summary\n"
"--------------------\n"
"* Install Directory            : ${CMAKE_INSTALL_PREFIX}\n"
"* Build libgroove              : ${LIBGROOVE_STATUS}\n"
"* Build libgrooveplayer        : ${LIBGROOVE_PLAYER_STATUS}\n"
"* Build libgrooveloudness      : ${LIBGROOVE_LOUDNESS_STATUS}\n"
)

message(
"Bundled Dependencies\n"
"--------------------\n"
"* SDL2                         : ${STATUS_DEP_SDL2}\n"
"* libav                        : ${STATUS_DEP_LIBAV}\n"
"* libebur128                   : ${STATUS_DEP_LIBEBUR128}\n"
)

message(
"System Dependencies\n"
"-------------------\n"
"* C99 Compiler                 : ${STATUS_C99}\n"
"* threads                      : ${STATUS_THREADS}\n"
"* SDL2                         : ${STATUS_SDL2}\n"
"* ebur128                      : ${STATUS_EBUR128}\n"
"* libavformat                  : ${STATUS_LIBAVFORMAT}\n"
"* libavcodec                   : ${STATUS_LIBAVCODEC}\n"
"* libavfilter                  : ${STATUS_LIBAVFILTER}\n"
"* libavutil                    : ${STATUS_LIBAVUTIL}"
)

if(LIBAV_IS_BUNDLED)
message(
"* yasm                         : ${STATUS_YASM}\n"
"* bzip2                        : ${STATUS_BZIP2}\n"
"* mp3lame                      : ${STATUS_MP3LAME}\n"
"* zlib                         : ${STATUS_ZLIB}"
)
endif()

message("\n"
"If everything is looks good, proceed with\n"
"make\n"
)
