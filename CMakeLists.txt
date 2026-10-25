cmake_minimum_required(VERSION 3.21)

project(sr2mods VERSION 0.2.0 LANGUAGES C CXX ASM_MASM)

if(NOT MSVC)
  message(FATAL_ERROR "sr2mods must be built with MSVC (cl.exe).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "sr2mods must be built for x64 (run from an x64 VS developer environment).")
endif()

# Version reported in the log and the overlay. Local builds use the project version; the release
# workflow (.github/workflows/release.yml) sets the environment variable SR2MODS_VERSION_STRING
# (e.g. 0.3 or dev-20261026-1412-315cf6f) before build.ps1, or pass -DSR2MODS_VERSION_STRING=...
if(DEFINED ENV{SR2MODS_VERSION_STRING} AND NOT "$ENV{SR2MODS_VERSION_STRING}" STREQUAL "")
  set(SR2_DEFAULT_VERSION "$ENV{SR2MODS_VERSION_STRING}")
else()
  set(SR2_DEFAULT_VERSION "${PROJECT_VERSION}")
endif()
set(SR2MODS_VERSION_STRING "${SR2_DEFAULT_VERSION}" CACHE STRING "Version string compiled into the DLL")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT everywhere (/MT, /MTd) - the DLL must not depend on VC++ redists.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --------------------------------------------------------------- MinHook ----
# Official TsudaKageyu/minhook sources vendored in third_party/minhook,
# compiled in as a static library.
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_options(minhook PRIVATE /W3)

# ----------------------------------------------------------- song codecs ----
# stb_vorbis (third_party/stb) and dr_mp3 / dr_wav (third_party/dr_libs), compiled once in
# modules/slime_dj/codecs.c as C with warnings off (third-party code). Plain C runtime only.
add_library(sr2_codecs STATIC modules/slime_dj/codecs.c)
target_compile_options(sr2_codecs PRIVATE /W0)

# ------------------------------------------------------------ Dear ImGui ----
# Vendored by the overlay owner into third_party/imgui (optional). Built as a
# static library with warnings off (third-party code); D3D DLLs are
# delay-loaded below so the proxy never pulls them in at load time.
set(SR2_HAVE_IMGUI OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp")
  set(SR2_HAVE_IMGUI ON)
  file(GLOB IMGUI_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui*.cpp")
  set(IMGUI_BACKEND_SOURCES)
  foreach(b imgui_impl_dx11.cpp imgui_impl_win32.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/${b}")
      list(APPEND IMGUI_BACKEND_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/${b}")
    endif()
  endforeach()
  add_library(imgui STATIC ${IMGUI_CORE_SOURCES} ${IMGUI_BACKEND_SOURCES})
  target_include_directories(imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends")
  target_compile_definitions(imgui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(imgui PRIVATE /W0 /utf-8)
  message(STATUS "sr2mods: Dear ImGui found (${IMGUI_BACKEND_SOURCES})")
else()
  message(STATUS "sr2mods: third_party/imgui not present - building without Dear ImGui")
endif()

# ---------------------------------------------------------- version.dll -----
# Modules and UI are owned by separate authors: pick up whatever they add.
file(GLOB_RECURSE SR2_MODULE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/modules/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/modules/*.h")
file(GLOB_RECURSE SR2_UI_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.h")

add_library(sr2mods SHARED
  proxy/dllmain.cpp
  proxy/proxy.cpp
  proxy/proxy.h
  proxy/proxy_exports.inc
  proxy/version_stubs.asm
  proxy/version.def

  core/core.cpp
  core/core.h
  core/log.cpp
  core/log.h
  core/paths.cpp
  core/paths.h
  core/json.cpp
  core/json.h
  core/config.cpp
  core/config.h
  core/il2cpp_api.cpp
  core/il2cpp_api.h
  core/resolver.cpp
  core/resolver.h
  core/hook_manager.cpp
  core/hook_manager.h
  core/guard.cpp
  core/guard.h
  core/detour_guard.cpp
  core/detour_guard.h
  core/main_thread.cpp
  core/main_thread.h
  core/perf.cpp
  core/perf.h
  core/settings.cpp
  core/settings.h
  core/settings_internal.h
  core/profiles.cpp
  core/profiles.h
  core/unity.cpp
  core/unity.h
  core/module.h
  core/module_manager.cpp
  core/module_manager.h
  core/module_registry.cpp
  core/version_guard.cpp
  core/version_guard.h
  core/text_override.cpp
  core/text_override.h
  core/build_info.cpp
  core/build_info.h

  ${SR2_MODULE_SOURCES}
  ${SR2_UI_SOURCES}
)

set_target_properties(sr2mods PROPERTIES
  OUTPUT_NAME "version"
  PREFIX ""
)

target_include_directories(sr2mods PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Build stamp (time + short git commit, shown in the overlay sidebar): regenerated on
# every build, not just on configure. The target is always out of date; the script
# rewrites the header only when its text changes, and BYPRODUCTS lets Ninja notice
# that and recompile core/build_info.cpp (the only file including it).
set(SR2_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(sr2mods_build_info
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${SR2_GEN_DIR}/sr2mods_build_info.h" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake"
  BYPRODUCTS "${SR2_GEN_DIR}/sr2mods_build_info.h"
  COMMENT "Stamping build time"
  VERBATIM)
add_dependencies(sr2mods sr2mods_build_info)
target_include_directories(sr2mods PRIVATE "${SR2_GEN_DIR}")
target_link_libraries(sr2mods PRIVATE minhook sr2_codecs bcrypt delayimp)

# Every system DLL beyond kernel32/user32 is delay-loaded: the proxy is loaded
# by UnityPlayer.dll very early, before the game initialises D3D, and must not
# drag anything in at that point. (/IGNORE:4199 = "no imports found" for a
# DLL that a given build does not happen to call.)
set(SR2_DELAYLOAD bcrypt.dll)
if(SR2_HAVE_IMGUI)
  target_link_libraries(sr2mods PRIVATE imgui d3d11 dxgi)
  target_compile_definitions(sr2mods PRIVATE SR2_HAVE_IMGUI=1)
  # imgui.cpp also calls ShellExecuteW (shell32) and Imm* (imm32).
  list(APPEND SR2_DELAYLOAD d3d11.dll dxgi.dll d3dcompiler_47.dll dwmapi.dll shell32.dll imm32.dll)
endif()
foreach(dll ${SR2_DELAYLOAD})
  target_link_options(sr2mods PRIVATE "/DELAYLOAD:${dll}")
endforeach()

target_compile_definitions(sr2mods PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  SR2MODS_VERSION="${SR2MODS_VERSION_STRING}"
)

target_compile_options(sr2mods PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus>
)
set_source_files_properties(proxy/version_stubs.asm PROPERTIES COMPILE_OPTIONS "/W3")

target_link_options(sr2mods PRIVATE
  /DEBUG              # always emit a PDB (crash triage), Release stays optimised
  $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
  /NODEFAULTLIB:version.lib   # we ARE version.dll - never import ourselves
  /IGNORE:4199
)
