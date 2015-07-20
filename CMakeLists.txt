cmake_minimum_required(VERSION 2.8)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING
      "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

project(groove C)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

set(LIBGROOVE_STATUS "yes")
set(LIBGROOVE_VERSION_MAJOR 4)
set(LIBGROOVE_VERSION_MINOR 3)
set(LIBGROOVE_VERSION_PATCH 0)
set(LIBGROOVE_VERSION "${LIBGROOVE_VERSION_MAJOR}.${LIBGROOVE_VERSION_MINOR}.${LIBGROOVE_VERSION_PATCH}")
message("Configuring libgroove version ${LIBGROOVE_VERSION}")
file(GLOB_RECURSE LIBGROOVE_SOURCES ${CMAKE_SOURCE_DIR}/groove/*.c)
file(GLOB_RECURSE LIBGROOVE_HEADERS ${CMAKE_SOURCE_DIR}/groove/*.h)

if(DISABLE_PLAYER)
  set(LIBGROOVE_PLAYER_STATUS "no")
else(DISABLE_PLAYER)
  set(LIBGROOVE_PLAYER_STATUS "yes")
  message("Configuring libgrooveplayer version ${LIBGROOVE_VERSION}")
  set(LIBGROOVE_PLAYER_SOURCES ${CMAKE_SOURCE_DIR}/grooveplayer/player.c)
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

if(DISABLE_FINGERPRINTER)
  set(LIBGROOVE_FINGERPRINTER_STATUS "no")
else(DISABLE_FINGERPRINTER)
  set(LIBGROOVE_FINGERPRINTER_STATUS "yes")
  message("Configuring libgroovefingerprinter version ${LIBGROOVE_VERSION}")
  file(GLOB_RECURSE LIBGROOVE_FINGERPRINTER_SOURCES ${CMAKE_SOURCE_DIR}/groovefingerprinter/*.c)
  file(GLOB_RECURSE LIBGROOVE_FINGERPRINTER_HEADERS ${CMAKE_SOURCE_DIR}/groovefingerprinter/*.h)
endif(DISABLE_FINGERPRINTER)

# check for ebur128
if(DISABLE_LOUDNESS)
  set(STATUS_EBUR128 "not needed")
else(DISABLE_LOUDNESS)
  find_package(ebur128)
  if(EBUR128_FOUND)
    set(STATUS_EBUR128 "OK")
  else(EBUR128_FOUND)
    set(STATUS_EBUR128 "not found")
  endif(EBUR128_FOUND)
endif(DISABLE_LOUDNESS)

# check for chromaprint
if(DISABLE_FINGERPRINTER)
  set(STATUS_CHROMAPRINT "not needed")
else(DISABLE_FINGERPRINTER)
  find_package(Chromaprint)
  if(CHROMAPRINT_FOUND)
    set(STATUS_CHROMAPRINT "OK")
  else(CHROMAPRINT_FOUND)
    set(STATUS_CHROMAPRINT "not found")
  endif(CHROMAPRINT_FOUND)
endif(DISABLE_FINGERPRINTER)

# check for libsoundio
if(DISABLE_PLAYER)
  set(STATUS_SOUNDIO "not needed")
else(DISABLE_PLAYER)
  find_package(soundio)
  if(SOUNDIO_FOUND)
    set(STATUS_SOUNDIO "OK")
  else(SOUNDIO_FOUND)
    set(STATUS_SOUNDIO "not found")
  endif(SOUNDIO_FOUND)
endif(DISABLE_PLAYER)

# check for ffmpeg
find_package(ffmpeg)
if(AVCODEC_FOUND)
  set(STATUS_LIBAVCODEC "OK")
else()
  set(STATUS_LIBAVCODEC "not found")
endif()
if(AVFILTER_FOUND)
  set(STATUS_LIBAVFILTER "OK")
else()
  set(STATUS_LIBAVFILTER "not found")
endif()
if(AVFORMAT_FOUND)
  set(STATUS_LIBAVFORMAT "OK")
else()
  set(STATUS_LIBAVFORMAT "not found")
endif()
if(AVUTIL_FOUND)
  set(STATUS_LIBAVUTIL "OK")
else()
  set(STATUS_LIBAVUTIL "not found")
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

set(LIB_CFLAGS "-std=c99 -pedantic -Werror -Wall -Werror=strict-prototypes -Werror=old-style-definition -Werror=missing-prototypes -D_REENTRANT -D_POSIX_C_SOURCE=200809L")
set(EXAMPLE_CFLAGS "-std=c99 -pedantic -Werror -Wall -g")
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
include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_BINARY_DIR})
target_link_libraries(groove LINK_PRIVATE
  ${AVCODEC_LIBRARIES}
  ${AVFILTER_LIBRARIES}
  ${AVFORMAT_LIBRARIES})
target_link_libraries(groove LINK_PUBLIC ${AVUTIL_LIBRARIES})
if(UNIX)
  target_link_libraries(groove LINK_PRIVATE m)
endif(UNIX)
include_directories(${FFMPEG_INCLUDE_DIRS})
target_link_libraries(groove LINK_PUBLIC ${CMAKE_THREAD_LIBS_INIT})


add_library(groove_static STATIC ${LIBGROOVE_SOURCES} ${LIBGROOVE_HEADERS})
set_target_properties(groove_static PROPERTIES
  OUTPUT_NAME groove
  COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
install(TARGETS groove_static DESTINATION lib)


install(FILES
  "groove/groove.h"
  "groove/queue.h"
  "groove/encoder.h"
  DESTINATION "include/groove")
install(TARGETS groove DESTINATION lib)

add_executable(metadata example/metadata.c)
set_target_properties(metadata PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(metadata groove)
add_dependencies(metadata groove)

add_executable(metadata_checksum example/metadata_checksum.c)
set_target_properties(metadata_checksum PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(metadata_checksum groove)
add_dependencies(metadata_checksum groove)

add_executable(transcode example/transcode.c)
set_target_properties(transcode PROPERTIES
  COMPILE_FLAGS ${EXAMPLE_CFLAGS})
include_directories(${EXAMPLE_INCLUDES})
target_link_libraries(transcode groove)
add_dependencies(transcode groove)


if(NOT DISABLE_PLAYER)
  add_library(grooveplayer SHARED
    ${LIBGROOVE_PLAYER_SOURCES}
    ${LIBGROOVE_PLAYER_HEADERS})
  set_target_properties(grooveplayer PROPERTIES
    SOVERSION ${LIBGROOVE_VERSION_MAJOR}
    VERSION ${LIBGROOVE_VERSION}
    COMPILE_FLAGS ${LIB_CFLAGS}
    )
  target_link_libraries(grooveplayer LINK_PUBLIC groove)
  if(UNIX AND NOT APPLE)
    target_link_libraries(grooveplayer LINK_PRIVATE rt)
  endif()
  add_dependencies(grooveplayer groove)
  target_link_libraries(grooveplayer LINK_PRIVATE ${SOUNDIO_LIBRARY})
  include_directories(${SOUNDIO_INCLUDE_DIR})

  install(FILES "grooveplayer/player.h" DESTINATION "include/grooveplayer")
  install(TARGETS grooveplayer DESTINATION lib)

  add_library(grooveplayer_static STATIC
    ${LIBGROOVE_PLAYER_SOURCES}
    ${LIBGROOVE_PLAYER_HEADERS})
  set_target_properties(grooveplayer_static PROPERTIES
    OUTPUT_NAME grooveplayer
    COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
  install(TARGETS grooveplayer_static DESTINATION lib)


  add_executable(playlist example/playlist.c ${PROJECT_SOURCE_DIR}/grooveplayer/player.h)
  set_target_properties(playlist PROPERTIES
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
  include_directories(${EXAMPLE_INCLUDES})
  target_link_libraries(playlist groove grooveplayer)
  add_dependencies(playlist groove grooveplayer)
endif()

if(NOT DISABLE_LOUDNESS)
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
  target_link_libraries(grooveloudness LINK_PRIVATE ${EBUR128_LIBRARY})
  include_directories(${EBUR128_INCLUDE_DIR})

  install(FILES "grooveloudness/loudness.h" DESTINATION "include/grooveloudness")
  install(TARGETS grooveloudness DESTINATION lib)


  add_library(grooveloudness_static STATIC
    ${LIBGROOVE_LOUDNESS_SOURCES}
    ${LIBGROOVE_LOUDNESS_HEADERS})
  set_target_properties(grooveloudness_static PROPERTIES
    OUTPUT_NAME grooveloudness
    COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
  install(TARGETS grooveloudness_static DESTINATION lib)


  add_executable(replaygain example/replaygain.c)
  set_target_properties(replaygain PROPERTIES
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
  include_directories(${EXAMPLE_INCLUDES})
  target_link_libraries(replaygain groove grooveloudness)
  add_dependencies(replaygain groove grooveloudness)
endif()

if(NOT DISABLE_FINGERPRINTER)
  add_library(groovefingerprinter SHARED
    ${LIBGROOVE_FINGERPRINTER_SOURCES}
    ${LIBGROOVE_FINGERPRINTER_HEADERS})
  set_target_properties(groovefingerprinter PROPERTIES
    SOVERSION ${LIBGROOVE_VERSION_MAJOR}
    VERSION ${LIBGROOVE_VERSION}
    COMPILE_FLAGS ${LIB_CFLAGS}
    )
  target_link_libraries(groovefingerprinter LINK_PUBLIC groove ${CMAKE_THREAD_LIBS_INIT})
  add_dependencies(groovefingerprinter groove)
  target_link_libraries(groovefingerprinter LINK_PRIVATE ${CHROMAPRINT_LIBRARY})
  include_directories(${CHROMAPRINT_INCLUDE_DIR})

  install(FILES "groovefingerprinter/fingerprinter.h" DESTINATION "include/groovefingerprinter")
  install(TARGETS groovefingerprinter DESTINATION lib)


  add_library(groovefingerprinter_static STATIC
    ${LIBGROOVE_FINGERPRINTER_SOURCES}
    ${LIBGROOVE_FINGERPRINTER_HEADERS})
  set_target_properties(groovefingerprinter_static PROPERTIES
    OUTPUT_NAME groovefingerprinter
    COMPILE_FLAGS "${LIB_CFLAGS} -fPIC")
  install(TARGETS groovefingerprinter_static DESTINATION lib)


  add_executable(fingerprint example/fingerprint.c)
  set_target_properties(fingerprint PROPERTIES
    COMPILE_FLAGS ${EXAMPLE_CFLAGS})
  include_directories(${EXAMPLE_INCLUDES})
  target_link_libraries(fingerprint groove groovefingerprinter)
  add_dependencies(fingerprint groove groovefingerprinter)
endif()

message("\n"
"Installation Summary\n"
"--------------------\n"
"* Install Directory            : ${CMAKE_INSTALL_PREFIX}\n"
"* Build libgroove              : ${LIBGROOVE_STATUS}\n"
"* Build libgrooveplayer        : ${LIBGROOVE_PLAYER_STATUS}\n"
"* Build libgrooveloudness      : ${LIBGROOVE_LOUDNESS_STATUS}\n"
"* Build libgroovefingerprinter : ${LIBGROOVE_FINGERPRINTER_STATUS}\n"
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
"* libavutil                    : ${STATUS_LIBAVUTIL}"
)

message("\n"
"If everything is looks good, proceed with\n"
"make\n"
)
