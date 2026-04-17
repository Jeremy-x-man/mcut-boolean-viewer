# Minimal CMake build system for Dear ImGui (static library)
# Used by the mcut-boolean-viewer deps build system.
# Installs: headers to <prefix>/include/imgui/
#           libimgui.a / imgui.lib to <prefix>/lib/
#           imgui-config.cmake to <prefix>/lib/cmake/imgui/

cmake_minimum_required(VERSION 3.16)
project(imgui VERSION 1.91.6 LANGUAGES CXX)

# ImGui source directory is the parent of this file (set by ExternalProject)
set(IMGUI_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Find GLFW (already installed into DESTDIR before ImGui)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

set(IMGUI_SOURCES
    "${IMGUI_SRC_DIR}/imgui.cpp"
    "${IMGUI_SRC_DIR}/imgui_draw.cpp"
    "${IMGUI_SRC_DIR}/imgui_tables.cpp"
    "${IMGUI_SRC_DIR}/imgui_widgets.cpp"
    "${IMGUI_SRC_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_SRC_DIR}/backends/imgui_impl_opengl3.cpp"
)

add_library(imgui STATIC ${IMGUI_SOURCES})

target_include_directories(imgui PUBLIC
    $<BUILD_INTERFACE:${IMGUI_SRC_DIR}>
    $<BUILD_INTERFACE:${IMGUI_SRC_DIR}/backends>
    $<INSTALL_INTERFACE:include/imgui>
    $<INSTALL_INTERFACE:include/imgui/backends>
)

target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

set_target_properties(imgui PROPERTIES
    CXX_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# Install library
install(TARGETS imgui
    EXPORT  imgui-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# Install headers
install(FILES
    "${IMGUI_SRC_DIR}/imgui.h"
    "${IMGUI_SRC_DIR}/imgui_internal.h"
    "${IMGUI_SRC_DIR}/imconfig.h"
    "${IMGUI_SRC_DIR}/imstb_rectpack.h"
    "${IMGUI_SRC_DIR}/imstb_textedit.h"
    "${IMGUI_SRC_DIR}/imstb_truetype.h"
    DESTINATION include/imgui
)

install(FILES
    "${IMGUI_SRC_DIR}/backends/imgui_impl_glfw.h"
    "${IMGUI_SRC_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_SRC_DIR}/backends/imgui_impl_opengl3.h"
    "${IMGUI_SRC_DIR}/backends/imgui_impl_opengl3.cpp"
    "${IMGUI_SRC_DIR}/backends/imgui_impl_opengl3_loader.h"
    DESTINATION include/imgui/backends
)

# Install CMake package config
install(EXPORT imgui-targets
    FILE        imgui-targets.cmake
    NAMESPACE   imgui::
    DESTINATION lib/cmake/imgui
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/imgui-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/imgui-config.cmake"
    INSTALL_DESTINATION lib/cmake/imgui
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/imgui-config.cmake"
    DESTINATION lib/cmake/imgui
)
