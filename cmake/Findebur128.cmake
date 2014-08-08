# Copyright (c) 2014 Andrew Kelley
# This file is MIT licensed.
# See http://opensource.org/licenses/MIT

# EBUR128_FOUND
# EBUR128_INCLUDE_DIR
# EBUR128_LIBRARY

find_path(EBUR128_INCLUDE_DIR NAMES ebur128.h)

find_library(EBUR128_LIBRARY NAMES ebur128)

if(EBUR128_LIBRARY AND EBUR128_INCLUDE_DIR)
  set(EBUR128_FOUND TRUE)
else()
  set(EBUR128_FOUND FALSE)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EBUR128 DEFAULT_MSG EBUR128_LIBRARY EBUR128_INCLUDE_DIR)

mark_as_advanced(EBUR128_INCLUDE_DIR EBUR128_LIBRARY)
