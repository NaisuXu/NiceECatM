# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.28)
project(NiceECatM LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NICEECATM_BUILD_GUI "Build the SDL3 / ImGui desktop workbench" ON)
option(NICEECATM_BUILD_TESTS "Build native core tests" ON)
include(cmake/dependencies.cmake)

add_library(niceecatm_core STATIC
  src/model/types.cpp
  src/model/pdo.cpp
  src/platform/paths.cpp
  src/platform/update_check.cpp
  src/platform/update_transport.cpp
  src/esi/catalog.cpp
  src/master/controller.cpp
  src/script/bridge.cpp
  src/script/runtime.cpp
  src/ethercat/simulation.cpp
  src/ethercat/soem_backend.cpp)
target_include_directories(niceecatm_core PUBLIC src)
target_link_libraries(niceecatm_core PUBLIC soem Threads::Threads PRIVATE niceecatm_xml niceecatm_lua)
if(WIN32)
  target_link_libraries(niceecatm_core PRIVATE winhttp)
elseif(UNIX)
  # Updates are optional: offline builds still have the Releases page link.
  find_package(CURL QUIET)
  if(CURL_FOUND)
    target_link_libraries(niceecatm_core PRIVATE CURL::libcurl)
    target_compile_definitions(niceecatm_core PRIVATE NICEECATM_HAVE_CURL=1)
  endif()
endif()
if(MSVC)
  target_compile_options(niceecatm_core PRIVATE /utf-8 /W4)
else()
  target_compile_options(niceecatm_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(niceecatm src/app/main.cpp src/app/cli.cpp)
set_target_properties(niceecatm PROPERTIES OUTPUT_NAME "NiceECatM")
# Refresh the portable version for every build, including incremental builds.
# The generator only replaces files when the build minute changes.
set(NICEECATM_VERSION_COMMAND "${CMAKE_COMMAND}"
  "-DNICEECATM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DNICEECATM_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write-version.cmake")
execute_process(COMMAND ${NICEECATM_VERSION_COMMAND} COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(niceecatm_build_version
  COMMAND ${NICEECATM_VERSION_COMMAND}
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/build_version.hpp" "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
  VERBATIM)
add_dependencies(niceecatm niceecatm_build_version)
target_include_directories(niceecatm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(WIN32)
  enable_language(RC)
  target_sources(niceecatm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/app.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/NiceECatM.ico")
endif()
target_link_libraries(niceecatm PRIVATE niceecatm_core)
if(NICEECATM_BUILD_GUI)
  # Font data is checked in, so font tooling is not a build prerequisite.
  target_sources(niceecatm PRIVATE
    src/ui/workbench.cpp
    src/ui/components.cpp
    src/ui/shell.cpp
    src/ui/pdo_page.cpp
    src/ui/service_pages.cpp
    src/ui/script_page.cpp
    src/app/desktop.cpp
    resources/fonts/workbench_font.cpp
    resources/fonts/cousine_font.cpp
    third_party/imgui_color_text_edit/TextEditor.cpp)
  target_include_directories(niceecatm PRIVATE third_party/imgui_color_text_edit)
  target_link_libraries(niceecatm PRIVATE niceecatm_imgui)
  target_compile_definitions(niceecatm PRIVATE NICEECATM_GUI=1)
endif()
if(MINGW)
  target_link_options(niceecatm PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
  target_compile_options(niceecatm PRIVATE /utf-8)
endif()
include(cmake/tests.cmake)

install(TARGETS niceecatm RUNTIME DESTINATION .)
install(FILES LICENSE DESTINATION .)
install(FILES resources/release/README.md resources/release/README.zh-CN.md
  resources/release/THIRD_PARTY.md DESTINATION .)
install(FILES resources/fonts/OFL.txt DESTINATION licenses RENAME NotoSansSC-OFL.txt)
install(FILES resources/fonts/Cousine-OFL.txt DESTINATION licenses)
install(FILES ${soem_SOURCE_DIR}/LICENSE.md DESTINATION licenses RENAME SOEM-LICENSE.md)
install(FILES third_party/tinyxml2/LICENSE.txt DESTINATION licenses RENAME TinyXML2-LICENSE.txt)
install(FILES third_party/lua/COPYRIGHT DESTINATION licenses RENAME Lua-COPYRIGHT.txt)
install(FILES third_party/imgui_color_text_edit/LICENSE DESTINATION licenses RENAME ImGuiColorTextEdit-LICENSE.txt)
install(DIRECTORY resources/esi/ DESTINATION esi FILES_MATCHING PATTERN "*.xml")
install(DIRECTORY resources/examples/ DESTINATION scripts FILES_MATCHING PATTERN "*.lua")
if(NICEECATM_BUILD_GUI)
  install(FILES ${sdl_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME SDL-LICENSE.txt)
  install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME ImGui-LICENSE.txt)
endif()
