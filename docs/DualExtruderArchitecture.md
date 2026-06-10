# MCUT Boolean Viewer 双喷头切片架构设计文档

**作者：** Manus AI
**日期：** 2026-06-10

---

## 1. 架构概述

MCUT Boolean Viewer 的双喷头（Dual Extruder）支持旨在为 FDM 3D 打印提供多材料、多色彩的切片能力。该架构深受 BambuStudio 的 `ExtruderManager` 和 `PrintConfig` 设计启发，将双喷头逻辑深度集成到现有的 IR（Intermediate Representation）流水线中。

整个架构分为三个解耦的阶段：
1. **路径规划与分配（DualExtruderPlanner）**：在单喷头切片完成后，为每一层的不同几何特征分配挤出头，并注入换料塔（Prime Tower）几何路径。
2. **中间表示生成（GcodeExporter）**：将带有挤出头分配信息的几何路径转换为 G-code IR 指令流，并标记 `Purge` 等特殊特征。
3. **后处理流水线（ToolChangePass）**：在 IR 层拦截特征边界，插入完整的工具切换（Tool Change）序列，处理温度控制、坐标偏移和换料清洗。

---

## 2. 核心数据结构

双喷头架构依赖于一系列专门的数据结构来管理硬件配置和分配策略。这些结构主要定义在 `DualExtruder.h` 中。

### 2.1 ExtruderConfig（挤出头配置）

每个挤出头（T0 和 T1）都拥有独立的物理和工艺参数，允许使用不同特性的材料（如 PLA 作为主体，PVA 作为支撑）。

| 参数分类 | 字段 | 说明 |
| :--- | :--- | :--- |
| **基础属性** | `index`, `material`, `color` | 挤出头索引（0/1）、材料名称、UI 渲染颜色 |
| **喷嘴参数** | `nozzleDiameter`, `extrusionWidth` | 喷嘴直径、基础挤出线宽 |
| **温度控制** | `extruderTemp`, `standbyTemp` | 打印工作温度、非激活状态下的待机温度 |
| **回抽设置** | `retractionLength`, `retractionSpeed` | 回抽长度、回抽速度（不同材料需求差异大） |
| **空间偏移** | `offset` | 相对于主喷头 T0 的 (X, Y) 物理偏移量 |

### 2.2 DualExtruderParams（双喷头全局参数）

顶层配置结构，包含两个 `ExtruderConfig` 实例、换料塔配置以及全局的工具切换策略。

- **分配策略 (`DualAssignStrategy`)**：
  - `ByFeature`：按几何特征分配（如 T0 打印模型，T1 打印支撑）。
  - `ByColor`：按颜色分配（用于多色模型打印）。
  - `ByMaterial`：按材料属性分配（结构件与柔性件）。
- **工具切换行为**：
  - `heatStandbyExtruder`：是否在非激活期间保持待机温度。
  - `applyOffsetInFirmware`：偏移补偿由固件（如 `OFFSET_EXTRUDER`）处理还是通过软件 `G92` 指令处理。

### 2.3 LayerExtruderAssignment（层级分配结果）

`DualExtruderPlanner` 遍历切片结果后生成的逐层分配计划，记录了该层每种特征使用的挤出头，以及该层需要执行的所有工具切换事件（`ToolChange` 结构）。

---

## 3. 算法流程与规划

双喷头的核心挑战在于如何在正确的时间点切换喷头，并确保切换后的挤出质量。

### 3.1 路径分配与换料塔注入

在 `SlicerEngine` 完成基础的几何切片（生成轮廓、填充、支撑等）后，`DualExtruderPlanner::plan()` 会被调用：
1. **策略映射**：根据 `DualExtruderParams` 中的策略（如 `ByFeature`），将外壳、填充、支撑等特征映射到具体的 T0 或 T1。
2. **事件提取**：按照固定的打印顺序（裙边 → 支撑 → 支撑接口 → 内壳 → 外壳 → 实心层 → 桥接 → 填充），检测相邻特征之间是否发生挤出头变更。
3. **换料塔注入**：如果某层存在工具切换，`DualExtruderPlanner::injectPrimeTower()` 会生成同心圆状的换料塔路径（从外向内螺旋），并将其添加到该层的 `primeTowerPaths` 集合中。换料塔的打印任务交替分配给 T0 和 T1。

### 3.2 换料塔（Prime Tower）几何生成

换料塔的作用是清除喷嘴内残留的旧材料，并在新材料开始打印模型前稳定挤出压力。
- **几何形状**：由一系列同心圆（`Loop2`）组成，外半径固定，内半径随清洗体积（Purge Volume）需求动态计算。
- **清洗体积计算**：`purgeVolume = totalLen * extrusionWidth * layerHeight`，确保挤出的体积达到设定阈值。
- **擦拭墙（Wipe Wall）**：在换料塔最外侧额外生成一圈擦拭墙，用于在离开换料塔时刮除喷嘴挂料。

---

## 4. IR Pass 流水线：ToolChangePass

G-code 生成采用了 IR（中间表示）架构，双喷头的底层控制由 `ToolChangePass` 完全接管。这种设计将复杂的温度和回抽逻辑与几何路径生成彻底解耦。

### 4.1 拦截与状态机

`ToolChangePass` 遍历 `GcodeIR::Program` 中的指令流，维护一个状态机（当前层、当前坐标、T0 和 T1 的独立挤出量 `E[2]`）。当遇到 `OpCode::FeatureBegin` 且目标挤出头与当前挤出头不一致时，触发工具切换序列。

### 4.2 标准工具切换序列

完整的工具切换序列（`emitToolChange`）严格遵循工业级切片软件的标准操作流程：

1. **当前喷头回抽**：执行合成的 `Retract` 操作，防止在移动过程中发生拉丝。
2. **移动到换料塔**：以高旅行速度（Travel Speed）移动到换料塔坐标上方。
3. **发送 Tx 指令**：输出 `T0` 或 `T1` 激活新喷头。
4. **偏移补偿**：如果未启用固件补偿，则输出 `G92 X... Y...` 修正坐标系。
5. **重置挤出量**：输出 `G92 E0`，因为每个喷头使用独立的 E 轴坐标系。
6. **温度切换**：
   - 将旧喷头降温至待机温度（`M104`）。
   - 将新喷头加热至工作温度，并阻塞等待（`M109`）。
7. **风扇加速冷却**：短暂开启全速风扇，加速旧材料的冷却。
8. **打印换料塔**：执行 `Purge` 特征的几何路径，挤出新材料。
9. **冷却停顿（Dwell）**：可选的 `G4` 停顿，等待温度和压力稳定。
10. **新喷头取消回抽**：执行 `Unretract`，恢复打印状态。

---

## 5. 用户界面（UI）集成

在 `main.cpp` 的 Slicer 面板中，新增了专属的 **Dual Extruder** 配置区域。

### 5.1 参数配置面板

采用 ImGui 树形控件组织：
- **全局控制**：启用开关、分配策略单选按钮。
- **T0 / T1 独立面板**：分别设置材料名称、工作/待机温度、喷嘴直径、回抽长度和 X/Y 偏移。
- **Prime Tower 面板**：位置坐标、半径、清洗体积和圈数控制。
- **Feature Assignment**：当选择 `ByFeature` 策略时，提供下拉菜单，允许用户为每种几何特征单独指定 T0 或 T1。

### 5.2 状态监控与可视化

- **渲染器支持**：`SlicerRenderer` 新增了 `COL_PRIME_TOWER`（金色）材质，并在 3D 视口中叠加显示换料塔路径。
- **状态面板**：切片完成后，结果区域会显示双喷头的状态摘要，包括 T0 和 T1 的材料/温度设定，以及实际生成换料塔的层数统计。

---

## 6. 总结

MCUT Boolean Viewer 的双喷头架构通过引入独立的 `ExtruderConfig`、智能的 `DualExtruderPlanner` 以及基于 IR 的 `ToolChangePass`，实现了一个高度模块化、易于扩展的多材料切片系统。该系统不仅能正确处理物理偏移和温度控制，还通过换料塔机制保证了双色/双材料打印的挤出质量。
