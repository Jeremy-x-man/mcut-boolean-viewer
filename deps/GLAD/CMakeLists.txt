# CMake build system for pre-generated GLAD OpenGL 3.3 Core loader.
# Called by deps/CMakeLists.txt via ExternalProject_Add.
# Builds glad_gl33 sources as a static library and installs into prefix.

cmake_minimum_required(VERSION 3.16)
project(glad VERSION 2.0 LANGUAGES C)

find_package(OpenGL REQUIRED)

# GLAD_GL33_DIR is passed from the parent ExternalProject
# Default: look for glad_gl33 next to this file
if(NOT DEFINED GLAD_GL33_DIR)
    set(GLAD_GL33_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/glad_gl33")
endif()

if(NOT EXISTS "${GLAD_GL33_DIR}/src/gl.c")
    message(FATAL_ERROR
        "GLAD sources not found at ${GLAD_GL33_DIR}/src/gl.c\n"
        "Set -DGLAD_GL33_DIR=<path-to-glad_gl33> when configuring.")
endif()

message(STATUS "GLAD sources: ${GLAD_GL33_DIR}")

add_library(glad STATIC "${GLAD_GL33_DIR}/src/gl.c")

target_include_directories(glad PUBLIC
    $<BUILD_INTERFACE:${GLAD_GL33_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(glad PUBLIC OpenGL::GL)

set_target_properties(glad PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- Install ---------------------------------------------------------------
install(TARGETS glad
    EXPORT  glad-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY "${GLAD_GL33_DIR}/include/"
    DESTINATION include
)

install(EXPORT glad-targets
    FILE        glad-targets.cmake
    NAMESPACE   glad::
    DESTINATION lib/cmake/glad
)

include(CMakePackageConfigHelpers)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/glad-config.cmake.in"
[=[
@PACKAGE_INIT@
include(CMakeFindDependencyMacro)
find_dependency(OpenGL)
include("${CMAKE_CURRENT_LIST_DIR}/glad-targets.cmake")
check_required_components(glad)
]=]
)

configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/glad-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/glad-config.cmake"
    INSTALL_DESTINATION lib/cmake/glad
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/glad-config.cmake"
    DESTINATION lib/cmake/glad
)
