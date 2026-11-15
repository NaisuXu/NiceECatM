# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

# All code dependencies are vendored at fixed revisions in third_party.
# Only their generated headers and compiled outputs belong in this build.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static dependencies" FORCE)
find_package(Threads REQUIRED)

set(soem_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/soem")
set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${soem_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/soem-build" EXCLUDE_FROM_ALL SYSTEM)
set(NICEECATM_SOEM_PATCHED_SOURCE_DIR "${CMAKE_BINARY_DIR}/generated/soem")
include("${CMAKE_CURRENT_LIST_DIR}/patches/soem-sdo.cmake")
get_target_property(_niceecatm_soem_sources soem SOURCES)
list(FILTER _niceecatm_soem_sources EXCLUDE REGEX "^src/ec_coe\\.c$")
list(APPEND _niceecatm_soem_sources "${NICEECATM_SOEM_PATCHED_SOURCE_DIR}/ec_coe.c")
set_property(TARGET soem PROPERTY SOURCES "${_niceecatm_soem_sources}")

if(WIN32)
  # Resolve Npcap at runtime so GUI/simulation work without a capture driver.
  get_target_property(_niceecatm_soem_links soem LINK_LIBRARIES)
  list(FILTER _niceecatm_soem_links EXCLUDE REGEX "[Ww][Pp][Cc][Aa][Pp]\\.lib|Packet\\.lib|ws2_32\\.lib|winmm\\.lib")
  set_property(TARGET soem PROPERTY LINK_LIBRARIES "${_niceecatm_soem_links}")
  set_property(TARGET soem PROPERTY INTERFACE_LINK_LIBRARIES "${_niceecatm_soem_links}")
  target_sources(soem PRIVATE "${PROJECT_SOURCE_DIR}/src/platform/pcap_runtime.c")
  target_include_directories(soem PRIVATE "${PROJECT_SOURCE_DIR}/src/platform")
  target_link_libraries(soem PUBLIC ws2_32 winmm iphlpapi)
  get_target_property(_niceecatm_soem_options soem INTERFACE_COMPILE_OPTIONS)
  list(FILTER _niceecatm_soem_options EXCLUDE REGEX "std=c11")
  set_property(TARGET soem PROPERTY INTERFACE_COMPILE_OPTIONS "${_niceecatm_soem_options}")
endif()

add_library(niceecatm_xml STATIC "${PROJECT_SOURCE_DIR}/third_party/tinyxml2/tinyxml2.cpp")
target_include_directories(niceecatm_xml PUBLIC "${PROJECT_SOURCE_DIR}/third_party/tinyxml2")

# Embed only the Lua libraries exposed by the script runtime.
set(_niceecatm_lua_dir "${PROJECT_SOURCE_DIR}/third_party/lua")
set(_niceecatm_lua_sources
  lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldebug.c ldo.c ldump.c
  lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c
  lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
  lutf8lib.c lvm.c lzio.c)
list(TRANSFORM _niceecatm_lua_sources PREPEND "${_niceecatm_lua_dir}/")
add_library(niceecatm_lua STATIC ${_niceecatm_lua_sources})
target_include_directories(niceecatm_lua PUBLIC "${_niceecatm_lua_dir}")
if(MSVC)
  target_compile_options(niceecatm_lua PRIVATE /W0)
else()
  target_compile_options(niceecatm_lua PRIVATE -w)
endif()

if(NICEECATM_BUILD_GUI)
  set(sdl_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/sdl")
  set(imgui_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")
  # Identify the pinned SDL source even when this project has its own .git.
  set(SDL_REVISION "SDL3-3.2.20-96292a5b464258a2b926e0a3d72f8b98c2a81aa6" CACHE STRING "Pinned SDL revision" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  foreach(feature AUDIO CAMERA JOYSTICK HAPTIC SENSOR DIALOG GPU VULKAN OPENGL)
    set(SDL_${feature} OFF CACHE BOOL "" FORCE)
  endforeach()
  add_subdirectory("${sdl_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/sdl-build" EXCLUDE_FROM_ALL SYSTEM)
  # MinGW GCC can ICE on SDL's generated precompiled header.
  if(MINGW AND TARGET SDL3-static)
    set_property(TARGET SDL3-static PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
  endif()
  add_library(niceecatm_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(niceecatm_imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
  target_link_libraries(niceecatm_imgui PUBLIC SDL3::SDL3-static)
endif()
