# copied from https://bitbucket.org/acoustid/acoustid-fingerprinter
# GPLv2 license
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE(PkgConfig)
PKG_CHECK_MODULES(PKG_LIBCHROMAPRINT libchromaprint)

FIND_PATH(CHROMAPRINT_INCLUDE_DIR chromaprint.h
    ${PKG_LIBCHROMAPRINT_INCLUDE_DIRS}
    /usr/include
    /usr/local/include
)

FIND_LIBRARY(CHROMAPRINT_LIBRARIES
    NAMES
    chromaprint chromaprint.dll
    PATHS
    ${PKG_LIBCHROMAPRINT_LIBRARY_DIRS}
    /usr/lib
    /usr/local/lib
)

FIND_PACKAGE_HANDLE_STANDARD_ARGS(Chromaprint DEFAULT_MSG CHROMAPRINT_LIBRARIES CHROMAPRINT_INCLUDE_DIR)
