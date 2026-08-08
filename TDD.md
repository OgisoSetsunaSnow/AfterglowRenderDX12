# AfterglowRender DX12 — 技术设计文档（Technical Design Document）

> 版本：0.1（2026-08-03）
> 本文档详细描述将 Vulkan 渲染器 **AfterglowRender** 移植到 DirectX 12 的实现设计与功能说明，供阅读者理解项目的架构、模块、数据流与设计决策。

---

## 1. 项目概述

### 1.1 背景

AfterglowRender 是一个基于 **Vulkan** 的实时渲染器，聚焦渲染算法验证与通用图形开发（PBR、Toon、GPU 粒子、地形、后处理等）。本项目将其**移植到 DirectX 12**：

- **复用**：全部 HLSL Shader 算法、场景/资源组织思想、assimp 模型加载。
- **重写**：CPU 侧 D3D12 框架（设备/命令/交换链/资源/描述符/管线/同步）。

### 1.2 当前能力（已完成）

| 能力 | 说明 |
|---|---|
| D3D12 基础框架 | 设备、命令队列、交换链、命令列表、fence 同步、窗口 resize |
| Shader 编译 | DXC（dxcompiler）将 HLSL 编译为 DXIL（vs_6_0 / ps_6_0） |
| 网格渲染 | 程序化几何（立方体/球/平面/三棱柱）+ PSO + 顶点/索引缓冲 + 深度测试 |
| 纹理 | WIC 加载 PNG → GPU 纹理 → SRV + 采样器 |
| 多物体渲染 | 每物体独立 model 矩阵 + 材质颜色 + 纹理索引（per-object CBV） |
| 相机 | 轨道相机（Orbit）+ 自由相机（Free/FPS），F 键切换 |
| 模型加载 | assimp 加载 fbx/obj，多 mesh 拆分，记录材质名 |
| 多材质纹理 | 每个 mesh 按材质名绑定不同贴图（BattleMage base/decal） |
| 方向光 + Toon | 法线数据（程序化几何 + assimp）+ 方向光（b1 FrameCB）+ 三档 Toon 分段着色 |
| 描边（材质级） | 单 pass CullMode FRONT 背面外扩，对齐参考项目 `ArcOutline.mat`；仅 `DrawItem.outline=true` 的物体启用 |

### 1.3 技术栈

- C++20、Visual Studio 2022+（v143）、Windows SDK 10.0.26100
- D3D12 / DXGI / d3dx12.h（辅助头，来自 d3d12book）
- DXC（dxcompiler.dll / dxcapi.h，来自 VulkanSDK）
- GLFW（窗口/输入）、GLM（数学）、Assimp（模型）、WIC（图片）

---

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                          Main.cpp                            │
│  初始化：Window → D3D12Context → ShaderCompiler → Texture →  │
│           Mesh → Model → MeshRenderer → 相机                 │
│  主循环：update → beginFrame → 相机输入 → 矩阵 → 渲染 → endFrame│
└───────────────────────────┬─────────────────────────────────┘
                            │
          ┌─────────────────┼──────────────────┐
          ▼                 ▼                  ▼
     ┌─────────┐     ┌────────────┐     ┌───────────┐
     │ Window  │     │D3D12Context│     │ MeshRenderer│
     │ glfw    │     │设备/队列/交换链│     │ PSO/根签名/绘制│
     └─────────┘     └────────────┘     └───────────┘
          ▲                 ▲                  ▲
          │                 │                  │
     ┌─────────┐     ┌────────────┐     ┌───────────┐
     │FreeCamera│     │  Texture   │     │   Mesh    │
     │OrbitCamera│    │ WIC→GPU纹理 │     │ 几何+VB/IB │
     └─────────┘     └────────────┘     └─────┬─────┘
                                              │
                                         ┌────┴────┐
                                         │  Model  │
                                         │assimp 加载│
                                         └─────────┘
```

### 2.1 目录结构

```
DX12/
├── AfterglowRenderDX12.sln / .vcxproj
├── include/
│   └── d3dx12.h                 # CD3DX12 辅助（新 SDK 已移除，来自 d3d12book）
├── Shaders/
│   └── Cube.hlsl                # 顶点/像素 shader（MVP + 纹理 + baseColor）
├── Textures/
│   └── checker.png              # 测试棋盘格纹理
└── src/
    ├── Main.cpp                 # 入口 + 场景搭建 + 主循环 + 相机输入
    ├── Window.h/.cpp            # glfw 窗口封装 + 输入查询
    ├── D3D12Core.h/.cpp         # D3D12Context：设备/命令/交换链/同步
    ├── ShaderCompiler.h/.cpp    # DXC 编译 HLSL→DXIL
    ├── Texture.h/.cpp           # WIC 加载纹理 + SRV
    ├── Mesh.h/.cpp              # 通用网格 + 几何工厂
    ├── MeshRenderer.h/.cpp      # 图形管线 + 多物体绘制
    ├── Camera.h/.cpp            # 轨道相机
    ├── FreeCamera.h/.cpp        # FPS 自由相机
    └── Model.h/.cpp             # assimp 模型加载
```

---

## 3. 构建与依赖

### 3.1 构建

```bash
MSBuild.exe AfterglowRenderDX12.sln /p:Configuration=Debug /p:Platform=x64 /m
```

- **工具集**：v143（VS2022+）；**SDK**：10.0.26100
- **标准**：C++20；**字符集**：Unicode；`/utf-8` 编译选项
- **运行时库**：`MultiThreadedDebug`（/MTd）—— 必须与 glfw 静态库（`glfw3_mt.lib`，/MT 编译）匹配

### 3.2 依赖库（位于 `../vsbuild/Libraries/`）

| 库 | 用途 | include | lib |
|---|---|---|---|
| GLFW 3.4 | 窗口/输入 | `glfw-3.4.bin.WIN64/include` | `lib-vc2022/glfw3_mt.lib` |
| GLM 1.0.1 | 数学 | `glm-1.0.1` | header-only |
| Assimp 5.4.3 | 模型加载 | `assimp-5.4.3/include` | `assimp-5.4.3/lib/assimp-vc143-mtd.lib` |
| VulkanSDK | DXC 编译 | `VulkanSDK-1.3.290.0/Include`（含 dxc/dxcapi.h） | `Lib/dxcompiler.lib` |
| OpenImageIO | 提供 zlib/minizip 等 | — | `openimageio-3.0.9.1/debug/lib`（zlibd.lib 等） |

链接依赖：`d3d12.lib; dxgi.lib; dxcompiler.lib; d3dcompiler.lib; dxguid.lib; opengl32.lib; gdi32.lib; shell32.lib; user32.lib; advapi32.lib`。

**运行时 DLL**：
- `zlibd1.dll`（assimp 静态库动态依赖，已复制到 `x64/Debug/`）
- `dxcompiler.dll`（DXC 编译，位于 VulkanSDK/Bin）

### 3.3 关键依赖陷阱（已解决）

1. **glfw 静态库运行时匹配**：`glfw3_mt.lib` 用 /MT 编译，项目必须用 /MT(d)，否则 LNK2038 冲突。
2. **d3dx12.h 缺失**：Windows SDK 22621+ 已移除，需从外部引入（本项目用 d3d12book 的旧版）。
3. **dxcapi.h 依赖 COM**：VulkanSDK 的 dxcapi.h 不自动包含 Windows 头，必须先 `#include <windows.h>`。
4. **assimp 运行时依赖 zlib**：链接 `assimp-vc143-mtd.lib` 后，程序启动还需要 `zlibd1.dll` 在 exe 目录。

---

## 4. 模块详细设计

### 4.1 Window（`Window.h/cpp`）

**职责**：封装 GLFW，提供窗口生命周期与输入查询。

**关键接口**：
| 方法 | 说明 |
|---|---|
| `Window(title, width, height)` | 创建窗口（`GLFW_CLIENT_API = GLFW_NO_API`，无渲染上下文） |
| `shouldClose()` / `update()` | 窗口关闭检测 / 轮询事件+交换缓冲 |
| `nativeHandle()` | 返回 HWND（供 DXGI 交换链使用） |
| `getFramebufferSize(w, h)` | 帧缓冲尺寸（DPI 感知） |
| `isKeyPressed(key)` / `isMouseButtonPressed(btn)` | 轮询输入 |
| `getCursorPos(x, y)` | 鼠标位置 |
| `consumeScrollOffset()` | 滚轮累计偏移（`glfwSetScrollCallback` 累加） |

**设计要点**：
- 静态 `std::once_flag` 保证 `glfwInit` 仅执行一次。
- 标题宽字符 → UTF-8（`WideCharToMultiByte`，支持非 ASCII）。
- resize 不主动处理，由渲染侧每帧检测帧缓冲尺寸。

### 4.2 D3D12Context（`D3D12Core.h/cpp`）

**职责**：D3D12 基础设施单例——工厂/设备/命令队列/交换链/命令列表/fence。

**成员**：
```
IDXGIFactory4           _factory
ID3D12Device            _device
ID3D12CommandQueue      _commandQueue   (DIRECT)
IDXGISwapChain3         _swapChain      (3 缓冲, FLIP_DISCARD)
ID3D12DescriptorHeap    _rtvHeap
ID3D12CommandAllocator  _commandAllocator
ID3D12GraphicsCommandList _commandList
ID3D12Fence             _fence
UINT64                  _fenceValue
std::vector<ComPtr<ID3D12Resource>> _backBuffers  (3)
```

**关键流程**：

- **构造**：`createFactoryAndDevice → createCommandQueue → createSwapChain → createFrameResources`。
  - 设备选择：遍历适配器，跳过软件（WARP），失败回退 WARP 软件渲染。
  - Debug 模式启用 `ID3D12Debug` 验证层 + `DXGI_CREATE_FACTORY_DEBUG`。
- **`beginFrame()`**：
  1. 检测窗口尺寸变化 → 重建交换链（`resizeSwapChain`：等 GPU → 释放后缓冲 → `ResizeBuffers` → 重建 RTV）。
  2. **帧同步（简化）**：若上一帧 fence 未完成则 `WaitForSingleObject`（每帧全同步，性能低但正确）。
  3. `Reset` 命令分配器 + 命令列表，取当前后台缓冲索引。
- **`endFrame()`**：`Close` → `ExecuteCommandLists` → `Present(1, 0)` → `Signal(fence)`。
- **`submitAndWait()`**：用于初始化阶段的一次性上传（记录命令 → 执行 → 等待）。
- **`transitionCurrentBackBuffer(before, after)`**：交换链后台缓冲资源状态切换 barrier。

**已知设计简化**：每帧全同步（fence wait）——未实现多帧在途（frame-in-flight）。后续优化方向：N 个命令分配器 + 按帧信号等待。

### 4.3 ShaderCompiler（`ShaderCompiler.h/cpp`）

**职责**：把 HLSL 源码编译为 DXIL 字节码（`std::vector<uint8_t>`）。

**接口**：
```cpp
namespace shader {
  struct CompileOptions {
    std::wstring entryPoint;      // 如 L"VSMain"
    std::wstring targetProfile;   // 如 L"vs_6_0" / L"ps_6_0" / L"cs_6_0"
    bool enableDebugInfo, skipOptimization;
  };
  std::vector<uint8_t> compileFromFile(path, options);
  std::vector<uint8_t> compileFromSource(src, name, options);
}
```

**实现**：
- `DxcCreateInstance(CLSID_DxcUtils / CLSID_DxcCompiler)`，`IDxcCompiler3::Compile`。
- 参数：`-E` 入口、`-T` profile、`-O3`（Release 优化）、debug 时 `-Zi -Qembed_debug`。
- **矩阵布局**：不传 `-Zpr`，HLSL 默认列主序（column-major），与 `glm::mat4` 内存布局一致。
- 编译失败：从 `DXC_OUT_ERRORS` 取错误信息抛 `std::runtime_error`。

### 4.4 Texture（`Texture.h/cpp`）

**职责**：从图片文件（PNG/JPG）加载并创建 GPU 纹理，供 shader 采样。

**关键方法**：
- `load(D3D12Context&, filePath)`：WIC 解码 → 创建默认堆 `Tex2D` → 上传（`UpdateSubresources`）→ barrier 到 `PIXEL_SHADER_RESOURCE`。
- `createSrv(device, cpuHandle)`：在外部描述符堆位置创建 SRV（`D3D12_SRV_DIMENSION_TEXTURE2D`，mip 1）。

**像素格式**：WIC 转 `GUID_WICPixelFormat32bppRGBA` → `DXGI_FORMAT_R8G8B8A8_UNORM`。

**设计要点**：
- COM 初始化用 `std::call_once` 仅一次（`CoInitializeEx(COINIT_MULTITHREADED)`）。
- 文件路径 UTF-8 → UTF-16（`MultiByteToWideChar`），支持非 ASCII 路径。
- 上传通过 `context.commandAllocator()/commandList()/submitAndWait()` 复用帧资源。

### 4.5 Mesh（`Mesh.h/cpp`）

**职责**：通用网格——顶点/索引数据 + GPU 缓冲（VB/IB）+ 几何工厂。

**顶点结构**：
```cpp
struct MeshVertex {
  glm::vec3 pos;    // offset 0
  glm::vec2 uv;     // offset 12
  glm::vec3 normal; // offset 20
  glm::vec4 color;  // offset 32
};
```

**接口**：
- `create(context, vertices, indices)`：创建默认堆 VB/IB（初始 COMMON）→ 上传 → barrier 到 `VERTEX_AND_CONSTANT_BUFFER` / `INDEX_BUFFER`。
- `vbView() / ibView() / indexCount()`：供绘制使用。
- **几何工厂**：
  - `makeCubeVertices/Indices`：24 顶点（6 面×4）+ 36 索引，CCW 从外看。
  - `makeSphereVertices/Indices(stacks, slices)`：经纬球，16×24。
  - `makePlaneVertices(size)/Indices`：水平单面平面。
  - `makePrismVertices/Indices`：三棱柱，**24 顶点**（6 面每面 4 顶点独立 UV，避免纹理压缩）。

**索引类型**：`uint16_t`（单 mesh 顶点数 < 65536 限制；后续大模型需 32 位索引）。

### 4.9 后处理（阶段 10，`OffscreenRT` / `FullScreenPass` / `PostProcess`）

**渲染链**：场景 → `OffscreenRT`(HDR R16G16B16A16_FLOAT sceneColor) → DownSampling(1/2) → HorizontalBlur → VerticalBlur/Combined → PostProcess(全屏) → swapchain。

- **OffscreenRT**：离屏 RT（RTV + SRV）。D3D12 同一时刻只能有一个 CBV_SRV_UAV 堆 → 多个 RT 的 SRV 必须写同一个**共享堆**（`create(context,w,h,sharedSrvHeap,index)`）。
- **FullScreenPass**：通用全屏 pass（1 SRV + sampler），用于 bloom 各 pass。
- **PostProcess**：全屏三角形（`SV_VertexID` 生成，等价参考项目 PostProcess_VS），合成 sceneColor + bloomCombinedTexture + tonemap。
- **Bloom**（参考项目 DownSampling/HorizontalBlur/VerticalBlurCombination）：2x2 box 降采样 → 水平高斯（`gaussianSigma3` 15-tap，乘 bloomIntensity）→ 垂直高斯。
- **ACES tonemap**：`PostProcess.hlsl` 内 Narkowicz Fitted ACES（`mul(InputMat)→RRT+ODT拟合→mul(OutputMat)→saturate`）。

**10c 备注**：参考项目 Chromatics ACES2（LUT compute 烘焙 + ACESTables）为**未完成实现**——`ACESOutParams`/`ACESReachMTable` 等 SSBO 在 HLSL 中只有使用、无声明（全仓库确认），shader 无法编译，Chromatics compute 从未工作。因此无法复现，改用行业标准 Narkowicz ACES 达成 tonemap 效果。

### 4.6 MeshRenderer（`MeshRenderer.h/cpp`）

**职责**：图形管线（根签名 + PSO）+ 多物体/多纹理绘制。

**根签名（4 参数）**：
| 参数 | 类型 | 绑定 | 阶段 |
|---|---|---|---|
| 0 | CBV root descriptor | `b0`（PerObjectCB） | ALL |
| 1 | CBV root descriptor | `b1`（FrameCB） | ALL（光照 PS 用；描边 VS 用相机/投影参数） |
| 2 | Descriptor table (SRV) | `t0`（纹理） | PIXEL |
| 3 | Descriptor table (Sampler) | `s0` | PIXEL |

**每物体常量缓冲**：
```cpp
struct PerObjectCB {         // 256B 对齐
  glm::mat4 viewProj;        // proj * view
  glm::mat4 world;
  glm::mat4 invTransModel;   // world 的逆转置（法线/描边外扩用，对齐参考项目）
  glm::vec4 baseColor;
  float outlineWidth;        // 描边外扩宽度（参考项目 outlineWidth=10）
  glm::vec3 _pad;
};
```

**每帧常量缓冲（阶段 8 新增）**：
```cpp
struct FrameCB {
  glm::vec4 lightDir;              // 指向光源的单位方向
  glm::vec4 lightColor;            // w = 强度（同参考项目 dirLightColor.a）
  glm::vec4 ambientColor;
  glm::vec4 cameraPosition;
  float invProjection00;           // 1/proj[0][0]，描边 FOV 补偿
  float invScreenAspectRatio;
  float outlineDistanceFadeFactor; // 参考项目 ArcOutline=0.001
  float _pad;
};
```
- `MeshRenderer::setLight(dir, color, ambient)` 设置场景方向光。

**DrawItem**：
```cpp
struct DrawItem {
  const Mesh* mesh;
  glm::mat4 model;       // 世界变换
  glm::vec4 color;       // 材质基础色
  UINT srvIndex;         // 纹理在 SRV heap 中的索引
  bool outline;          // 是否描边（材质级，参考项目仅角色模型启用）
  float outlineWidth;    // 描边宽度（参考项目 10.0）
};
```

**描边（阶段 9，对齐参考项目 `ArcOutline.mat`）**：
- 独立 PSO：`CullMode=FRONT` + `FrontCounterClockwise=TRUE`（= `VK_CULL_MODE_FRONT_BIT` + CCW front），输入布局仅 POSITION/NORMAL。
- `Outline.hlsl`：世界空间沿法线外扩 `worldPos += outlineWidth * (distanceFade * fovFade) * worldNormal`，
  距离衰减 `min(dist*factor, 0.001)`、FOV 补偿 `invProjection00 * invScreenAspectRatio`（参考项目 ArcOutline_VS）。
- 仅 `DrawItem.outline=true` 的物体绘制描边（材质级，非全局）；主体 pass 在其后正常覆盖。

**关键实现**：
- PSO：VS/PS bytecode、输入布局（POSITION/TEXCOORD/NORMAL/COLOR）、`D3D12_CULL_MODE_NONE`（双面渲染）、`FrontCounterClockwise=TRUE`、D32 深度、R8G8B8A8 RTV。
- 深度缓冲：默认堆 `Tex2D(D32_FLOAT)` + DSV heap，初始 `DEPTH_WRITE`。
- 纹理管理：构造接收 `vector<Texture*>`，创建 `CBV_SRV_UAV` heap（每纹理一个 SRV），记录 `_srvDescriptorSize`。
- `render(view, proj, items)`：
  1. 视口/裁剪 → 清深度 → 绑定 RTV+DSV。
  2. 绑定描述符堆、根签名、采样器表。
  3. 写 `FrameCB`（光照+相机+描边参数）到帧 CBV（b1）。
  4. 描边 pass（若启用）：逐 `item.outline` 物体写 `PerObjectCB`（viewProj/world/invTransModel/outlineWidth）→ 画描边。
  5. 主体 pass：逐物体写 `PerObjectCB` → 按 `srvIndex` 设 SRV 表 → `DrawIndexedInstanced`。

**设计简化**：每帧 `Map/Unmap` 上传缓冲写所有物体 CBV；后续用 ring buffer 优化。

### 4.7 相机（`Camera.h/cpp` + `FreeCamera.h/cpp`）

**OrbitCamera（轨道相机）**——相机绕目标点公转：
- 状态：`target / distance / yaw / pitch / fov`。
- `rotate(dx, dy)`：拖拽改变 yaw/pitch（pitch 限 ±89°）。
- `zoom(delta)`：按比例缩放距离。
- `pan(dx, dy)`：平移目标点（屏幕空间）。
- `viewMatrix()` = `glm::lookAt(position, target, up)`；`position()` 从球面坐标计算。

**FreeCamera（自由/FPS 相机）**——自身移动+转向：
- 状态：`position / yaw / pitch`（yaw 初值 -90°，forward=(0,0,-1)）。
- `rotate(dx, dy)`：右键拖拽转向。
- `move(forwardAxis, strafeAxis, upAxis, dt)`：水平前向 + 侧移 + 垂直（WASD + E/Q）。

**坐标系统说明**：项目使用 **glm 右手系**（lookAt/perspective 均 RH），free 相机看 -Z。与 DX12 原生左手系不同，但矩阵自洽可正常渲染（深度 z∈[0,1]）。后续可加 `GLM_FORCE_LEFT_HANDED` 统一。

### 4.8 Model（`Model.h/cpp`）

**职责**：assimp 加载模型文件，拆分为多个 `Mesh`。

**接口**：
- `load(context, filePath)`：`aiProcess_Triangulate | FlipUVs | GenNormals | GenUVCoords | OptimizeMeshes | JoinIdenticalVertices`。
- `meshes()`：模型的所有 Mesh。
- `materialNames()`：每个 mesh 关联的 assimp 材质名。

**实现**：
- 展平节点树（BFS/递归）收集所有 `aiMesh*`。
- 每 mesh：顶点（pos/uv/color）+ 索引 → `Mesh::create` 上传。
- 材质名：`scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, name)`（注意 `GetName()` 签名不兼容）。

**已知限制**：未应用节点层次变换（只取 mesh 几何）；模型朝向可能需手动修正（如 BattleMage 绕 X -90°）；动画/骨骼未处理。

---

## 5. 渲染流程（每帧数据流）

```
window.update()                     # 轮询 GLFW 事件
context.beginFrame()                # 检测 resize + 帧同步 + 重置命令列表
  ├─ 相机输入（鼠标位移 → rotate/pan/zoom；按键 → move）
  ├─ F 键切换 Orbit/Free
  ├─ 计算 view / proj 矩阵
  ├─ 物体动画（立方体旋转、球自转）
  └─ transitionBackBuffer(PRESENT → RENDER_TARGET)
      ClearRenderTargetView（背景色）
      meshRenderer.render(view, proj, items)
        ├─ ClearDepthStencilView + OMSetRenderTargets
        ├─ SetDescriptorHeaps / 根签名 / PSO / 采样器
        └─ 逐物体：写 CBV → 绑定 SRV → DrawIndexedInstanced
      transitionBackBuffer(RENDER_TARGET → PRESENT)
context.endFrame()                  # ExecuteCommandLists + Present + Signal
```

### 场景内容（Main.cpp 硬编码演示）

| 物体 | 几何 | 位置 | 动画 | 纹理 | 颜色 |
|---|---|---|---|---|---|
| 立方体 | makeCube | (0,1,0) | 旋转 | 棋盘格 | 红 tint |
| 球 | makeSphere | (-3.5,1,0) | 自转 | 棋盘格 | 绿 tint |
| 三棱柱 | makePrism | (2.5,0.5,0) | 静止 | 棋盘格 | 蓝 tint |
| Box.fbx | assimp | (4.5,1,0) | 静止 | 棋盘格 | 橙 tint |
| 地面 | makePlane(10) | 原点 | 固定 | 棋盘格 | 灰 |
| BattleMage | assimp（4 mesh） | (0,0,5) scale 0.01 | 静止 | base/decal 贴图 | 白 |

---

## 6. 资源与描述符管理

| 资源类型 | 堆 | 状态 |
|---|---|---|
| 顶点/索引缓冲 | 默认堆（DEFAULT） | COPY_DEST → VERTEX_AND_CONSTANT_BUFFER / INDEX_BUFFER |
| 纹理 | 默认堆（DEFAULT） | COPY_DEST → PIXEL_SHADER_RESOURCE |
| 上传缓冲（CBV/数据） | 上传堆（UPLOAD） | GENERIC_READ |
| RTV | 堆（不 shader visible） | RENDER_TARGET |
| DSV | 堆 | DEPTH_WRITE |
| SRV（纹理） | CBV_SRV_UAV 堆（shader visible） | — |
| Sampler | SAMPLER 堆（shader visible） | — |

**描述符句柄运算**：所有描述符通过 `heapStart + index * descriptorSize` 定位，`descriptorSize` 由 `GetDescriptorHandleIncrementSize(type)` 获取。

---

## 7. Shader 与坐标系

### 7.1 Cube.hlsl

```hlsl
cbuffer PerObject : register(b0) {
    float4x4 mvp;       // 列主序（与 glm 一致）
    float4 baseColor;
};
Texture2D baseTexture : register(t0);
SamplerState baseSampler : register(s0);

PSIn VSMain(VSIn input) {
    output.pos = mul(mvp, float4(input.pos, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
float4 PSMain(PSIn input) : SV_TARGET {
    return baseTexture.Sample(baseSampler, input.uv) * input.color * baseColor;
}
```

### 7.2 坐标系

- **矩阵**：glm 右手系 + HLSL 列主序，`mul(mvp, pos)`。
- **深度**：DX12 `SV_POSITION.z ∈ [0,1]`，深度缓冲 D32_FLOAT，`glm::perspective` 输出兼容。
- **面序**：程序化几何 CCW（从外看）为正面，`FrontCounterClockwise=TRUE`；同时 `CullMode=NONE` 双面渲染。

---

## 8. 材质系统（参考项目 .mat / .mati）

### 8.1 格式

- **`.mat`（材质资产）**：json，含 `vertexShaderPath`/`fragmentShaderPath`、`scalars`（metallic/roughness 等）、`vectors`、`textures`（albedoTex/normalTex 等）。
- **`.mati`（材质实例）**：json，`parentMaterialName` 继承 .mat，可覆写 `textures[].value.path`。

### 8.2 当前 DX12 集成

- 仅使用纹理路径（albedo）：加载 `Texture`，通过 `DrawItem.srvIndex` 绑定。
- BattleMage 材质名 → 贴图映射（Main.cpp 中按 `materialNames()` 匹配）：
  - `MI_19A_Body_base` → `19A_Body_base.png`
  - `MI_19A_Body_decal` → `19A_Body_Decal.png`
  - 其余回退 base。
- **未实现**：scalars/vectors 参数化、sss/ilm/normal 多贴图、PBR/Toon shader。

---

## 9. 已知限制与设计简化

| # | 项 | 说明 | 优先级 |
|---|---|---|---|
| 1 | 帧同步 | 每帧 `WaitForSingleObject` 全同步 | 高（性能） |
| 2 | 深度缓冲 | resize 时不重建 | 中 |
| 3 | CBV 上传 | 每帧 Map/Unmap 全部物体 | 中 |
| 4 | 坐标系统 | glm RH 混用 DX12 LH | 低 |
| 5 | 索引精度 | ~~uint16~~ 已升级 **uint32**（R32_UINT），支持 >65535 顶点（Terrain 26 万顶点） | 已解决 |
| 6 | 节点变换 | Model 未应用 assimp 节点层次变换 | 中 |
| 7 | 渲染能力 | 方向光+Toon+模型描边（材质级）+ 阶段10后处理（bloom/ACES/雾/色差/尘埃/暗角/光轴）+ SkySphere + 3 角色（BattleMage/Yvonne/Ankha）；compute pipeline 已调通（CS 写 UAV → SRV 采样验证通过，readback 确认） | 高（对目标效果） |
| 8 | 棋盘格颜色 | 测试纹理黄+深蓝（GDI+ 颜色管理） | 低 |
| 9 | 调试日志 | cout 缓冲不实时，调试打印用 cerr | 低 |

---

## 10. 路线图

```
[✓] 阶段 1-2：基础框架 + 立方体绘制管线
[✓] 阶段 3-4：纹理 + 相机（Orbit/Free）
[✓] 阶段 5：多 Mesh + 材质数据系统
[✓] 阶段 6-7：assimp 模型 + 多材质纹理（BattleMage）
[✓] 阶段 8：法线数据（MeshVertex + normal）→ 方向光 + Toon shading（2026-08-04）
[✓] 阶段 9：描边（Outline pass，单 pass 对齐参考项目 ArcOutline，材质级）（2026-08-04）
[✓] 阶段 10：后处理（bloom / ACES tonemap）（2026-08-04）
  [✓] 10a：离屏 sceneColor(HDR) + 全屏 PostProcess pass
  [✓] 10b：Bloom（DownSampling + HorizontalBlur + VerticalBlur/Combined）
  [✓] 10c：ACES tonemap（Narkowicz Fitted ACES 公式；参考项目 Chromatics ACES2 为未完成实现，见 10c 备注）
  [✓] 10d：depth 采样（可采样 D32 depth + 距离雾）（2026-08-04）
  [✓] 10d-2：色差/尘埃/暗角/光轴 SkylightShaft（2026-08-05/06，PostProcess 调试按键 1/2/3/5）
  [✓] SkySphere 天空盒：fbx 半径50000 scale0.01 + 独立 Unlit（方向向量 equirect UV）+ DepthFunc=LESS（2026-08-05）
  [✓] 角色导入：Yvonne(12mesh)/Ankha(11mesh) + 材质级描边系统（outlineColor per-material）（2026-08-05）
  [✓] 索引 uint32 升级（R32_UINT）（2026-08-06）
[ ] 阶段 11：多 pass 框架（Forward / Compute / UI）→ 向参考项目管线靠拢
  [x] compute pipeline 基础设施：ComputePass + OffscreenRT::createUav + CS 编译 + dispatch（**已调通**：readback 验证 CS 执行 + UAV 写入 + PostProcess 采样全链路红色；根因是 PostProcess 根签名描述符 range offset 全 0 导致 t0-t3 全绑 index 0，已修复为单 range NumDescriptors=4）
  [x] **ComputePass sampler 支持**（2026-08-08）：构造加 `numSamplerRanges`，root sig 加独立 SAMPLER 描述符表（s0..，offset=0），dispatch 加 `samplerHeap`+`samplers` 参数（SetDescriptorHeaps 同时绑 CBV_SRV_UAV 堆 + SAMPLER 堆）。
  [x] **TerrainData_CS/Meteorograph 平滑采样**（2026-08-08）：`SampleMeteorograph` 改参考模板版（支持 float4/float2 双线性 SampleLevel）；TerrainData_CS 用双线性替换 Load 点采样（MeteorographSampler s0 / PluviometerSampler s1）。
  [x] **Meteorograph 格式对齐参考**（2026-08-08）：Meteorograph R16G16B16A16F→**RGBA32F**（textureMode 9）、Pluviometer R16G16B16A16F→**RG16F**（textureMode 4）；Meteorograph_CS/PluviometerInit 对应 RWTexture2D<float2>。
  [x] **水面 WaterMesh**（2026-08-08）：Water_VS（采样 TerrainHeight.y 水位 + globalOffset 跟随相机）+ Water_FS（折射 waterSceneCopy + 地形法线/沉积物/速度 + FractalSimplexNoise 泡沫 + 深度淡出 + DefaultShading）；WaterRenderer 独立 heap t0-t7（sceneCopy/depth/height/normal/sediment/velocity/fractal/ambient）+ s0-s1；FractalSimplexNoise 256² R32F compute 一次性生成；水 pass depthWrite=false + cullMode=NONE，渲染到离屏 RT（先拷贝场景色做折射源）。
  [ ] 程序化地形 WaterMesh 细节（水下相机/深度处理）
  [ ] InstancingGrass / AnkhaFur / Boids（需 MeshRenderer instancing）
[ ] 阶段 12：OpenImageIO / EXR HDR 支持
```

---

## 附录 A：验证方式

1. **构建**：MSBuild Debug|x64 应 `EXIT=0`。
2. **运行**：工作目录为 `DX12/`，启动后窗口应显示 6 个物体 + BattleMage。
3. **日志**：stdout 打印各资源加载信息（见 TDD 验证清单）。
4. **目视**：每物体颜色/纹理正确；相机可自由漫游；BattleMage 显示真实贴图。

## 附录 B：参考

- 参考项目源码：`C:\Users\32991\Desktop\AfterglowRender-main`（Vulkan 版）
- 参考项目运行版：`C:\Users\32991\Desktop\AfterglowRender_v0.1_x64_debug`
- 官方文档：https://Celumina.github.io/AfterglowRender/
