# =============================================================================
# mcut-boolean-viewer — Dependency Build Script
#
# Design inspired by OrcaSlicer's deps/CMakeLists.txt:
#   https://github.com/SoftFever/OrcaSlicer/blob/main/deps/CMakeLists.txt
#
# Key design principles (same as OrcaSlicer):
#   1. Uses CMake ExternalProject_Add to download, build, and install each dep
#   2. All deps land in a single DESTDIR (Unix-style prefix: include/ lib/ bin/)
#   3. A helper function (mcut_add_cmake_dep) forwards compiler, platform,
#      install-prefix, and build-type to every sub-project — identical to
#      OrcaSlicer's orcaslicer_add_cmake_project()
#   4. The main project only needs -DCMAKE_PREFIX_PATH pointing at DESTDIR
#   5. No vcpkg, no system package manager, no manual downloads required
#
# Dependencies built:
#   - MCUT   1.3.0  (mesh boolean operations)
#   - GLFW   3.4    (window + OpenGL context + input)
#   - GLM    1.0.1  (header-only math library)
#   - ImGui  1.91.6 (immediate-mode GUI, built as static lib)
#   - GLAD           (OpenGL 3.3 Core loader, pre-generated sources bundled)
#
# Usage — out-of-source build (strongly recommended):
#
#   Linux / macOS:
#     cd <repo-root>
#     mkdir deps_build && cd deps_build
#     cmake ../deps -DCMAKE_BUILD_TYPE=Release
#     cmake --build . -j$(nproc)
#
#   Windows (x64 Native Tools Command Prompt for VS):
#     cd <repo-root>
#     mkdir deps_build && cd deps_build
#     cmake ..\deps
#     cmake --build . --config Release
#
# After building, configure the main project:
#   cmake <src> -DCMAKE_PREFIX_PATH=<abs-path>/deps_build/install
# =============================================================================

cmake_minimum_required(VERSION 3.16)

if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
endif()

if(POLICY CMP0135)   # DOWNLOAD_EXTRACT_TIMESTAMP — suppress warning
    cmake_policy(SET CMP0135 NEW)
endif()

project(mcut-boolean-viewer-deps LANGUAGES C CXX)

include(ExternalProject)
include(ProcessorCount)

# ---------------------------------------------------------------------------
# Processor count (for parallel sub-builds — same pattern as OrcaSlicer)
# ---------------------------------------------------------------------------
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 1)
endif()

# ---------------------------------------------------------------------------
# Install prefix — all deps land here (mirrors OrcaSlicer's DESTDIR/usr/local)
# ---------------------------------------------------------------------------
if("${DESTDIR}" STREQUAL "")
    set(DESTDIR "${CMAKE_BINARY_DIR}/install"
        CACHE PATH "Root directory where all dependencies will be installed." FORCE)
endif()

message(STATUS "")
message(STATUS "=== mcut-boolean-viewer deps ===")
message(STATUS "  Install prefix : ${DESTDIR}")
message(STATUS "  Parallel jobs  : ${NPROC}")

# ---------------------------------------------------------------------------
# Download cache — avoids re-downloading on clean builds
# ---------------------------------------------------------------------------
set(DEP_DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/DL_CACHE"
    CACHE PATH "Directory for cached source archives.")
message(STATUS "  Download cache : ${DEP_DOWNLOAD_DIR}")

# ---------------------------------------------------------------------------
# Multi-config detection (Visual Studio, Xcode)
# ---------------------------------------------------------------------------
get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(NOT _is_multi AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message(STATUS "  Build type     : Release (defaulted)")
else()
    message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "================================")
message(STATUS "")

# Parallel flag forwarded to sub-project cmake --build
if(MSVC)
    set(_build_j "/m")
else()
    set(_build_j "-j${NPROC}")
endif()

# Generator forwarding for MSVC (keeps sub-projects on the same toolset)
if(MSVC)
    set(_msvc_gen
        CMAKE_GENERATOR           "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM  "${CMAKE_GENERATOR_PLATFORM}"
    )
else()
    set(_msvc_gen "")
endif()

# Build-type line for single-config generators
if(_is_multi OR MSVC)
    set(_cfg_line "")
else()
    set(_cfg_line "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}")
endif()

# macOS deployment target forwarding
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    set(_osx_target "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
    set(_osx_target "")
endif()

# ---------------------------------------------------------------------------
# mcut_add_cmake_dep — mirrors OrcaSlicer's orcaslicer_add_cmake_project()
#
# Parameters:
#   NAME       <name>          Target name (dep_<name> will be created)
#   URL        <url>           Source archive URL
#   URL_HASH   <SHA256=hash>   Archive integrity check
#   CMAKE_ARGS <arg>...        Extra cmake -D flags for the sub-project
#   DEPENDS    <dep>...        Other dep_<x> targets this one depends on
# ---------------------------------------------------------------------------
function(mcut_add_cmake_dep)
    cmake_parse_arguments(_D "" "NAME;URL;URL_HASH" "CMAKE_ARGS;DEPENDS" ${ARGN})

    ExternalProject_Add(dep_${_D_NAME}
        URL             "${_D_URL}"
        URL_HASH        "${_D_URL_HASH}"
        DOWNLOAD_DIR    "${DEP_DOWNLOAD_DIR}/${_D_NAME}"
        INSTALL_DIR     "${DESTDIR}"
        ${_msvc_gen}
        DEPENDS         ${_D_DEPENDS}
        CMAKE_ARGS
            # --- install destination (same as OrcaSlicer) ---
            -DCMAKE_INSTALL_PREFIX:PATH=${DESTDIR}
            -DCMAKE_PREFIX_PATH:PATH=${DESTDIR}
            # --- compiler forwarding ---
            -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
            # --- static libs only ---
            -DBUILD_SHARED_LIBS:BOOL=OFF
            # --- build type ---
            ${_cfg_line}
            # --- macOS ---
            ${_osx_target}
            # --- caller's extra args ---
            ${_D_CMAKE_ARGS}
        BUILD_COMMAND
            ${CMAKE_COMMAND} --build . --config Release -- ${_build_j}
        INSTALL_COMMAND
            ${CMAKE_COMMAND} --build . --target install --config Release
        # Log to files so the top-level output stays readable
        # (same recommendation as OrcaSlicer: use -j1 at top level)
        LOG_DOWNLOAD  ON
        LOG_CONFIGURE ON
        LOG_BUILD     ON
        LOG_INSTALL   ON
    )
endfunction()

# ===========================================================================
# 1. MCUT — mesh boolean operations
#    https://github.com/cutdigital/mcut
# ===========================================================================
# MCUT installs its static lib into lib/static/ (non-standard).
# We use a custom ExternalProject_Add (not the helper function) so we can
# add a post-install step that:
#   1. Copies lib/static/libmcut.a → lib/libmcut.a
#   2. Generates a proper CMake package config (mcut-config.cmake)
# This mirrors OrcaSlicer's pattern of patching non-standard deps.
ExternalProject_Add(dep_MCUT
    URL          https://github.com/cutdigital/mcut/archive/refs/tags/v1.3.0.tar.gz
    URL_HASH     SHA256=5f33a8196f2ea4cb700b7ceb493c0674ab7f98fec415cbf04d546366bcc566a2
    DOWNLOAD_DIR "${DEP_DOWNLOAD_DIR}/MCUT"
    INSTALL_DIR  "${DESTDIR}"
    ${_msvc_gen}
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${DESTDIR}
        -DCMAKE_PREFIX_PATH:PATH=${DESTDIR}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DMCUT_BUILD_TUTORIALS:BOOL=OFF
        -DMCUT_BUILD_TESTS:BOOL=OFF
        -DMCUT_BUILD_AS_SHARED_LIB:BOOL=OFF
        ${_cfg_line}
        ${_osx_target}
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build . --config Release -- ${_build_j}
    # ExternalProject only supports one INSTALL_COMMAND.
    # Use cmake -P to run a wrapper script that does both install + post-install.
    INSTALL_COMMAND
        ${CMAKE_COMMAND}
            -DBINARY_DIR=<BINARY_DIR>
            -DDESTDIR=${DESTDIR}
            -DCMAKE_COMMAND=${CMAKE_COMMAND}
            -P "${CMAKE_SOURCE_DIR}/MCUT/install_and_postinstall.cmake"
    LOG_DOWNLOAD  ON
    LOG_CONFIGURE ON
    LOG_BUILD     ON
    LOG_INSTALL   ON
)

# ===========================================================================
# 2. GLFW — window, OpenGL context, and input
#    https://github.com/glfw/glfw
# ===========================================================================
if(WIN32)
    set(_glfw_platform -DGLFW_BUILD_WIN32:BOOL=ON)
elseif(APPLE)
    set(_glfw_platform -DGLFW_BUILD_COCOA:BOOL=ON)
else()
    # Linux: prefer X11; disable Wayland to avoid extra deps
    set(_glfw_platform
        -DGLFW_BUILD_X11:BOOL=ON
        -DGLFW_BUILD_WAYLAND:BOOL=OFF
    )
endif()

mcut_add_cmake_dep(
    NAME     GLFW
    URL      https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    CMAKE_ARGS
        -DGLFW_BUILD_DOCS:BOOL=OFF
        -DGLFW_BUILD_TESTS:BOOL=OFF
        -DGLFW_BUILD_EXAMPLES:BOOL=OFF
        -DGLFW_INSTALL:BOOL=ON
        ${_glfw_platform}
)

# ===========================================================================
# 3. GLM — header-only math library
#    https://github.com/g-truc/glm
# ===========================================================================
mcut_add_cmake_dep(
    NAME     GLM
    URL      https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
    CMAKE_ARGS
        -DGLM_BUILD_TESTS:BOOL=OFF
        -DGLM_BUILD_INSTALL:BOOL=ON
)

# ===========================================================================
# 4. GLAD — OpenGL 3.3 Core loader (pre-generated sources, bundled)
#
#    The glad_gl33/ directory (include/ + src/) is committed to the repo
#    under third_party/. We build it with our own deps/GLAD/CMakeLists.txt
#    that turns it into a proper installed static library + CMake package.
# ===========================================================================
set(_glad_src_dir "${CMAKE_SOURCE_DIR}/../third_party")

ExternalProject_Add(dep_GLAD
    # No download needed — sources are already in the repo
    DOWNLOAD_COMMAND  ""
    SOURCE_DIR        "${CMAKE_SOURCE_DIR}/GLAD"
    BINARY_DIR        "${CMAKE_BINARY_DIR}/dep_GLAD-build"
    INSTALL_DIR       "${DESTDIR}"
    ${_msvc_gen}
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${DESTDIR}
        -DCMAKE_PREFIX_PATH:PATH=${DESTDIR}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DBUILD_SHARED_LIBS:BOOL=OFF
        ${_cfg_line}
        ${_osx_target}
        # Tell our GLAD CMakeLists.txt where the glad_gl33 sources live
        -DGLAD_GL33_DIR:PATH=${_glad_src_dir}/glad_gl33
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build . --config Release -- ${_build_j}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} --build . --target install --config Release
    LOG_CONFIGURE ON
    LOG_BUILD     ON
    LOG_INSTALL   ON
)

# ===========================================================================
# 5. ImGui — immediate-mode GUI (built as static library)
#    https://github.com/ocornut/imgui
#
#    ImGui has no upstream CMake build system. We supply our own
#    deps/ImGui/CMakeLists.txt that builds it as a static library and
#    installs headers + CMake package config into DESTDIR.
#    ImGui depends on GLFW (for the backend), so we declare that dependency.
# ===========================================================================
ExternalProject_Add(dep_ImGui
    URL          https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz
    URL_HASH     SHA256=c5fbc5dcab1d46064001c3b84d7a88812985cde7e0e9ced03f5677bec1ba502a
    DOWNLOAD_DIR "${DEP_DOWNLOAD_DIR}/ImGui"
    INSTALL_DIR  "${DESTDIR}"
    DEPENDS      dep_GLFW dep_GLAD
    ${_msvc_gen}
    # Inject our CMakeLists.txt into the downloaded source tree
    PATCH_COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ImGui/CMakeLists.txt"
            "<SOURCE_DIR>/CMakeLists.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ImGui/imgui-config.cmake.in"
            "<SOURCE_DIR>/imgui-config.cmake.in"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${DESTDIR}
        -DCMAKE_PREFIX_PATH:PATH=${DESTDIR}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DBUILD_SHARED_LIBS:BOOL=OFF
        ${_cfg_line}
        ${_osx_target}
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build . --config Release -- ${_build_j}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} --build . --target install --config Release
    LOG_DOWNLOAD  ON
    LOG_CONFIGURE ON
    LOG_BUILD     ON
    LOG_INSTALL   ON
)

# ===========================================================================
# Top-level convenience target
# ===========================================================================
add_custom_target(deps ALL
    DEPENDS dep_MCUT dep_GLFW dep_GLM dep_GLAD dep_ImGui
    COMMENT "All mcut-boolean-viewer dependencies built → ${DESTDIR}"
)
