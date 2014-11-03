# Copyright (c) 2014 Andrew Kelley
# This file is MIT licensed.
# See http://opensource.org/licenses/MIT

# SDL2_FOUND
# SDL2_INCLUDE_DIR
# SDL2_LIBRARY

find_path(SDL2_INCLUDE_DIR NAMES SDL2/SDL.h)

find_library(SDL2_LIBRARY NAMES SDL2)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2 DEFAULT_MSG SDL2_LIBRARY SDL2_INCLUDE_DIR)

mark_as_advanced(SDL2_INCLUDE_DIR SDL2_LIBRARY)
