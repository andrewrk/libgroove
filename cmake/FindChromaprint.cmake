# Copyright (c) 2014 Andrew Kelley
# This file is MIT licensed.
# See http://opensource.org/licenses/MIT

# CHROMAPRINT_FOUND
# CHROMAPRINT_INCLUDE_DIR
# CHROMAPRINT_LIBRARY

find_path(CHROMAPRINT_INCLUDE_DIR NAMES chromaprint.h)

find_library(CHROMAPRINT_LIBRARY NAMES chromaprint)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CHROMAPRINT DEFAULT_MSG CHROMAPRINT_LIBRARY CHROMAPRINT_INCLUDE_DIR)

mark_as_advanced(CHROMAPRINT_INCLUDE_DIR CHROMAPRINT_LIBRARY)
