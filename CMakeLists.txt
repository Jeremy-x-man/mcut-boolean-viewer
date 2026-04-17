cmake_minimum_required(VERSION 3.16)
project(mcut_viewer VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Platform detection
# ============================================================
if(WIN32)
    message(STATUS "Platform: Windows")
    # Suppress MSVC CRT security warnings (sscanf, fopen, etc.)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Avoid Windows.h macro pollution (min/max)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    message(STATUS "Platform: macOS")
else()
    message(STATUS "Platform: Linux")
endif()

# ============================================================
# MCUT library paths (user-configurable)
# ============================================================
set(MCUT_DIR "" CACHE PATH "Root directory of the MCUT source tree (contains include/mcut/mcut.h)")
set(MCUT_LIBRARY "" CACHE FILEPATH "Path to the compiled MCUT static/import library (.lib on Windows, .a on Linux/macOS)")

# Auto-detect sibling layout: ../mcut and ../mcut_build
if(MCUT_DIR STREQUAL "")
    set(_AUTO_MCUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcut")
    if(EXISTS "${_AUTO_MCUT_DIR}/include/mcut/mcut.h")
        set(MCUT_DIR "${_AUTO_MCUT_DIR}")
        message(STATUS "Auto-detected MCUT_DIR: ${MCUT_DIR}")
    else()
        message(FATAL_ERROR
            "Cannot find MCUT headers. Please set -DMCUT_DIR=<path/to/mcut>")
    endif()
endif()

if(MCUT_LIBRARY STREQUAL "")
    set(_AUTO_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcut_build")
    if(WIN32)
        # MSVC multi-config layout
        foreach(_cfg Release RelWithDebInfo Debug)
            if(EXISTS "${_AUTO_BUILD_DIR}/${_cfg}/mcut.lib")
                set(MCUT_LIBRARY "${_AUTO_BUILD_DIR}/${_cfg}/mcut.lib")
                break()
            endif()
        endforeach()
        # Single-config / MinGW layout
        if(MCUT_LIBRARY STREQUAL "")
            foreach(_name mcut.lib libmcut.a libmcut.lib)
                if(EXISTS "${_AUTO_BUILD_DIR}/lib/${_name}")
                    set(MCUT_LIBRARY "${_AUTO_BUILD_DIR}/lib/${_name}")
                    break()
                endif()
                if(EXISTS "${_AUTO_BUILD_DIR}/${_name}")
                    set(MCUT_LIBRARY "${_AUTO_BUILD_DIR}/${_name}")
                    break()
                endif()
            endforeach()
        endif()
    else()
        if(EXISTS "${_AUTO_BUILD_DIR}/lib/libmcut.a")
            set(MCUT_LIBRARY "${_AUTO_BUILD_DIR}/lib/libmcut.a")
        endif()
    endif()

    if(MCUT_LIBRARY STREQUAL "")
        message(FATAL_ERROR
            "Cannot find MCUT library. Please set -DMCUT_LIBRARY=<path/to/mcut.lib or libmcut.a>")
    endif()
    message(STATUS "Auto-detected MCUT_LIBRARY: ${MCUT_LIBRARY}")
endif()

# ============================================================
# Find system packages
# ============================================================
find_package(OpenGL REQUIRED)

# GLFW: prefer find_package (works with vcpkg toolchain or system install)
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    # Fallback: look for GLFW in common manual install locations
    find_library(GLFW_LIB NAMES glfw3 glfw HINTS
        "$ENV{GLFW_DIR}/lib"
        "$ENV{GLFW_DIR}/lib-vc2022"
        "$ENV{GLFW_DIR}/lib-vc2019"
        "$ENV{GLFW_DIR}/lib-mingw-w64"
        "C:/glfw/lib"
        "C:/glfw/lib-vc2022"
    )
    find_path(GLFW_INCLUDE NAMES GLFW/glfw3.h HINTS
        "$ENV{GLFW_DIR}/include"
        "C:/glfw/include"
    )
    if(GLFW_LIB AND GLFW_INCLUDE)
        add_library(glfw STATIC IMPORTED)
        set_target_properties(glfw PROPERTIES
            IMPORTED_LOCATION "${GLFW_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${GLFW_INCLUDE}"
        )
        message(STATUS "Found GLFW manually: ${GLFW_LIB}")
    else()
        message(FATAL_ERROR
            "GLFW not found. Install via vcpkg (vcpkg install glfw3:x64-windows) "
            "or set GLFW_DIR environment variable to the GLFW root directory.")
    endif()
endif()

# GLM: header-only, find_package or manual
find_package(glm QUIET)
if(NOT glm_FOUND)
    find_path(GLM_INCLUDE NAMES glm/glm.hpp HINTS
        "$ENV{GLM_DIR}/include"
        "$ENV{GLM_DIR}"
        "C:/glm"
        "C:/glm/include"
    )
    if(GLM_INCLUDE)
        add_library(glm::glm INTERFACE IMPORTED)
        set_target_properties(glm::glm PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${GLM_INCLUDE}")
        message(STATUS "Found GLM manually: ${GLM_INCLUDE}")
    else()
        message(FATAL_ERROR
            "GLM not found. Install via vcpkg (vcpkg install glm:x64-windows) "
            "or set GLM_DIR environment variable.")
    endif()
endif()

# ============================================================
# MCUT imported target
# ============================================================
add_library(mcut_static STATIC IMPORTED)
set_target_properties(mcut_static PROPERTIES
    IMPORTED_LOCATION "${MCUT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MCUT_DIR}/include"
)

# On Linux/macOS MCUT needs pthreads; on Windows it uses native threads
if(NOT WIN32)
    find_package(Threads REQUIRED)
    set_property(TARGET mcut_static APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads)
endif()

# ============================================================
# Build GLAD (OpenGL 3.3 Core loader)
# ============================================================
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad_gl33")

add_library(glad STATIC "${GLAD_DIR}/src/gl.c")
target_include_directories(glad PUBLIC "${GLAD_DIR}/include")

# On Windows, OpenGL32.lib is needed
if(WIN32)
    target_link_libraries(glad PUBLIC opengl32)
endif()

# ============================================================
# Build ImGui
# ============================================================
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ============================================================
# Main executable
# ============================================================
add_executable(mcut_viewer src/main.cpp)

target_include_directories(mcut_viewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MCUT_DIR}/include"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${GLAD_DIR}/include"
)

target_link_libraries(mcut_viewer PRIVATE
    mcut_static
    imgui
    glad
    glfw
    OpenGL::GL
)

# Platform-specific extra libraries
if(WIN32)
    # gdi32 and user32 are needed by GLFW on Windows
    target_link_libraries(mcut_viewer PRIVATE gdi32 user32 shell32)
elseif(UNIX AND NOT APPLE)
    # dl is needed by GLAD on Linux
    target_link_libraries(mcut_viewer PRIVATE ${CMAKE_DL_LIBS})
endif()

# MSVC: set the executable subsystem to Windows (no console) for Release,
# but keep console for Debug so log output is visible.
if(MSVC)
    set_target_properties(mcut_viewer PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:mcut_viewer>"
    )
    # Use /MT (static CRT) to reduce runtime dependency issues
    # Comment out if you prefer /MD (dynamic CRT)
    # set_property(TARGET mcut_viewer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ============================================================
# Post-build: copy assets and shaders to output directory
# ============================================================
add_custom_command(TARGET mcut_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:mcut_viewer>/assets"
    COMMENT "Copying assets..."
)

add_custom_command(TARGET mcut_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:mcut_viewer>/shaders"
    COMMENT "Copying shaders..."
)

# ============================================================
# Summary
# ============================================================
message(STATUS "--------------------------------------------")
message(STATUS "MCUT Viewer build configuration:")
message(STATUS "  Platform     : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  MCUT dir     : ${MCUT_DIR}")
message(STATUS "  MCUT library : ${MCUT_LIBRARY}")
message(STATUS "  ImGui dir    : ${IMGUI_DIR}")
message(STATUS "  GLAD dir     : ${GLAD_DIR}")
message(STATUS "--------------------------------------------")
