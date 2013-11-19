# +-----------------------------------------------------------------------------+
# | $Id::                                                                     $ |
# +-----------------------------------------------------------------------------+
# |   Copyright (C) 2007                                                        |
# |   Lars B"ahren (bahren@astron.nl)                                           |
# |                                                                             |
# |   This program is free software; you can redistribute it and/or modify      |
# |   it under the terms of the GNU General Public License as published by      |
# |   the Free Software Foundation; either version 2 of the License, or         |
# |   (at your option) any later version.                                       |
# |                                                                             |
# |   This program is distributed in the hope that it will be useful,           |
# |   but WITHOUT ANY WARRANTY; without even the implied warranty of            |
# |   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             |
# |   GNU General Public License for more details.                              |
# |                                                                             |
# |   You should have received a copy of the GNU General Public License         |
# |   along with this program; if not, write to the                             |
# |   Free Software Foundation, Inc.,                                           |
# |   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.                 |
# +-----------------------------------------------------------------------------+

# - Check for the presence of YASM
#
# The following variables are set when YASM is found:
#  YASM_FOUND      = Set to true, if all components of YASM
#                         have been found.
#  YASM_INCLUDES   = Include path for the header files of YASM
#  YASM_LIBRARIES  = Link these to use YASM
#  YASM_LFLAGS     = Linker flags (optional)

if (NOT YASM_FOUND)
    
  ##_____________________________________________________________________________
  ## Check for the header files
  
  find_path (YASM_INCLUDES libyasm.h libyasm/valparam.h
    PATHS /sw /usr /usr/local /opt/local ${CMAKE_INSTALL_PREFIX}
    PATH_SUFFIXES include include/libyasm include/yasm
    )
  
  ##_____________________________________________________________________________
  ## Check for the library
  
  find_library (YASM_LIBRARIES yasm
    PATHS /sw /usr /usr/local /opt/local ${CMAKE_INSTALL_PREFIX}
    PATH_SUFFIXES lib
    )
  
  ##_____________________________________________________________________________
  ## Check for the executable
  
  find_program (YASM_EXECUTABLE yasm
    PATHS /sw /usr /usr/local /opt/local ${CMAKE_INSTALL_PREFIX}
    PATH_SUFFIXES bin
    )
  
  ##_____________________________________________________________________________
  ## Actions taken when all components have been found
  
  if (YASM_INCLUDES AND YASM_LIBRARIES)
    set (YASM_FOUND TRUE)
  else (YASM_INCLUDES AND YASM_LIBRARIES)
    set (YASM_FOUND FALSE)
    if (NOT YASM_FIND_QUIETLY)
      if (NOT YASM_INCLUDES)
	message (STATUS "Unable to find YASM header files!")
      endif (NOT YASM_INCLUDES)
      if (NOT YASM_LIBRARIES)
	message (STATUS "Unable to find YASM library files!")
      endif (NOT YASM_LIBRARIES)
    endif (NOT YASM_FIND_QUIETLY)
  endif (YASM_INCLUDES AND YASM_LIBRARIES)
  
  if (YASM_FOUND)
    if (NOT YASM_FIND_QUIETLY)
      message (STATUS "Found components for Yasm")
      message (STATUS "YASM_INCLUDES  = ${YASM_INCLUDES}")
      message (STATUS "YASM_LIBRARIES = ${YASM_LIBRARIES}")
    endif (NOT YASM_FIND_QUIETLY)
  else (YASM_FOUND)
    if (YASM_FIND_REQUIRED)
      message (FATAL_ERROR "Could not find YASM!")
    endif (YASM_FIND_REQUIRED)
  endif (YASM_FOUND)
  
  ##_____________________________________________________________________________
  ## Mark advanced variables
  
  mark_as_advanced (
    YASM_INCLUDES
    YASM_LIBRARIES
    YASM_EXECUTABLE
    )
  
endif (NOT YASM_FOUND)
