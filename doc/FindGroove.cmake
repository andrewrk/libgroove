# Copyright (c) 2015 Andrew Kelley
# This file is MIT licensed.
# See http://opensource.org/licenses/MIT

# GROOVE_FOUND
# GROOVE_INCLUDE_DIR
# GROOVE_LIBRARY

find_path(GROOVE_INCLUDE_DIR NAMES groove/groove.h)

find_library(GROOVE_LIBRARY NAMES groove)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GROOVE DEFAULT_MSG GROOVE_LIBRARY GROOVE_INCLUDE_DIR)

mark_as_advanced(GROOVE_INCLUDE_DIR GROOVE_LIBRARY)
