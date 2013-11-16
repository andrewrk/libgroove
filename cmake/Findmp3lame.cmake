# Locate libmp3lame library  
# This module defines
# MP3LAME_LIBRARY, the name of the library to link against
# MP3LAME_FOUND, if false, do not try to link
# MP3LAME_INCLUDE_DIR, where to find header
#

set( MP3LAME_FOUND "NO" )

find_path( MP3LAME_INCLUDE_DIR lame/lame.h
  HINTS
  PATH_SUFFIXES include 
  PATHS
  ~/Library/Frameworks
  /Library/Frameworks
  /usr/local/include
  /usr/include
  /sw/include
  /opt/local/include
  /opt/csw/include 
  /opt/include
  /mingw
)

find_library( MP3LAME_LIBRARY
  NAMES mp3lame
  HINTS
  PATH_SUFFIXES lib64 lib
  PATHS
  /usr/local
  /usr
  /sw
  /opt/local
  /opt/csw
  /opt
  /mingw
)

if(MP3LAME_LIBRARY)
set( MP3LAME_FOUND "YES" )
endif(MP3LAME_LIBRARY)
