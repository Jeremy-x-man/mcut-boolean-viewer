# MCUT Boolean Viewer

一个基于 **MCUT** 网格布尔运算库和 **ImGui** 图形界面库构建的交互式 3D 网格布尔运算预览工具。

## 功能特性

| 功能 | 说明 |
|------|------|
| **布尔运算** | Union（并集）、Intersection（交集）、Difference A-B、Difference B-A、All Fragments |
| **实时 3D 预览** | OpenGL 3.3 渲染，支持轨迹球旋转、平移、缩放 |
| **连通分量展示** | 自动按类型着色（Fragment/Patch/Seam/Input），可单独控制显示/隐藏 |
| **OBJ 导入** | 支持加载任意三角/多边形 OBJ 格式网格 |
| **OBJ 导出** | 将布尔运算结果各连通分量导出为 OBJ 文件 |
| **预设网格** | 内置 Cube+Torus、Sphere+Cube、Sphere+Cylinder 等 6 种预设组合 |
| **MCUT Debug 日志** | 实时显示 MCUT 内部调试信息 |

## 技术栈

- **MCUT** — 高精度网格布尔运算库（LGPL）
- **ImGui** — 即时模式 GUI 库
- **OpenGL 3.3 Core** — 渲染后端
- **GLFW** — 窗口和输入管理
- **GLAD** — OpenGL 函数加载器
- **GLM** — 数学库

## 目录结构

```
mcut_viewer/
├── CMakeLists.txt          # 构建脚本
├── src/
│   └── main.cpp            # 主程序（渲染循环 + ImGui UI）
├── include/
│   ├── Camera.h            # 轨迹球相机
│   ├── Shader.h            # GLSL 着色器工具类
│   ├── RenderMesh.h        # GPU 网格容器
│   ├── ObjLoader.h         # OBJ 解析器
│   └── BooleanOp.h         # MCUT 布尔运算管理器
├── assets/
│   └── meshes/             # 内置测试网格
│       ├── cube.obj
│       ├── torus.obj
│       ├── sphere.obj
│       ├── cylinder.obj
│       └── plane.obj
└── third_party/
    ├── imgui/              # ImGui 源码
    └── glad_gl33/          # GLAD OpenGL 加载器
```

## 构建依赖

**Linux (Ubuntu/Debian):**

```bash
sudo apt-get install -y \
    cmake build-essential \
    libglfw3-dev libgl1-mesa-dev \
    libglm-dev
```

## 构建步骤

```bash
# 1. 克隆本项目
git clone <this-repo> mcut_viewer
cd mcut_viewer

# 2. 构建 MCUT 库（需要先克隆 MCUT）
git clone https://github.com/cutdigital/mcut.git
mkdir mcut_build && cd mcut_build
cmake ../mcut -DCMAKE_BUILD_TYPE=Release -DMCUT_BUILD_TUTORIALS=OFF -DMCUT_BUILD_TESTS=OFF
make -j$(nproc)
cd ..

# 3. 构建 mcut_viewer
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DMCUT_INCLUDE_DIR=../mcut/include \
         -DMCUT_LIBRARY=../mcut_build/lib/libmcut.a
make -j$(nproc)

# 4. 运行
./mcut_viewer
```

## 使用说明

### 基本操作

| 操作 | 说明 |
|------|------|
| 左键拖拽 | 旋转视角（轨迹球） |
| 中键拖拽 | 平移视角 |
| 滚轮 | 缩放 |

### 工作流程

1. **加载网格**：在 "A Source Mesh" 和 "B Cut Mesh" 输入框中填写 OBJ 路径，点击 `Load`
2. **选择运算**：在 "Boolean Operation" 面板中选择运算类型
3. **执行运算**：点击 `Execute Boolean` 按钮
4. **查看结果**：在 "Results" 面板中查看各连通分量，可单独控制显示
5. **导出结果**：填写导出目录，点击 `Export OBJ`

### 布尔运算说明

| 运算 | MCUT Filter Flags | 说明 |
|------|-------------------|------|
| Union (A \| B) | `SEALING_OUTSIDE \| LOCATION_ABOVE` | A 和 B 的并集 |
| Intersection (A & B) | `SEALING_INSIDE \| LOCATION_BELOW` | A 和 B 的交集 |
| Difference A - B | `SEALING_INSIDE \| LOCATION_ABOVE` | A 减去 B |
| Difference B - A | `SEALING_OUTSIDE \| LOCATION_BELOW` | B 减去 A |
| All Fragments | `MC_DISPATCH_FILTER_ALL` | 所有连通分量 |

### 连通分量颜色编码

| 类型 | 颜色 | 说明 |
|------|------|------|
| Fragment | 亮色（蓝/绿/橙） | 布尔运算结果片段 |
| Patch | 黄色 | 切割补丁 |
| Seam | 粉色（带线框） | 缝合线 |
| Input | 灰色（半透明） | 输入网格副本 |

## 架构说明

### BooleanOpManager

`BooleanOp.h` 中的核心类，封装了 MCUT 的完整生命周期：

```cpp
BooleanOpManager mgr;
mgr.init();                    // 创建 McContext（需在 OpenGL 初始化后调用）
mgr.setSourceMesh(srcObj);     // 设置源网格
mgr.setCutMesh(cutObj);        // 设置切割网格
mgr.execute(BoolOpType::UNION);// 执行布尔运算
// 结果在 mgr.resultMeshes 中
```

### 渲染管线

采用 OpenGL 3.3 Core Profile，两遍渲染：
1. **实体渲染**：双面 Phong 光照，支持 alpha 混合
2. **线框叠加**：可选的黑色线框覆盖层

## License

本项目代码采用 MIT License。MCUT 库采用 LGPL v3 License。
