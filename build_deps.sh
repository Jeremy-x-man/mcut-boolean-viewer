#!/usr/bin/env bash
# =============================================================================
# build_deps.sh — mcut-boolean-viewer one-shot build script (Linux / macOS)
#
# Inspired by OrcaSlicer's deps build workflow:
#   1. Builds all dependencies via deps/CMakeLists.txt (ExternalProject_Add)
#      into deps_build/install/ — NO system packages, NO vcpkg needed.
#   2. Configures and builds the main mcut_viewer executable, pointing
#      CMAKE_PREFIX_PATH at the deps install tree.
#
# Usage:
#   ./build_deps.sh              # build deps + app (Release)
#   ./build_deps.sh Debug        # build deps + app (Debug)
#   ./build_deps.sh --deps-only  # build deps only
#   ./build_deps.sh --app-only   # build app only (deps must already exist)
#   ./build_deps.sh clean        # clean everything and rebuild from scratch
#
# Dependencies built automatically (downloaded at configure time):
#   - MCUT  1.3.0   — mesh boolean operations
#   - GLFW  3.4     — window + OpenGL context
#   - GLM   1.0.1   — math library (header-only)
#   - GLAD  (bundled) — OpenGL 3.3 core function loader (pre-generated)
#   - ImGui 1.91.1  — immediate-mode GUI
#
# System requirements (Linux):
#   sudo apt-get install -y cmake build-essential libgl1-mesa-dev \
#       libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
#
# System requirements (macOS):
#   xcode-select --install && brew install cmake
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_BUILD_DIR="${SCRIPT_DIR}/deps_build"
DEPS_INSTALL_DIR="${DEPS_BUILD_DIR}/install"
APP_BUILD_DIR="${SCRIPT_DIR}/build"

# ---- Parse arguments -------------------------------------------------------
BUILD_TYPE="Release"
BUILD_DEPS=true
BUILD_APP=true

for arg in "$@"; do
    case "$arg" in
        Debug|Release|RelWithDebInfo|MinSizeRel)
            BUILD_TYPE="$arg" ;;
        --deps-only) BUILD_APP=false  ;;
        --app-only)  BUILD_DEPS=false ;;
        clean)
            echo "[clean] Removing ${DEPS_BUILD_DIR} and ${APP_BUILD_DIR} ..."
            rm -rf "${DEPS_BUILD_DIR}" "${APP_BUILD_DIR}"
            echo "[clean] Done."
            exit 0
            ;;
        -h|--help)
            head -30 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg  (use --help for usage)"
            exit 1
            ;;
    esac
done

# ---- Detect CPU count ------------------------------------------------------
if command -v nproc &>/dev/null; then
    NPROC=$(nproc)
elif command -v sysctl &>/dev/null; then
    NPROC=$(sysctl -n hw.logicalcpu)
else
    NPROC=4
fi

echo ""
echo "============================================================"
echo " mcut-boolean-viewer build"
echo "  Build type  : ${BUILD_TYPE}"
echo "  Parallel    : ${NPROC} jobs"
echo "  Deps prefix : ${DEPS_INSTALL_DIR}"
echo "============================================================"
echo ""

# ===========================================================================
# Step 1: Build dependencies
# ===========================================================================
if [ "$BUILD_DEPS" = true ]; then
    echo "[1/2] Configuring dependencies ..."
    mkdir -p "${DEPS_BUILD_DIR}"
    cmake -S "${SCRIPT_DIR}/deps" \
          -B "${DEPS_BUILD_DIR}" \
          -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
          -DDESTDIR="${DEPS_INSTALL_DIR}"

    echo ""
    echo "[1/2] Building dependencies (first run downloads & compiles ~5 min) ..."
    echo "      Subsequent runs are instant (all cached)."
    echo ""
    # Use -j1 at top level so ExternalProject log output stays readable;
    # each sub-project still uses parallel build internally.
    cmake --build "${DEPS_BUILD_DIR}" --config "${BUILD_TYPE}" -j1

    echo ""
    echo "[1/2] Dependencies installed to: ${DEPS_INSTALL_DIR}"
    echo "      Libraries:"
    find "${DEPS_INSTALL_DIR}/lib" -maxdepth 1 -name "*.a" 2>/dev/null | sort | \
        while read -r f; do
            printf "        %-30s %s\n" "$(basename "$f")" "$(du -sh "$f" 2>/dev/null | cut -f1)"
        done
    echo ""
fi

# ===========================================================================
# Step 2: Build main application
# ===========================================================================
if [ "$BUILD_APP" = true ]; then
    if [ ! -d "${DEPS_INSTALL_DIR}" ]; then
        echo "[ERROR] Deps install directory not found: ${DEPS_INSTALL_DIR}"
        echo "        Run without --app-only first to build dependencies."
        exit 1
    fi

    echo "[2/2] Configuring mcut_viewer ..."
    mkdir -p "${APP_BUILD_DIR}"
    cmake -S "${SCRIPT_DIR}" \
          -B "${APP_BUILD_DIR}" \
          -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
          -DCMAKE_PREFIX_PATH="${DEPS_INSTALL_DIR}"

    echo ""
    echo "[2/2] Building mcut_viewer ..."
    cmake --build "${APP_BUILD_DIR}" --config "${BUILD_TYPE}" -- -j"${NPROC}"

    BINARY="${APP_BUILD_DIR}/mcut_viewer"
    echo ""
    echo "============================================================"
    if [ -f "${BINARY}" ]; then
        echo " Build successful!"
        echo "  Executable : ${BINARY}"
        echo "  Size       : $(du -sh "${BINARY}" | cut -f1)"
        echo ""
        echo " Run with:"
        echo "   cd ${APP_BUILD_DIR} && ./mcut_viewer"
    else
        echo " [ERROR] Build failed — executable not found."
        exit 1
    fi
    echo "============================================================"
    echo ""
fi
