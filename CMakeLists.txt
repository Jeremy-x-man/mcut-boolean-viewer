cmake_minimum_required(VERSION 3.16)
project(mcut_viewer VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Options
# ============================================================
option(MCUT_BUILD_AS_SHARED_LIB "Build MCUT as shared library" OFF)
option(MCUT_BUILD_TESTS "Build MCUT tests" OFF)
option(MCUT_BUILD_TUTORIALS "Build MCUT tutorials" OFF)

# ============================================================
# Find system packages
# ============================================================
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

# ============================================================
# MCUT library (pre-built)
# ============================================================
set(MCUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcut")
set(MCUT_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcut_build")

add_library(mcut_static STATIC IMPORTED)
set_target_properties(mcut_static PROPERTIES
    IMPORTED_LOCATION "${MCUT_BUILD_DIR}/lib/libmcut.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MCUT_DIR}/include"
)
# MCUT needs pthreads
find_package(Threads REQUIRED)
set_property(TARGET mcut_static APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES Threads::Threads)

# ============================================================
# Build GLAD (OpenGL 3.3 loader)
# ============================================================
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad_gl33")

add_library(glad STATIC "${GLAD_DIR}/src/gl.c")
target_include_directories(glad PUBLIC "${GLAD_DIR}/include")

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
add_executable(mcut_viewer
    src/main.cpp
)

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
    ${CMAKE_DL_LIBS}
)

# Copy assets to build directory
add_custom_command(TARGET mcut_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:mcut_viewer>/assets"
    COMMENT "Copying assets..."
)

# Copy shaders to build directory
add_custom_command(TARGET mcut_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:mcut_viewer>/shaders"
    COMMENT "Copying shaders..."
)

message(STATUS "MCUT Viewer configured.")
message(STATUS "  MCUT dir  : ${MCUT_DIR}")
message(STATUS "  ImGui dir : ${IMGUI_DIR}")
message(STATUS "  GLAD dir  : ${GLAD_DIR}")
