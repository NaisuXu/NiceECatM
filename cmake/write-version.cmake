# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED NICEECATM_SOURCE_DIR OR NOT DEFINED NICEECATM_BINARY_DIR)
  message(FATAL_ERROR "Version generation requires source and binary directories")
endif()

# Local build time; string(TIMESTAMP) also honors SOURCE_DATE_EPOCH for
# reproducible release builds. All version consumers share this one value.
string(TIMESTAMP NICEECATM_VERSION "%Y%m%d%H%M")
string(SUBSTRING "${NICEECATM_VERSION}" 0 4 NICEECATM_VERSION_YEAR)
string(SUBSTRING "${NICEECATM_VERSION}" 4 4 NICEECATM_VERSION_MONTHDAY)
string(SUBSTRING "${NICEECATM_VERSION}" 8 4 NICEECATM_VERSION_HOURMINUTE)
foreach(component IN ITEMS MONTHDAY HOURMINUTE)
  string(REGEX REPLACE "^0+" "" NICEECATM_VERSION_${component} "${NICEECATM_VERSION_${component}}")
  if("${NICEECATM_VERSION_${component}}" STREQUAL "")
    set(NICEECATM_VERSION_${component} 0)
  endif()
endforeach()

file(MAKE_DIRECTORY "${NICEECATM_BINARY_DIR}/generated")
configure_file("${NICEECATM_SOURCE_DIR}/cmake/build_version.hpp.in"
  "${NICEECATM_BINARY_DIR}/generated/build_version.hpp" @ONLY)
configure_file("${NICEECATM_SOURCE_DIR}/resources/windows/app.rc.in"
  "${NICEECATM_BINARY_DIR}/app.rc" @ONLY)
