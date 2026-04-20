# MCUT Boolean Viewer

**MCUT Boolean Viewer** 是一个基于 [MCUT](https://github.com/cutdigital/mcut) 库的交互式网格布尔运算预览工具。它允许用户加载 3D 网格模型，实时预览并执行多种布尔运算（并集、交集、差集等），并直观地查看和导出结果的各个连通分量（Connected Components）。

该项目采用 **ImGui** 构建现代化的用户界面，并使用 **OpenGL 3.3 Core** 实现高性能的 3D 实时渲染。

---

## 🌟 核心特性

- **多种布尔运算支持**
  支持 Union（并集）、Intersection（交集）、Difference A-B（差集 A-B）、Difference B-A（差集 B-A）以及 All Fragments（提取所有碎片）。
- **交互式网格拖动**
  按住 `Alt` + 左键即可在 3D 视口中自由拖动选中的网格，松开鼠标后自动重新执行布尔运算并实时刷新结果。
- **独立连通分量预览面板**
  布尔运算生成的每个连通分量（CC）都会在右侧面板中拥有独立的缩略图预览（基于 FBO 离屏渲染），支持自动旋转、独立颜色编辑、可见性控制和单独导出 OBJ。
- **状态快照与撤销/重做**
  支持基于 Snapshot 的 Undo (`Ctrl+Z`) 和 Redo (`Ctrl+Y`)，自动记录每次网格移动和布尔运算的状态。
- **自包含的一键构建系统**
  采用类似 OrcaSlicer 的 `ExternalProject` 依赖管理方案，**无需手动安装任何第三方库，也无需 vcpkg**。所有依赖（MCUT, GLFW, GLM, ImGui, GLAD）会在配置阶段自动下载、编译并安装到本地目录。

---

## 🛠️ 编译与构建（自包含依赖方案）

本项目自带完整的依赖构建脚本，支持 Windows、Linux 和 macOS 平台。

### 前置环境要求

- **CMake** (≥ 3.16)
- **Git** (用于自动下载依赖源码)
- **C++17 兼容的编译器** (GCC, Clang, 或 MSVC 2019/2022)

**Linux 额外依赖（窗口系统头文件）：**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libgl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

**macOS 额外依赖：**
```bash
xcode-select --install
brew install cmake
```

### 🚀 一键构建命令

#### Linux / macOS
在项目根目录下，直接运行提供的 Shell 脚本：
```bash
# 赋予执行权限
chmod +x build_deps.sh

# 一键编译依赖和主程序（首次运行需下载依赖，约 5 分钟；后续为秒级）
./build_deps.sh

# 运行程序
cd build
./mcut_viewer
```

#### Windows (MSVC)
打开 **x64 Native Tools Command Prompt for VS 2022**（或 2019），在项目根目录下运行：
```cmd
:: 一键编译依赖和主程序（首次运行需下载依赖，约 10 分钟；后续为秒级）
build_deps.bat

:: 运行程序
cd build\Release
mcut_viewer.exe
```

### 🔧 高级构建选项

构建脚本支持以下参数，用于增量编译或指定构建类型：

| 命令参数 | 说明 |
|----------|------|
| `Release` / `Debug` | 指定构建类型（默认为 Release） |
| `--app-only` | **常用：** 仅重新编译主程序（当你修改了 `src/main.cpp` 等源码时使用，跳过依赖检查，速度极快） |
| `--deps-only` | 仅编译依赖库（不编译主程序） |
| `clean` | 删除所有构建目录（`build/` 和 `deps_build/`），用于彻底重建 |

**示例（修改代码后快速增量编译）：**
```bash
# Linux/macOS
./build_deps.sh --app-only

# Windows
build_deps.bat --app-only
```

---

## 🎮 使用指南

### 界面布局
程序启动后分为三个主要区域：
1. **左侧控制面板 (Controls)**：负责网格加载、预设选择、布尔运算类型切换以及执行按钮。
2. **中央 3D 视口 (Viewport)**：显示源网格（Source Mesh, A）和切割网格（Cut Mesh, B），支持相机交互。
3. **右侧预览面板 (Result Preview)**：布尔运算执行后，此处将分层列出所有生成的连通分量，并提供独立的 3D 缩略图和导出按钮。

### 鼠标与键盘交互

**相机控制（中央视口）：**
- **左键拖动**：轨道旋转 (Orbit)
- **中键拖动**：平移视口 (Pan)
- **滚轮滚动**：缩放 (Zoom)

**网格拖动（实时布尔预览）：**
- **Alt + 左键拖动**：在平行于屏幕的平面上移动当前选中的网格（在左侧 Drag Control 中切换目标 A/B）。
- **Alt + 滚轮滚动**：沿相机深度轴前后移动网格。
- *提示：拖动结束后松开鼠标，程序会自动重新执行布尔运算并刷新结果。*

**快捷键：**
- **Ctrl + Z**：撤销 (Undo) 上一步操作（网格移动或布尔运算更改）。
- **Ctrl + Y**：重做 (Redo)。

---

## 📂 目录结构说明

```text
mcut-boolean-viewer/
├── build_deps.sh          # Linux/macOS 一键构建脚本
├── build_deps.bat         # Windows 一键构建脚本
├── CMakeLists.txt         # 主程序 CMake 配置
├── deps/                  # 依赖库构建系统 (ExternalProject_Add)
│   ├── CMakeLists.txt     # 自动下载/编译 MCUT, GLFW, GLM, ImGui
│   └── ...
├── src/
│   └── main.cpp           # 主程序入口、渲染循环与 UI 逻辑
├── include/
│   ├── BooleanOp.h        # 核心：MCUT 布尔运算封装与执行
│   ├── Camera.h           # 轨迹球相机实现
│   ├── ObjLoader.h        # 简易 OBJ 文件解析器
│   ├── RenderMesh.h       # OpenGL 网格渲染封装
│   └── Shader.h           # GLSL 着色器编译与管理
├── shaders/               # 顶点与片段着色器
├── assets/meshes/         # 内置的测试网格 (Cube, Sphere, Torus 等)
└── third_party/
    └── glad_gl33/         # 预生成的 GLAD (OpenGL 3.3 Core) 源码
```

---

## 📜 开源协议

本项目自身代码采用 MIT 协议开源。
请注意，本项目依赖的 [MCUT](https://github.com/cutdigital/mcut) 库是双重授权软件（GNU LGPL v3+ 或商业授权）。如果您在商业产品中使用本工具或 MCUT，请务必遵守 MCUT 的开源协议或获取其商业授权。
