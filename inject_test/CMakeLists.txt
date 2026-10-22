# Minimal injection-test version.dll (separate from the full mod build).
# Reuses the export forwarding from ../proxy; own DllMain in main.cpp.
# Build with inject_test\build.ps1 -> dist\InjectTest\version.dll
cmake_minimum_required(VERSION 3.21)

project(sr2mods_injecttest VERSION 0.1.0 LANGUAGES C CXX ASM_MASM)

if(NOT MSVC)
  message(FATAL_ERROR "Must be built with MSVC (cl.exe).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Must be built for x64 (run from an x64 VS developer environment).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PROXY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../proxy)

add_library(injecttest SHARED
  main.cpp
  ${PROXY_DIR}/proxy.cpp
  ${PROXY_DIR}/proxy.h
  ${PROXY_DIR}/proxy_exports.inc
  ${PROXY_DIR}/version_stubs.asm
  ${PROXY_DIR}/version.def
)

set_target_properties(injecttest PROPERTIES
  OUTPUT_NAME "version"
  PREFIX ""
)

target_include_directories(injecttest PRIVATE ${PROXY_DIR})

target_compile_definitions(injecttest PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

target_compile_options(injecttest PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zi>
)
set_source_files_properties(${PROXY_DIR}/version_stubs.asm PROPERTIES COMPILE_OPTIONS "/W3")

target_link_options(injecttest PRIVATE
  /DEBUG
  $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
  /NODEFAULTLIB:version.lib   # we ARE version.dll - never import ourselves
)
