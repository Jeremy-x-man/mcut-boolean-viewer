cmake_minimum_required(VERSION 3.16)
project(mcut_viewer VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Platform detection
# ============================================================
if(WIN32)
    message(STATUS "Platform: Windows")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    message(STATUS "Platform: macOS")
else()
    message(STATUS "Platform: Linux")
endif()

# ============================================================
# Dependency discovery strategy
#
# Two modes (auto-detected, no user action required):
#
#   Mode A — deps build (recommended):
#     cmake ../deps && cmake --build .   →  produces deps_build/install/
#     cmake .. -DCMAKE_PREFIX_PATH=<deps_build/install>
#     All deps are found via find_package() from that prefix.
#
#   Mode B — legacy sibling layout (backward-compatible):
#     Expects ../mcut (source) and ../mcut_build (compiled library)
#     alongside the repo, plus system/vcpkg GLFW and GLM.
#
# Mode A takes priority when CMAKE_PREFIX_PATH contains a valid MCUT install.
# ============================================================

# ---- Try Mode A: find MCUT via find_package (deps-built) ------------------
# MCUT's cmake config is installed to lib/cmake/mcut/ by our post_install.cmake
find_package(mcut QUIET HINTS "${CMAKE_PREFIX_PATH}/lib/cmake/mcut")

if(mcut_FOUND)
    set(_MCUT_MODE "deps-build (find_package)")
    # mcut exports mcut::mcut target
    set(_MCUT_TARGET mcut::mcut)
else()
    # ---- Mode B: legacy sibling layout ------------------------------------
    set(_MCUT_MODE "legacy sibling layout")

    set(MCUT_DIR "" CACHE PATH
        "MCUT source tree root (contains include/mcut/mcut.h)")
    set(MCUT_LIBRARY "" CACHE FILEPATH
        "Compiled MCUT static library (.lib / .a)")

    # Auto-detect ../mcut source
    if(MCUT_DIR STREQUAL "")
        set(_auto "${CMAKE_CURRENT_SOURCE_DIR}/../mcut")
        if(EXISTS "${_auto}/include/mcut/mcut.h")
            set(MCUT_DIR "${_auto}")
        else()
            message(FATAL_ERROR
                "MCUT not found.\n"
                "Option 1 (recommended): build deps first:\n"
                "  cd <repo>; mkdir deps_build; cd deps_build\n"
                "  cmake ../deps && cmake --build .\n"
                "  cd ..; mkdir build; cd build\n"
                "  cmake .. -DCMAKE_PREFIX_PATH=../deps_build/install\n"
                "Option 2 (legacy): set -DMCUT_DIR=<path/to/mcut-source>")
        endif()
    endif()

    # Auto-detect ../mcut_build compiled library
    if(MCUT_LIBRARY STREQUAL "")
        set(_bd "${CMAKE_CURRENT_SOURCE_DIR}/../mcut_build")
        if(WIN32)
            foreach(_cfg Release RelWithDebInfo Debug)
                if(EXISTS "${_bd}/${_cfg}/mcut.lib")
                    set(MCUT_LIBRARY "${_bd}/${_cfg}/mcut.lib")
                    break()
                endif()
            endforeach()
            foreach(_n mcut.lib libmcut.a)
                if(MCUT_LIBRARY STREQUAL "" AND EXISTS "${_bd}/lib/${_n}")
                    set(MCUT_LIBRARY "${_bd}/lib/${_n}")
                endif()
            endforeach()
        else()
            if(EXISTS "${_bd}/lib/libmcut.a")
                set(MCUT_LIBRARY "${_bd}/lib/libmcut.a")
            endif()
        endif()
        if(MCUT_LIBRARY STREQUAL "")
            message(FATAL_ERROR
                "MCUT library not found. Set -DMCUT_LIBRARY=<path/to/libmcut.a>")
        endif()
    endif()

    add_library(mcut_imported STATIC IMPORTED)
    set_target_properties(mcut_imported PROPERTIES
        IMPORTED_LOCATION             "${MCUT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MCUT_DIR}/include"
    )
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        set_property(TARGET mcut_imported APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES Threads::Threads)
    endif()
    set(_MCUT_TARGET mcut_imported)
    message(STATUS "MCUT dir     : ${MCUT_DIR}")
    message(STATUS "MCUT library : ${MCUT_LIBRARY}")
endif()

# ---- OpenGL ----------------------------------------------------------------
find_package(OpenGL REQUIRED)

# ---- GLFW ------------------------------------------------------------------
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    # Fallback for Mode B with manual GLFW install
    find_library(GLFW_LIB NAMES glfw3 glfw HINTS
        "$ENV{GLFW_DIR}/lib"
        "$ENV{GLFW_DIR}/lib-vc2022"
        "$ENV{GLFW_DIR}/lib-vc2019"
        "$ENV{GLFW_DIR}/lib-mingw-w64"
    )
    find_path(GLFW_INCLUDE NAMES GLFW/glfw3.h HINTS
        "$ENV{GLFW_DIR}/include"
    )
    if(GLFW_LIB AND GLFW_INCLUDE)
        add_library(glfw STATIC IMPORTED)
        set_target_properties(glfw PROPERTIES
            IMPORTED_LOCATION             "${GLFW_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${GLFW_INCLUDE}"
        )
    else()
        message(FATAL_ERROR
            "GLFW not found. Build deps first (see README) or set GLFW_DIR.")
    endif()
endif()

# ---- GLM -------------------------------------------------------------------
find_package(glm QUIET)
if(NOT glm_FOUND)
    find_path(GLM_INCLUDE NAMES glm/glm.hpp HINTS
        "$ENV{GLM_DIR}/include" "$ENV{GLM_DIR}"
    )
    if(GLM_INCLUDE)
        add_library(glm::glm INTERFACE IMPORTED)
        set_target_properties(glm::glm PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${GLM_INCLUDE}")
    else()
        message(FATAL_ERROR
            "GLM not found. Build deps first (see README) or set GLM_DIR.")
    endif()
endif()

# ---- GLAD ------------------------------------------------------------------
# Mode A: installed by deps build → find via find_package
# Mode B: use bundled third_party/glad_gl33 sources
find_package(glad QUIET)
if(glad_FOUND)
    set(_GLAD_TARGET glad::glad)
else()
    set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad_gl33")
    if(NOT EXISTS "${GLAD_DIR}/src/gl.c")
        message(FATAL_ERROR "GLAD sources not found at ${GLAD_DIR}")
    endif()
    add_library(glad_local STATIC "${GLAD_DIR}/src/gl.c")
    target_include_directories(glad_local PUBLIC "${GLAD_DIR}/include")
    if(WIN32)
        target_link_libraries(glad_local PUBLIC opengl32)
    endif()
    set(_GLAD_TARGET glad_local)
endif()

# ---- ImGui -----------------------------------------------------------------
# Mode A: installed by deps build → find via find_package
# Mode B: build from bundled third_party/imgui sources
find_package(imgui QUIET)
if(imgui_FOUND)
    set(_IMGUI_TARGET imgui::imgui)
    set(_IMGUI_INCLUDE_DIRS "")   # provided by imported target
else()
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
    if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
        message(FATAL_ERROR "ImGui sources not found at ${IMGUI_DIR}")
    endif()
    add_library(imgui_local STATIC
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(imgui_local PUBLIC
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends"
    )
    target_link_libraries(imgui_local PUBLIC glfw OpenGL::GL)
    set(_IMGUI_TARGET imgui_local)
    set(_IMGUI_INCLUDE_DIRS "${IMGUI_DIR};${IMGUI_DIR}/backends")
endif()

# ============================================================
# Main executable
# ============================================================
add_executable(mcut_viewer src/main.cpp)

target_include_directories(mcut_viewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${_IMGUI_INCLUDE_DIRS}
)

target_link_libraries(mcut_viewer PRIVATE
    ${_MCUT_TARGET}
    ${_IMGUI_TARGET}
    ${_GLAD_TARGET}
    glfw
    OpenGL::GL
    glm::glm
)

# Platform-specific extra libraries
if(WIN32)
    target_link_libraries(mcut_viewer PRIVATE gdi32 user32 shell32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(mcut_viewer PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    set_target_properties(mcut_viewer PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:mcut_viewer>"
    )
endif()

# ============================================================
# Post-build: copy assets and shaders
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
message(STATUS "  MCUT mode    : ${_MCUT_MODE}")
message(STATUS "--------------------------------------------")
