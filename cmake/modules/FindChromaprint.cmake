include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
  pkg_check_modules(CHROMAPRINT libchromaprint QUIET)
endif ()

if ( NOT CHROMAPRINT_FOUND )
  find_path(CHROMAPRINT_INCLUDE_DIRS NAMES chromaprint.h)
  find_library(CHROMAPRINT_LIBRARIES NAMES chromaprint)

  if ( NOT "${CHROMAPRINT_LIBRARIES}" STREQUAL "")
    set (CHROMAPRINT_FOUND TRUE)
    set (CHROMAPRINT_LINK_LIBRARIES ${CHROMAPRINT_LINK_LIBRARIES} ${CHROMAPRINT_LIBRARIES})
  endif ()
endif ()

if ( CHROMAPRINT_INCLUDEDIR AND NOT CHROMAPRINT_INCLUDE_DIRS )
  set (CHROMAPRINT_INCLUDE_DIRS ${CHROMAPRINT_INCLUDEDIR})
endif ()

find_package_handle_standard_args(Chromaprint
  FOUND_VAR
    CHROMAPRINT_FOUND
  REQUIRED_VARS
    CHROMAPRINT_LINK_LIBRARIES
    CHROMAPRINT_INCLUDE_DIRS
  VERSION_VAR
    CHROMAPRINT_VERSION)