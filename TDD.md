# AfterglowRender DX12 — 技术设计文档（TDD）

> 版本：1.1（2026-08-09）
> 面向读者：**新手小白**。本文从零讲解这个 D3D12 渲染器是怎么搭起来的、每个模块做什么、关键代码长什么样、怎么运行、出问题怎么查。
> 配套文档：`ERRORS.md`（踩坑记录）、`WORKLOG.md`（开发工作记录）、`AUDIT_DIFF.md`（与参考项目的差异审计）。

---

## 目录

1. [项目简介](#1-项目简介)
2. [新手快速入门](#2-新手快速入门)
3. [技术栈与依赖](#3-技术栈与依赖)
4. [目录结构](#4-目录结构)
5. [DirectX 12 基础概念（新手必读）](#5-directx-12-基础概念新手必读)
6. [架构总览与数据流](#6-架构总览与数据流)
7. [核心模块详解](#7-核心模块详解)
7.20-7.24. [HLSL Shader 解读](#720-hlsl-shader-解读)（公共库 / 角色 / 地形与计算 / 特效 / 后处理）
8. [关键数据布局](#8-关键数据布局)
9. [坐标系约定](#9-坐标系约定)
10. [渲染流程（逐帧）](#10-渲染流程逐帧)
11. [场景内容](#11-场景内容)
12. [材质系统](#12-材质系统)
13. [调试指南](#13-调试指南)
14. [常见问题 FAQ](#14-常见问题-faq)
15. [项目路线图](#15-项目路线图)
16. [附录](#16-附录)

---

## 1. 项目简介

### 1.1 这是什么

本项目是把一个 **Vulkan** 实时渲染器（参考项目 *AfterglowRender*）**移植到 DirectX 12** 的完整实现。参考项目用 Vulkan API，本项目用 D3D12 API，但**渲染算法、HLSL Shader、场景设计全部照搬参考项目**——除 API 差异外不改方案。

复现的内容包括：

- **三个角色**：BattleMage（红发法师，Toon 卡通材质）、Yvonne（粉发双马尾少女，12 个部位的精细材质）、Ankha（橙猫娘，毛茸茸 ShellFur + mask）。
- **程序化地形**：4096×4096 高度图 + 水文模拟（降雨/侵蚀/水流）+ 草地质感。
- **草 / 鸟群 / 毛 / 粒子**：GPU 实例化草地、鸟群群集算法、ShellFur 壳层毛、4096 粒子。
- **水面**：折射 + 泡沫 + 深度淡出的水。
- **后处理全套**：Bloom 泛光、ACES 色调映射（Chromatics 3D LUT）、色差、尘埃、暗角、光轴、高度雾、水下效果。
- **ImGui GUI**：浅色圆角控制台面板，可切换后处理、控制相机、播放音乐。
- **音频**：miniaudio 音乐播放器。

### 1.2 参考项目的架构（为什么这样移植）

参考项目是一个完整的 Vulkan 引擎，核心架构：

```text
AfterglowApplication（场景搭建）
  └─ Scene（Entity-Component 系统）
       ├─ AfterglowStaticMeshComponent（网格 + 材质）
       ├─ AfterglowComputeComponent（计算任务：地形/草/粒子）
       ├─ AfterglowCameraComponent / AfterglowDirectionalLightComponent
       └─ AfterglowPostProcessComponent（后处理）
AfterglowRenderer（渲染线程）
  └─ AfterglowPassManager（pass 链）
       Forward → Transparency → BloomPassSet → PostProcess → UserInterface
```

本工程不移植整个引擎框架，而是**只移植"场景 + 渲染算法"**，用更简单的单线程循环 + 硬编码场景组织代替 ECS。核心思路：

- **Shader 全部照搬**（参考 `vsbuild/Shaders/` → 本工程 `Shaders/`，做 D3D12 语法适配）。
- **材质参数照搬**（参考 `.mat/.mati` → 本工程 `materialGroups` 表格 + `materialParams*`）。
- **场景实体照搬**（参考 `AfterglowApplication.cpp` 的 22 个实体 → 本工程 `Main.cpp`）。
- **API 层重写**（Vulkan 的 DescriptorSet/RenderPass → D3D12 的描述符堆/PSO）。

### 1.3 面向新手的阅读建议

如果你完全没接触过图形 API，建议按顺序读：

1. 先读 **第 5 章 D3D12 基础概念**（不依赖代码，讲清 GPU 编程的最小心智模型）。
2. 再读 **第 6 章架构总览** 了解整体拼图。
3. 然后挑感兴趣的模块读 **第 7 章**（每个模块都从"职责 → 结构 → 关键代码 → 坑"展开）。
4. 遇到问题翻 **第 13 章调试指南** 和 `ERRORS.md`。

### 1.4 参考项目位置（勿删）

- `C:\Users\32991\Desktop\AfterglowRender-main` —— Vulkan 源码 + HLSL Shader（`vsbuild/Shaders`）
- `C:\Users\32991\Desktop\AfterglowRender_v0.1_x64_release` —— 真实游戏资产（`Assets\Shared\...`）
- `C:\Users\32991\Desktop\AfterglowRender_v0.1_x64_debug` —— Debug 版运行程序

---

## 2. 新手快速入门

### 2.1 环境要求

- Windows 10/11（x64）
- Visual Studio 2022+（本项目用 **v145 工具集**，即 VS2026 v18；旧版 VS 需把 vcxproj 中 `<PlatformToolset>v145</PlatformToolset>` 改成本机版本）
- Windows SDK **10.0.26100**

### 2.2 构建

在工程根目录 `DX12/` 打开 VS 开发者命令行（或任意终端，用完整 MSBuild 路径）：

```powershell
# 方法一：VS Developer Prompt 里直接
MSBuild AfterglowRenderDX12.vcxproj -p:Configuration=Debug -p:Platform=x64 -m

# 方法二：完整路径（VS2026 v18）
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe' AfterglowRenderDX12.vcxproj -p:Configuration=Debug -p:Platform=x64 -m
```

输出 exe：`x64\Debug\AfterglowRenderDX12.exe`。

> 注意：**工作目录必须是工程根 `DX12/`**，程序用相对路径引用 `Shaders/`、`Textures/`、`vsbuild/Assets/`。

### 2.3 运行

```powershell
cd C:\Users\32991\Desktop\AfterglowRenderForDX12\DX12
.\x64\Debug\AfterglowRenderDX12.exe
```

### 2.4 操作

| 操作 | 功能 |
|---|---|
| 鼠标左键拖拽 | 轨道相机旋转（Orbit 模式） |
| 鼠标中键拖拽 | 平移相机目标 |
| 滚轮 | 缩放 |
| 鼠标右键拖拽 | 自由相机转向（Free 模式） |
| `W/A/S/D`、`E/Q`、`Space/C` | 自由相机移动（Free 模式） |
| `F` | 切换 轨道/自由 相机 |
| `~`（反引号） | 开关 ImGui 控制台 |
| `1/2/3` | 色差 / 尘埃 / 暗角 开关 |
| `5/6/7/8` | 显示 noise / 场景原色 / bloom / LUT 调试视图 |
| `0` / `4` | 后处理全关 / 全开 |

### 2.5 启动日志怎么看

程序启动会在控制台打印大量 `[AfterglowRender DX12] ...` 日志：shader 编译字节数、纹理尺寸、模型 mesh 数、角色世界位置等。**排查加载类问题时重点看这些日志**。fatal 错误写 `fatal.log`。

---

## 3. 技术栈与依赖

### 3.1 技术栈

| 项 | 说明 |
|---|---|
| 语言 | C++20 |
| 图形 API | Direct3D 12（DXGI 交换链） |
| Shader | HLSL（vs_6_0 / ps_6_0 / cs_6_0），运行时 DXC 编译 |
| 数学 | GLM（右手系） |
| 窗口/输入 | GLFW 3.4 |
| 模型 | Assimp 5.4.3 |
| 图片 | WIC（Windows Imaging Component） |
| GUI | Dear ImGui 1.92.3（glfw + dx12 后端） |
| 音频 | miniaudio（单头文件库） |
| 反射 | 自研 Inreflect（照搬参考项目，GUI 反射按钮用） |

### 3.2 依赖库（`vsbuild/Libraries/`）

| 库 | 用途 | 说明 |
|---|---|---|
| `glfw-3.4.bin.WIN64/` | 窗口/输入 | 静态库 `glfw3_mt.lib`（/MT 编译，项目必须用 /MTd Debug） |
| `glm-1.0.1/` | 数学 | header-only |
| `VulkanSDK-1.3.290.0/` | 只取 DXC | `dxcapi.h` / `dxcompiler.lib` / `dxcompiler.dll` |
| `assimp-5.4.3/` | 模型加载 | 静态库 `.lib` 体积大不入 git，见 README 自行下载 |
| `openimageio-3.0.9.1/` | assimp 依赖 | 提供 zlib/minizip/lzma/bz2 链接库与运行 DLL |
| `imgui-1.92.3/` | GUI | 含 `imgui_impl_glfw.cpp` + `imgui_impl_dx12.cpp` |
| `Inreflect/` | 反射库 | `Inreflect.h` + `FunctionTraits.h` + `TemplateString.h` + `TypeConstraints.h` |
| `miniaudio/` | 音频 | 单文件 `miniaudio.h`（MIT） |

### 3.3 关键依赖陷阱（已解决）

1. **glfw 静态库 /MT 与项目 /MTd 匹配**：改运行时库设置会 LNK2038 冲突。
2. **d3dx12.h**：新版 Windows SDK 移除了，本项目从 d3d12book 引入到 `include/`。
3. **dxcapi.h 依赖 windows.h**：必须先 `#include <windows.h>`。
4. **assimp 运行依赖 zlib**：Debug 要 `zlibd1.dll`、Release 要 `zlib1.dll/bz2.dll/liblzma.dll`，vcxproj 的 `CopyRuntimeDLLs` target 已自动复制到输出目录。
5. **NOMINMAX**：`Main.cpp` 顶部定义了 `NOMINMAX`，否则 Windows 的 `min/max` 宏会破坏标准库和 Inreflect 的 `std::max`。

---

## 4. 目录结构

```text
DX12/                              # 仓库根 = 工程根
├── AfterglowRenderDX12.sln/.vcxproj
├── README.md / TDD.md / ERRORS.md / WORKLOG.md / AUDIT_DIFF.md
├── include/d3dx12.h               # CD3DX12 辅助头
├── src/                           # C++ 源码（按功能分子目录）
│   ├── Main.cpp                   # 入口：场景搭建 + 主循环 + 相机输入
│   ├── Core/                      # 基础设施
│   │   ├── D3D12Core.h/cpp        # D3D12Context：设备/队列/交换链/同步
│   │   ├── Window.h/cpp           # GLFW 窗口 + 输入
│   │   ├── Mesh.h/cpp             # 网格 + 几何工厂 + GPU 缓冲
│   │   ├── Model.h/cpp            # assimp 模型加载
│   │   ├── Texture.h/cpp          # 纹理加载 + GPU mip
│   │   └── Camera.h/cpp           # Orbit 轨道相机
│   │   └── FreeCamera.h/cpp       # FPS 自由相机
│   ├── Renderers/                 # 各渲染器（自管 PSO/描述符/CBV）
│   │   ├── MeshRenderer.*         # 通用物体 + 描边 + 天空盒
│   │   ├── CharacterRenderer.*    # 角色材质系统（11+ 材质 PSO）
│   │   ├── TerrainRenderer.*      # 程序化地形
│   │   ├── WaterRenderer.*        # 水面
│   │   ├── GrassRenderer.*        # 实例化草地
│   │   ├── BoidsRenderer.*        # 鸟群
│   │   ├── FurRenderer.*          # AnkhaFur 壳层毛
│   │   └── ParticleRenderer.*     # 粒子
│   ├── Passes/
│   │   ├── OffscreenRT.h/cpp      # 离屏 RT（RTV+SRV/UAV）
│   │   ├── FullScreenPass.h/cpp   # 通用全屏 pass
│   │   ├── ComputePass.h/cpp      # 计算管线
│   │   └── PostProcess.h/cpp      # 后处理
│   ├── Utils/
│   │   ├── ShaderCompiler.h/cpp   # DXC 编译
│   │   ├── ACES.h/cpp             # ACES2 表生成
│   │   ├── FractalNoise.h/cpp     # 噪声纹理生成
│   │   └── GuiInspect.h           # 反射按钮渲染
│   ├── Audio/AudioSystem.h/cpp    # miniaudio 音乐
│   └── GUI/GuiRenderer.h/cpp      # ImGui 集成
├── Shaders/                       # HLSL（角色/地形/草/水面/后处理/Chromatics/Boid/Grass/ShellFur/ParticleExample）
├── Textures/                      # 测试纹理
└── vsbuild/
    ├── Libraries/                 # 第三方依赖
    └── Assets/                    # 运行资源（模型/贴图/字体/音乐）
```

---

## 5. DirectX 12 基础概念（新手必读）

这一章非常重要。不懂这些，后面代码看不懂。用最通俗的方式讲，并配真实代码。

### 5.1 GPU 编程的最小模型

你可以把 GPU 想成一个**非常专一的工人**：

- 它只能按你**提前编译好**的指令（Shader）和**提前准备好**的数据（网格/纹理）干活。
- 你不能"随时改"它正在用的数据——要改得先让它**停下（同步）**，或者**换个状态（资源屏障）**。
- 一次干活叫一个 **Draw 调用**（画物体）或 **Dispatch 调用**（算数据）。

CPU 和 GPU 是**异步**的：CPU 把命令写进"笔记本"（命令列表），合上后整本交给 GPU 按顺序执行。CPU 不会等 GPU，所以需要 fence 才能知道 GPU 干完没。

### 5.2 核心对象

| 对象 | 通俗解释 | 本工程封装 |
|---|---|---|
| `ID3D12Device` | GPU 的"身份证"和工厂，创建所有对象 | `D3D12Context._device` |
| `ID3D12CommandQueue` | 提交命令的"管道"，命令按顺序执行 | `_commandQueue` |
| `ID3D12CommandAllocator` | 命令的内存池 | `_commandAllocator` |
| `ID3D12GraphicsCommandList` | 记录指令的"笔记本" | `_commandList` |
| `ID3D12Resource` | GPU 内存（Buffer/Texture） | `Mesh`/`Texture`/`OffscreenRT` |
| `ID3D12DescriptorHeap` | 描述符（资源的"门牌号"）的集合 | 各渲染器 `_heap` |
| `ID3D12RootSignature` | 告诉 GPU "shader 需要哪几张牌" | 各渲染器 `_rootSignature` |
| `ID3D12PipelineState`（PSO） | 编译好的完整渲染配方 | 各渲染器 `_pso` |
| `ID3D12Fence` | GPU 干完活的"闹钟" | `_fence` |

### 5.3 一次绘制的基本流程

```cpp
// 1. 清空"笔记本"（命令分配器），准备记录
commandAllocator->Reset();
commandList->Reset(commandAllocator, nullptr);

// 2. 资源屏障：告诉 GPU "这块内存用途变了"
//    例如：把后台缓冲从"展示"切成"可绘制"
D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
commandList->ResourceBarrier(1, &barrier);

// 3. 绑定渲染目标 + 深度缓冲（OMSetRenderTargets）
commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

// 4. 清屏（可选）
float clearColor[4] = { 0.1f, 0.11f, 0.13f, 1.0f };
commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

// 5. 绑定管线 + 根签名 + 描述符堆 + 参数
commandList->SetPipelineState(pso);
commandList->SetGraphicsRootSignature(rootSig);
ID3D12DescriptorHeap* heaps[] = { srvHeap, samplerHeap };
commandList->SetDescriptorHeaps(2, heaps);
commandList->SetGraphicsRootDescriptorTable(0, gpuHandle); // t0 表
commandList->SetGraphicsRootConstantBufferView(1, cbvGpuAddress); // b1

// 6. 顶点/索引缓冲 + 画
commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
commandList->IASetVertexBuffers(0, 1, &vbView);
commandList->IASetIndexBuffer(&ibView);
commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);

// 7. 合上笔记本，交给管道执行，设闹钟
commandList->Close();
commandQueue->ExecuteCommandLists(1, &commandList);
commandQueue->Signal(fence, value);
```

### 5.4 资源与堆类型

D3D12 有三种内存"堆"（内存区域）：

| 堆类型 | 谁看得见 | 用途 | 本工程例子 |
|---|---|---|---|
| `DEFAULT`（默认堆） | 只有 GPU 能访问 | 顶点/索引缓冲、纹理 | 几乎全部 GPU 资源 |
| `UPLOAD`（上传堆） | CPU+GPU | CPU 写数据给 GPU 读 | 各类 CBV、上传缓冲 |
| `READBACK`（回读堆） | CPU 读 GPU 写的 | GPU 算完拿回 CPU（调试） | 粒子/草诊断读回 |

创建示例：

```cpp
// 上传堆缓冲（CPU 可写，GPU 可读）——常量缓冲通用做法
const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
const CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(512);
device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbv));

// CPU 写入
void* p; cbv->Map(0, nullptr, &p); memcpy(p, &data, sizeof(data)); cbv->Unmap(0, nullptr);
```

> **坑**：READBACK 堆**只支持 Buffer**，不能创建纹理。要读纹理得用 `CopyTextureRegion` 拷到 Buffer（`ERRORS.md` #7）。

### 5.5 资源状态与资源屏障

这是 D3D12（和 Vulkan）最重要的概念之一。**同一块 GPU 内存，在不同阶段有不同的"用途状态"**：

| 状态 | 用途 |
|---|---|
| `D3D12_RESOURCE_STATE_RENDER_TARGET` | 作为渲染目标写 |
| `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` | 作为纹理采样读（PS） |
| `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE` | 作为纹理采样读（VS/CS） |
| `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` | 作为计算读写（UAV） |
| `D3D12_RESOURCE_STATE_COPY_DEST/SOURCE` | CPU/GPU 拷贝 |
| `D3D12_RESOURCE_STATE_PRESENT` | 交换链展示 |
| `D3D12_RESOURCE_STATE_DEPTH_WRITE/READ` | 深度读写 |
| `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT` | 间接绘制命令缓冲 |

**资源屏障（Resource Barrier）** 告诉 GPU"用途变了，请同步数据"：

```cpp
D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
    tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
commandList->ResourceBarrier(1, &b);
```

> 本工程大量"怪 bug"源于**漏了 barrier**：compute 写完后没转采样态就采样、UAV 与 SRV 状态冲突等（见 `ERRORS.md` #6 #7）。**每次切换用途都必须显式 barrier**。

### 5.6 描述符与描述符堆

- **描述符（Descriptor）** 是资源的"门牌号"。GPU 拿到描述符才知道"去哪个内存地址读纹理"。
- 描述符必须放在 **描述符堆（Descriptor Heap）** 里，堆分两种：
  - **CPU 可见**（`D3D12_DESCRIPTOR_HEAP_FLAG_NONE`）：CPU 端记录用，如 RTV/DSV 堆。
  - **Shader 可见**（`D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE`）：GPU 能引用，SRV/UAV/CBV 堆。

```cpp
// 创建 shader 可见的 SRV 堆
D3D12_DESCRIPTOR_HEAP_DESC desc{};
desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
desc.NumDescriptors = 25;
desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));

// 定位第 i 个描述符的句柄（重要公式！）
UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
cpu.ptr += i * inc;
D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
gpu.ptr += i * inc;
```

**关键规则**：
- 同一时刻只能绑定**一个** `CBV_SRV_UAV` 堆 + 一个 `SAMPLER` 堆（`SetDescriptorHeaps` 一起设置）。
- shader 里 `t0、t1...` 对应描述符表里**连续**的描述符。

### 5.7 根签名（Root Signature）与 PSO

- **根签名** = shader 的"参数清单"：声明这个 shader 用哪些常量缓冲（`b0 b1...`）、纹理（`t0 t1...`）、采样器（`s0 s1...`）。C++ 端和 HLSL 端的绑定必须完全一致。

```cpp
// 根签名：b0 CBV + b1 CBV + t0-t5 SRV 表 + s0-s5 采样器表
D3D12_ROOT_PARAMETER params[4]{};
params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
params[0].Descriptor.ShaderRegister = 0; // b0
params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
params[1].Descriptor.ShaderRegister = 1; // b1
D3D12_DESCRIPTOR_RANGE srvRange{};
srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
srvRange.NumDescriptors = 6;             // t0-t5 连续
params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
params[2].DescriptorTable.NumDescriptorRanges = 1;
params[2].DescriptorTable.pDescriptorRanges = &srvRange;
// ... s0-s5 采样器表同理 ...
D3D12_ROOT_SIGNATURE_DESC rsDesc{};
rsDesc.NumParameters = 4;
rsDesc.pParameters = params;
rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
// 序列化 + 创建
```

- **PSO（管线状态对象）** = 渲染配方：shader + 光栅化（剔除/线框）+ 深度模板 + 混合 + 输入布局 + 输出格式。**D3D12 不允许运行时改 PSO 内任何东西**，每个不同配置都要新建一个 PSO。

```cpp
D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
desc.pRootSignature = rootSig.Get();
desc.VS = { vsBytes.data(), vsBytes.size() };
desc.PS = { psBytes.data(), psBytes.size() };
desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
desc.RasterizerState.FrontCounterClockwise = TRUE; // CCW 为正面
desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;            // 必须显式，匹配实际 DSV
desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // 必须显式，匹配实际 RTV
desc.NumRenderTargets = 1;
desc.InputLayout = { il.data(), (UINT)il.size() };
device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
```

> **坑**：`D3D12_GRAPHICS_PIPELINE_STATE_DESC{}` 零初始化后 `DepthFunc=0=NEVER`、`DSVFormat=UNKNOWN` → 绘制失败（`ERRORS.md` #1）。所有字段显式设置。

### 5.8 同步（Fence）

GPU 和 CPU 异步。要等 GPU 干完用 Fence：

```cpp
commandQueue->Signal(fence, ++value);        // GPU 干完后把 fence 设成 value
if (fence->GetCompletedValue() < value) {    // CPU 检查
    fence->SetEventOnCompletion(value, event);
    WaitForSingleObject(event, INFINITE);    // 阻塞等
}
```

本工程目前**每帧等待 GPU**（`beginFrame` 里等上一帧 fence）——简单正确但慢。后续可做多帧在途。

### 5.9 交换链（Swapchain）

双/三缓冲：GPU 画到后台缓冲，Present 翻到前面，同时拿另一块继续画。本工程 3 个后台缓冲（`FLIP_DISCARD`）。窗口 resize 时：等 GPU → 释放旧缓冲 → `ResizeBuffers` → 重建 RTV。

## 6. 架构总览与数据流

### 6.1 分层图

```text
┌────────────────────────────────────────────────────────────────┐
│                        Main.cpp（场景 + 主循环）                  │
│  初始化：Window → D3D12Context → ShaderCompiler → Texture →     │
│          Mesh → Model → 各 Renderer → GuiRenderer → AudioSystem  │
│  主循环：update → beginFrame → 相机 → compute 模拟 → 渲染 → GUI → │
│          endFrame                                                │
└───────────┬──────────────────────┬──────────────────────────────┘
            │                      │
      ┌─────▼─────┐         ┌──────▼──────┐
      │ Core/     │         │  Passes/    │
      │ Window,   │         │ OffscreenRT │
      │ D3D12Core │         │ FullScreen  │
      │ Mesh,     │         │ ComputePass │
      │ Model,    │         │ PostProcess │
      │ Texture   │         └─────────────┘
      └─────┬─────┘
            │
      ┌─────▼──────────────────────────────┐
      │ Renderers/（每个渲染器自管 PSO/描述符）│
      │ MeshRenderer → CharacterRenderer →  │
      │ Terrain → Water → Grass → Boids →  │
      │ Fur → Particle                      │
      └────────────────────────────────────┘
```

### 6.2 每帧流程（一句话版）

```text
事件 → 相机输入 → 计算 view/proj
→ 计算管线模拟（噪声/气象/群集/草/粒子）
→ 场景渲染到离屏 RT（天空→地形→物体→角色→鸟群→草→毛→粒子→水）
→ Bloom（6 级金字塔）
→ 后处理（ACES LUT + 雾 + 色差 + 尘埃 + 暗角 + 光轴）输出到交换链
→ ImGui 控制台
→ Present
```

### 6.3 渲染目标链（RT 链）

```text
swapchain 后台缓冲 (R8G8B8A8_UNORM)
   ↑ 后处理 PostProcess（全屏）
   ↑ Bloom combined[0]（1/2 分辨率，6 级金字塔最终结果）
   ↑ 6 级 Bloom（down → horz → combined）
   ↑ 离屏 OffscreenRT (R16G16B16A16_FLOAT sceneColor)
   ↑ 场景渲染（地形/物体/角色/草/水/粒子等）
```

### 6.4 渲染器的通用模式（读懂一个就懂全部）

每个渲染器（Renderer）基本都按这个套路写，新手先抓住这个模板：

```cpp
class XxxRenderer {
public:
    // 构造：创建根签名 → PSO → 描述符堆 → CBV → 采样器
    XxxRenderer(context, shaderBytes..., 资源指针...);
    // 一次性初始化（compute 数据生成）
    void initialize();
    // 每帧渲染（写 CBV → 绑定 → Draw/Dispatch）
    void render(view, proj, ...);
private:
    void createRootSignature();
    void createPipelineState();
    void createHeaps();    // 描述符堆
    void createCbvs();     // 常量缓冲
    void createSamplers();
    D3D12Context& _context;
    ComPtr<ID3D12RootSignature> _rootSig;
    ComPtr<ID3D12PipelineState> _pso;
    ComPtr<ID3D12DescriptorHeap> _heap;
    ComPtr<ID3D12Resource> _cbv;
};
```

每个渲染器独立管理自己的根签名/PSO/描述符堆，**唯一例外是"共享堆"**（多个 RT 的 SRV 必须写同一个 shader 可见堆，见 8.1）。

---

## 7. 核心模块详解

### 7.1 Window（`src/Core/Window.h/cpp`）

**职责**：封装 GLFW，提供窗口生命周期与输入查询。

类结构：

```cpp
class Window {
public:
    Window(const std::wstring& title, uint32_t width, uint32_t height);
    ~Window();
    bool shouldClose() const;                 // 窗口关闭检测
    void update();                            // glfwPollEvents + glfwSwapBuffers
    GLFWwindow* handle() const;               // 给 imgui 用
    uint32_t width() const; uint32_t height() const;
    void getFramebufferSize(uint32_t& w, uint32_t& h) const; // DPI 感知
    void* nativeHandle() const;               // HWND，给 DXGI swapchain
    // ---- 输入（轮询式）----
    bool isKeyPressed(int key) const;
    bool isMouseButtonPressed(int button) const;
    void getCursorPos(double& x, double& y) const;
    double consumeScrollOffset();             // 滚轮累计偏移
private:
    static void resizeCallback(GLFWwindow*, int, int);
    static void scrollCallback(GLFWwindow*, double, double);
    GLFWwindow* _window;
    uint32_t _width, _height;
    double _scrollOffset;
};
```

设计要点：
- 静态 `std::once_flag` 保证 `glfwInit` 只执行一次。
- 创建窗口时 `GLFW_CLIENT_API = GLFW_NO_API`（不创建 GL 上下文，纯窗口）。
- 标题宽字符转 UTF-8（`WideCharToMultiByte`）支持中文标题。
- 滚轮用回调累加 `_scrollOffset`（`consumeScrollOffset` 读取并清零）。
- 窗口 resize 不主动处理，由 `D3D12Context::beginFrame` 每帧比对尺寸重建交换链。

> 注：imgui 接入后，GLFW 的 scroll/key/mouse/char 回调被 `GuiRenderer` 接管（GLFW 每事件类型只有一个回调），滚轮 zoom 改为从 `GuiRenderer::consumeScrollOffset()` 读（`ERRORS.md` 5.4）。

### 7.2 D3D12Context（`src/Core/D3D12Core.h/cpp`）

**职责**：D3D12 基础设施——工厂/设备/命令队列/交换链/命令列表/fence。整个程序就一个实例，所有渲染器都从它拿设备、命令列表。

成员：

```cpp
class D3D12Context {
public:
    explicit D3D12Context(Window& window);
    ~D3D12Context();

    bool beginFrame();    // resize 检测 + 帧同步 + 重置命令列表
    void endFrame();      // Close + Execute + Present + Signal
    void submitAndWait(); // 初始化一次性上传用
    void waitForGpu();

    ID3D12Device* device() const;
    ID3D12CommandQueue* commandQueue() const;
    ID3D12GraphicsCommandList* commandList() const;
    ID3D12DescriptorHeap* rtvHeap() const;
    ID3D12Resource* currentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE currentRtv() const; // 当前后台缓冲 RTV
    uint32_t width() const; uint32_t height() const;
    void transitionCurrentBackBuffer(before, after); // 后台缓冲 barrier
private:
    void createFactoryAndDevice();  // 枚举适配器，跳过 WARP，Debug 开验证层
    void createCommandQueue();
    void createSwapChain();         // 3 缓冲 FLIP_DISCARD
    void createFrameResources();
    void resizeSwapChain();         // 等 GPU → ResizeBuffers → 重建 RTV
    static constexpr UINT BackBufferCount = 3;
};
```

`beginFrame` 详细流程：

```cpp
bool D3D12Context::beginFrame() {
    // 1. 窗口尺寸变化 → 重建交换链
    uint32_t w, h; _window->getFramebufferSize(w, h);
    if ((w != _width || h != _height) && w > 0 && h > 0) {
        _width = w; _height = h; _needsResize = true;
    }
    if (_needsResize) resizeSwapChain();

    // 2. 帧同步：等上一帧 GPU 完成（简化方案，每帧全同步）
    if (_fence->GetCompletedValue() < _fenceValue) {
        _fence->SetEventOnCompletion(_fenceValue, _fenceEvent);
        WaitForSingleObject(_fenceEvent, INFINITE);
    }

    // 3. 重置命令分配器 + 命令列表，取当前后台缓冲索引
    _commandAllocator->Reset();
    _commandList->Reset(_commandAllocator.Get(), nullptr);
    _currentBackBufferIndex = _swapChain->GetCurrentBackBufferIndex();
    return true;
}
```

`endFrame`：

```cpp
void D3D12Context::endFrame() {
    _commandList->Close();
    ID3D12CommandList* lists[] = { _commandList.Get() };
    _commandQueue->ExecuteCommandLists(1, lists);
    _swapChain->Present(1, 0);
    _commandQueue->Signal(_fence.Get(), ++_fenceValue); // 供下一帧等待
}
```

> 设计简化：每帧全同步（fence wait），未做多帧在途（frame-in-flight）。后续优化方向：N 个命令分配器 + 按帧信号。

### 7.3 ShaderCompiler（`src/Utils/ShaderCompiler.h/cpp`）

**职责**：把 HLSL 编译成 DXIL 字节码。**运行时编译**，改 shader 不用重链 exe（程序启动时编译一次）。

```cpp
namespace shader {
  struct CompileOptions {
    std::wstring entryPoint;    // L"VSMain" / L"PSMain" / L"main"(CS)
    std::wstring targetProfile; // L"vs_6_0" / L"ps_6_0" / L"cs_6_0"
    bool enableDebugInfo = false, skipOptimization = false;
  };
  std::vector<uint8_t> compileFromFile(const std::string& path, const CompileOptions& o);
  std::vector<uint8_t> compileFromSource(const std::string& src, const std::string& name, const CompileOptions& o);
}
```

实现核心：

```cpp
// DxcCreateInstance 拿到 utils 和 compiler
Microsoft::WRL::ComPtr<IDxcUtils> utils;
Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));

// 构造编译参数：-E 入口 -T profile [-Zi -Qembed_debug]
std::vector<LPCWSTR> args = { L"-E", entry, L"-T", target, L"-O3" /* 或 -Od */ };
// IDxcCompiler3::Compile(sourceBlob, args, includeHandler, &result)
// 从 result->GetOutput(DXC_OUT_ERRORS) 拿错误信息（编译失败抛异常）
// 从 result->GetOutput(DXC_OUT_OBJECT) 拿 DXIL 字节
```

要点：
- **不传 `-Zpr`**：保持 HLSL 默认列主序，与 `glm::mat4` 内存布局一致。
- 入口名约定：VS/PS 用 `VSMain/PSMain`，CS 用 `main`。

> 开发时预编译验证：`dxc.exe -I Shaders -E PSMain -T ps_6_0 <file> -Fo NUL`。角色 shader 改动后必须预编译，否则启动才报错。

### 7.4 Texture（`src/Core/Texture.h/cpp`）

**职责**：加载图片（PNG/JPG）→ GPU 纹理 → SRV；支持 GPU mip 链生成。

加载流程：

```cpp
void Texture::load(D3D12Context& context, const std::string& filePath,
                   DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    // 1. WIC 解码（COM 初始化 call_once）：IWICBitmapDecoder → Frame → 像素
    //    WIC 转 GUID_WICPixelFormat32bppRGBA
    // 2. 创建默认堆 Tex2D（ALLOW_UNORDERED_ACCESS 以便 GPU mip）
    // 3. 上传 mip0（UpdateSubresources，注意 FirstSubresource=0, Num=1）
    // 4. GPU mip 生成（若 mipCount>1）：GenerateMip_CS 逐级 blit
    // 5. barrier 到采样态
}
```

**GPU mip 生成**（等价参考项目 `vkCmdBlitImage`）：

- sRGB 格式不支持 UAV → 内部用 UNORM + WIC 像素 sRGB→linear，mip 在**线性空间**生成，SRV 用 UNORM。
- `Shaders/GenerateMip_CS.hlsl`：双线性 blit，`srcTex.SampleLevel(uv, 0)`（SRV `MostDetailedMip` 传 srcMip），每级 UAV→SRV barrier。
- **早期创建会触发驱动错误 0x887A0005** → 用 `Texture::warmUpMipGenerator(context)` 在 main 早期预创建。

`createSrv(device, cpuHandle)`：在外部描述符堆的指定位置创建 SRV（`MipLevels=-1` 允许完整 mip 链）。**所有 SRV desc 必须设 `Shader4ComponentMapping`**（`ERRORS.md` #4）。

文件路径 UTF-8 → UTF-16（`MultiByteToWideChar`）支持中文路径。

### 7.5 Mesh 与 Model（`src/Core/`）

**Mesh**：通用网格 + GPU 缓冲。顶点结构（**60 字节**）：

```cpp
struct MeshVertex {
    glm::vec3 pos;     // offset 0
    glm::vec2 uv;      // offset 12
    glm::vec3 normal;  // offset 20
    glm::vec4 color;   // offset 32
    glm::vec3 tangent; // offset 48
};
```

输入布局（与顶点结构对应）：

```cpp
std::array<D3D12_INPUT_ELEMENT_DESC, 5> il = { {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
} };
```

索引为 **32 位 `R32_UINT`**（支持 >65535 顶点的大模型，地形网格 26 万顶点）。

创建 GPU 缓冲：默认堆 VB/IB（初始 `COMMON`）→ 上传 → barrier 到 `VERTEX_AND_CONSTANT_BUFFER` / `INDEX_BUFFER`。提供 `vbView()/ibView()/indexCount()` 供绘制。

几何工厂：`makeCubeVertices/Indices`（24 顶点 36 索引）、`makeSphereVertices(16,24)`、`makePlaneVertices(size)`、`makePrismVertices`（24 顶点 6 面独立 UV）、`makeTerrainVertices(256, 4.0)` / `makeTerrainIndices(256)`（照搬参考 GridVertex/GridIndexPack）。

**Model**：assimp 加载 fbx/obj，拆成多个 Mesh。

```cpp
bool Model::load(D3D12Context& context, const std::string& filePath);
const std::vector<Mesh>& meshes() const;
const std::vector<std::string>& materialNames() const; // 每 mesh 材质名
```

导入标志：`Triangulate | FlipUVs | GenNormals | GenUVCoords | CalcTangentSpace | OptimizeMeshes | JoinIdenticalVertices`。展平节点树收集所有 aiMesh；材质名 `scene->mMaterials[mMaterialIndex]->Get(AI_MATKEY_NAME)`。

> 已知限制：未应用 assimp 节点层次变换（只取 mesh 几何）；动画/骨骼未处理。角色朝向靠外部矩阵 `rotate(-90°X)` 修正。

### 7.6 渲染 Pass（`src/Passes/`）

**OffscreenRT**：离屏渲染目标（HDR `R16G16B16A16_FLOAT`），同时有 RTV（画）和 SRV（采样）。

```cpp
class OffscreenRT {
public:
    void create(D3D12Context&, UINT w, UINT h, ID3D12DescriptorHeap* sharedHeap,
                UINT srvIndex, const float clearColor[4]);
    void createUav(D3D12Context&, UINT w, UINT h, ID3D12DescriptorHeap* sharedHeap,
                   UINT srvIndex, UINT uavIndex); // compute 用
    void transitionColor(D3D12_RESOURCE_STATES state); // 资源状态 barrier
    ID3D12Resource* resource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle() const;
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle() const;
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle() const;
};
```

要点：**所有 RT 的 SRV 必须写在同一个共享堆的不同 index**（D3D12 同时只能一个 CBV_SRV_UAV 堆，`ERRORS.md` #2）。

**FullScreenPass**：通用全屏 pass（1~2 SRV + 可选 CBV），Bloom 各 pass 用。内部全屏三角形由 `SV_VertexID` 生成。

**ComputePass**：计算管线。

```cpp
class ComputePass {
public:
    // numSrvRanges / numUavRanges / hasCbv / numSamplerRanges
    ComputePass(D3D12Context&, const std::vector<uint8_t>& cs,
                int numSrvRanges = 0, int numUavRanges = 1,
                bool hasCbv = false, int numSamplerRanges = 0);
    void dispatch(ID3D12DescriptorHeap* heap, D3D12_GPU_DESCRIPTOR_HANDLE uav,
                  UINT x, UINT y, UINT z, D3D12_GPU_DESCRIPTOR_HANDLE srvBase = {},
                  ID3D12GraphicsCommandList* cmdList = nullptr);
    // 多 SRV/UAV/CBV/sampler 的重载
};
```

**根签名结构**（ComputePass）：UAV 描述符表 + 可选 SRV 表（多个独立表）+ 可选 b0 CBV + 可选 sampler 表。

> **描述符表 offset 大坑**：多个 SRV range 若各自是**独立的 root parameter（每表 1 描述符）**，offset 必须 `=0`；同一表内多描述符才用 offset 递增。写反会导致 shader 读错资源（`ERRORS.md` #3，Chromatics LUT 全黑的根因）。

### 7.7 MeshRenderer（`src/Renderers/MeshRenderer.h/cpp`）

**职责**：通用物体绘制——天空盒、程序化几何、远景模型。也负责深度缓冲的创建与共享。

根签名（4 参数）：

| 参数 | 类型 | 绑定 | 内容 |
|---|---|---|---|
| 0 | CBV（root descriptor） | b0 | PerObjectCB |
| 1 | CBV（root descriptor） | b1 | FrameCB |
| 2 | Descriptor table (SRV) | t0 | 物体纹理 |
| 3 | Descriptor table (Sampler) | s0 | 采样器 |

每物体 CBV（PerObjectCB）：

```cpp
struct PerObjectCB {          // 256B
    glm::mat4 viewProj;       // proj * view
    glm::mat4 world;          // model
    glm::mat4 invTransModel;  // world 的逆转置（法线/描边用）
    glm::vec4 baseColor;
    float outlineWidth;       // 描边宽度（参考 outlineWidth=10）
    glm::vec3 _pad;
};
```

每帧 CBV（FrameCB）：

```cpp
struct FrameCB {
    glm::vec4 lightDir;              // 指向光源
    glm::vec4 lightColor;            // w = 强度
    glm::vec4 ambientColor;
    glm::vec4 cameraPosition;
    float invProjection00;           // 1/proj[0][0]，描边 FOV 补偿
    float invScreenAspectRatio;
    float outlineDistanceFadeFactor; // 参考 ArcOutline=0.001
    float _pad;
};
```

DrawItem：

```cpp
struct DrawItem {
    const Mesh* mesh;
    glm::mat4 model;     // 世界变换
    glm::vec4 color;     // 材质基础色
    UINT srvIndex;       // 纹理在 SRV heap 的索引
    bool outline;        // 是否描边
    float outlineWidth;  // 描边宽度
    bool disableDepth;   // 天空盒等不写深度
    bool sky;            // 用天空盒 PSO
};
```

**三种 PSO**：
- 主 PSO：`CullMode=NONE` 双面 + `FrontCCW=TRUE` + D32 + 物体格式 RTV。
- 描边 PSO：`CullMode=FRONT`（背面外扩形成轮廓）+ 只声明 POSITION/NORMAL 输入。
- 天空盒 PSO：`DepthFunc=ALWAYS + DepthWriteMask=ZERO + DSVFormat=D32_FLOAT` + 只声明 POSITION/TEXCOORD。

**描边原理**（对齐参考 `ArcOutline.mat`）：

```hlsl
// Outline.hlsl（世界空间沿法线外扩）
worldPos += outlineWidth * (distanceFade * fovFade) * worldNormal;
// distanceFade = min(dist * outlineDistanceFadeFactor, 0.002)
// fovFade = invProjection00 * invScreenAspectRatio
```

**render 流程**：视口 → 清深度 → 绑定 RTV+DSV → 设描述符堆/根签名 → 写 FrameCB(b1) → 描边 pass（`item.outline` 的物体先画，Cull FRONT 背面外扩）→ 主 pass（逐物体写 PerObjectCB + 按 srvIndex 绑 SRV 表 + DrawIndexedInstanced）。

深度缓冲：`D32_FLOAT` 默认堆 + DSV 堆；深度 SRV 写共享堆 index 2（后处理雾采样用）；`transitionDepthTo(DEPTH_WRITE/DEPTH_READ)` 切换状态（水面 pass 只读深度）。

### 7.8 CharacterRenderer（`src/Renderers/CharacterRenderer.h/cpp`）

**职责**：三个角色的材质渲染。本项目最复杂的渲染器——**每种材质一个 PSO**，严格照搬参考项目材质 shader。

#### 材质枚举（`CharacterMaterial`）

```cpp
enum class CharacterMaterial : int {
    ArcToon = 0,        // BattleMage（albedo+sss+ilm，动漫硬色阶 + GGX + IBL）
    EndfieldBody,       // Yvonne Body/Neck（法线 + color ramp + 次表面）
    EndfieldPBR,        // Yvonne Cloth/Tail（PBR，cull NONE 双面）
    EndfieldThinFilm,   // Yvonne ThinFilm（半透明薄膜）
    EndfieldFace,       // Yvonne Face（SDF 光照）
    EndfieldHair,       // Yvonne Hair（双法线各向异性 GGX）
    EndfieldIris,       // Yvonne Iris
    EndfieldEyelash,    // Yvonne Eyelash（半透明）
    EndfieldEyeShadow,  // Yvonne EyeShadow（半透明，Unlit）
    EndfieldHairShadow, // Yvonne HairShadow（半透明）
    DefaultLit,         // Ankha/Ball/BoxB（标准 PBR）
    AnkhaMask,          // Ankha 身体（ShellFurMask_VS + DefaultLit_FS）
    ArcCardShadow,      // BattleMage 卡面阴影（半透明黑卡）
    Count
};
```

每材质有独立的 VS/FS/PSO 配置：

```cpp
struct CharacterMaterialDesc {
    D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK; // 对应 .mat cullMode
    bool depthWrite = true;   // Transparency 域 = false
    bool blend = false;       // Transparency 域 alpha blend
};
```

#### 材质参数（对应 shader 的 MeshUniform）

```cpp
struct CharacterMaterialParams {
    glm::vec4 materialParams;   // Endfield: x=rimLightWidth y=rimLightIntensity z=subsurfaceMFP w=viewAttenuation
    glm::vec4 materialParams2;  // DefaultLit: x=metallic y=specular z=roughness w=anisotropy
    glm::vec4 materialParams3;  // x=subsurfaceOpacity y=irisDepth z=sizeExpand(Ankha) w=depthOffset(Ankha)
    glm::vec4 outlineColor;     // 描边颜色 / HairShadow 阴影色 / CardShadow 颜色
    glm::vec4 outlineParams;    // x=outlineWidth y=outlineDistanceFadeFactor
};
```

#### 绘制项（CharacterDrawItem）

```cpp
struct CharacterDrawItem {
    const Mesh* mesh;
    glm::mat4 model;
    CharacterMaterial material;
    UINT textureGroup;          // 材质纹理组（见 materialGroups）
    UINT outlineTextureGroup;   // 描边纹理组（EndfieldOutline 采样 t3=stTex）
    CharacterMaterialParams params;
    bool outline;
    glm::vec3 objectUp, objectForward, objectRight; // EndfieldFace SDF 光照用
};
```

#### 纹理布局

每个材质一组 **6 槽** = [5 材质纹理, ambient(DefaultSky IBL)]，在 Main.cpp 的 `materialGroups` 定义。示例：

```cpp
// 0 ArcToon：albedo + sss + ilm（+2 占位黑）
{ &battleMageBody, &battleMageSss, &battleMageIlm, &placeholderBlack, &placeholderBlack },
// 1 EndfieldBody：albedo + normal + ramp（+white +white，供描边 stTex）
{ &yvonneBody, &yvBodyN, &yvBodyRD, &placeholderWhite, &placeholderWhite },
// 5 EndfieldFace：albedo + sdf + ramp + mask + highlightMask
{ &yvonneFace, &yvFaceSDF, &yvFaceRD, &yvFaceCmM, &yvFaceHlM },
// 12 AnkhaBody：albedo + normal
{ &ankhaBody, &ankhaBodyN, &placeholderBlack, &placeholderBlack, &placeholderBlack },
```

缺纹理槽用占位黑/白纹理（避免 null SRV）。

#### 根签名与 CBV

- 根签名：b0 MeshUniform + b1 GlobalUniform（CBV）+ t0-t5 SRV 表 + s0-s5 采样器表。
- MeshUniform（每物体 512B，CbvStride）：
  ```
  model / invTransModel / minAABB / objectID / maxAABB / indexCount
  + materialParams / materialParams2 / materialParams3 / outlineColor
  + objectForward / objectRight / objectUp / outlineParams
  ```
- GlobalUniform（每帧 768B）：完整照搬参考 GlobalUniform（view/projection/viewProjection/invView/invProjection/invViewProjection/dirLightDirection/dirLightColor/cameraPositionNear/cameraVectorFar/screenResolution/invScreenResolution/cursorPosition/time/deltaTime/frustumPlane×6/cameraFov/aspect/frameIndex）。

#### 渲染顺序

```text
描边 pass 先画（item.outline 的物体，CullMode=FRONT 背面外扩，depth LESS+write）
→ 主体 pass（按 charItems 顺序）
```

半透明材质（ThinFilm/Eyelash/EyeShadow/HairShadow/CardShadow）在所有不透明**之后**画（Main.cpp 里排好顺序）。BattleMage 4 mesh 中 mesh1(CardShadow) 也排在最后。

**重要约束**：`render()` 有 `cbvOffset` 参数——同一帧多次调用必须传不同偏移，否则共享 CBV slot 0 被覆盖（`ERRORS.md` #17）。

### 7.9 TerrainRenderer（`src/Renderers/TerrainRenderer.h/cpp`）

**职责**：程序化地形（照搬参考项目，严格 4096 分辨率）。

数据纹理（全部 4096²，compute 生成，照搬参考 textureMode）：

| 纹理 | 格式 | 用途 |
|---|---|---|
| TerrainHeight | R32G32_FLOAT（.x 地形 .y 水面） | 高度 |
| TerrainNormal | R8G8B8A8_SNORM | 法线 |
| TerrainSurface | R8G8B8A8_UNORM | 表面类型（草地/沙滩/雪/湿度） |
| WaterFlow | RGBA16F | 水流 |
| WaterSediment | R32_FLOAT | 沉积物 |
| WaterVelocity | R16G16_FLOAT | 流速 |
| Meteorograph | R32G32B32A32_FLOAT（128²） | 气象（风/湿度/温度） |
| Pluviometer | R16G16_FLOAT（128²） | 降雨 |

表面贴图（参考 `TerrainMesh.mat`）：`MossyGrass`（BC/N/AoRDp）+ `ButtressRoot` + `RockCliff` + `DefaultSky`（IBL）。

描述符堆 20 槽：SRV 0-11（数据纹理 + 6 表面贴图 + ambient）+ UAV 12-19。

初始化 dispatch 序列（一次）：height → normal → surface → meteorograph init → pluviometer init → TerrainData（Once）。

渲染：256×256 网格（interval 4.0），`rotate(-90°X)` 转 y-up。VS 采样 TerrainHeight 位移（`LoadTerrain` + globalOffset），FS 完整 PBR（6 纹理 + slope 混合 + `VariantTerrainSurface` + `DefaultShading`）。

### 7.10 计算管线（TerrainData / Meteorograph / FractalNoise）

**Meteorograph（气象，每帧）**：128² 模拟。读 TerrainHeight，写 Meteorograph + Pluviometer。用 `RWTexture2D` 读+写（UAV Load，规避 D3D12 同资源 SRV+UAV 状态冲突）。

**TerrainData（水文，一次性 Once）**：浅水方程（pipe model）+ 侵蚀/沉积 + 热侵蚀 + 蒸发 + 降雨。读 Meteorograph/Pluviometer（带 sampler 双线性采样，`ComputePass` 支持 sampler），写 height/normal/waterFlow/sediment/velocity/surface。**`dispatchFrequency=Once` 只在初始化跑一次**——每帧跑会导致地形持续崩坏（`ERRORS.md` 4.5）。

**FractalNoise**：3 纹理（Simplex R32F / Worley R32F / Normal R8G8B8A8_SNORM），一次性生成，供水面泡沫 / 毛 / 草采样。

**ComputePass 使用要点**（一再踩坑，务必记住）：
- 每个被初始化的 UAV 纹理都要显式 barrier 回采样态（`ERRORS.md` #6）。
- 同一个纹理既当 SRV 又当 UAV → 用 `RWTexture2D` 读+写（UAV Load）规避。
- root CBV 的生命周期必须覆盖命令列表 Close+Execute（`ERRORS.md` #5 #921）。
- 每帧模拟的 time/deltaTime 用共享上传堆 CBV（`meteorTimeCbv`），deltaTime clamp 防爆炸。

### 7.11 Grass（`src/Renderers/GrassRenderer.h/cpp`）

**职责**：实例化草地（照搬参考 `GrassInstancing.mat` + `GrassData.mat`）。

数据流：

```text
GrassData_CS（一次性）：2048² 草地分布纹理
GrassInstancing_CS（每帧）：相机前方 128×128 网格
  → 视锥/坡度剔除 + InterlockedAdd 生成实例
  → InstanceBuffer（16384 实例）+ IndirectBuffer
ExecuteIndirect（间接绘制）InstancingGrass.fbx（250 顶点/75 索引）
```

**ExecuteIndirect（D3D12 间接绘制）**：

```cpp
// 1. CommandSignature（声明间接参数布局）
D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
// 注意：DRAW_INDEXED 不修改 root 参数 → pRootSignature 传 nullptr！
D3D12_COMMAND_SIGNATURE_DESC csDesc{};
csDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); // 20B
csDesc.NumArgumentDescs = 1;
csDesc.pArgumentDescs = &argDesc;
device->CreateCommandSignature(&csDesc, nullptr, IID_PPV_ARGS(&_commandSignature));

// 2. 执行
commandList->SetGraphicsRootSignature(_graphicsRootSig);
commandList->SetGraphicsRootConstantBufferView(...);
commandList->ExecuteIndirect(_commandSignature.Get(), 1, _indirectBuffer.Get(), 0, nullptr, 0);
```

**IndirectBuffer 状态链**：`UAV（CS 写）→ COPY_DEST（CPU 写 indexCount）→ INDIRECT_ARGUMENT（ExecuteIndirect 读）→ 回 UAV`。每帧按序 barrier。

**四个 bug 教训（详见 WORKLOG）**：
1. VS b0 cbuffer 布局错位 → 垃圾矩阵 → 拉伸大三角形（`ERRORS.md` #13）。
2. FS b1 的 cameraPosition/dirLight 要转 z-up（草顶点是 z-up）。
3. `cameraVector = cameraPos - target` 语义（草在镜头前方）；相机前向取矩阵**第三行取反**。
4. Gribb-Hartmann 视锥平面必须**归一化**才能做球剔除。

**z-up 坐标适配**：草的生成/剔除在 z-up 空间（与 TerrainHeight 纹理坐标系一致），渲染时 `viewProjection = proj * view * rotate(-90X)`。相机前向 y-up → z-up：`fwd_zup = (x, -z, y)`。

### 7.12 Boids（`src/Renderers/BoidsRenderer.h/cpp`）

**职责**：鸟群群集（照搬参考 `BoidInstancing.mat`）。1024 只纸飞机，GPU 每帧 O(N²) flocking 模拟。

数据流：

```hlsl
BoidBuffer（1024×128B 属性）+ InstanceBuffer（1024×192B 实例矩阵）
  → attribute init（一次性）
  → instance init（一次性）
  → 每帧 sim（读旧状态 → 算 flocking → 写新状态）
  → BoidInstancing_VS（SV_InstanceID 读 InstanceBuffer）+ DefaultLit_FS 实例化绘制
```

BoidBufferElement（照搬参考，float3+float 交替，每字段 16B 对齐）：

```cpp
struct BoidBufferElement { // 128B
    glm::vec4 acceleration;    influenceRadius;    maxAcceleration;    turnSpeed;
    glm::vec4 velocity;        curiosity;         maxVelocity;        perception;
};
```

**根签名**（graphics）：b0 MeshUniform + b1 GlobalUniform + **VS t0=InstanceBuffer（VERTEX 可见性）+ PS t0-t5 纹理（PIXEL 可见性）**——同一 register 不同 stage 不同绑定是合法的。

要点：
- InstanceBuffer 每帧在 UAV（sim 写）与 SRV（渲染读）之间 barrier。
- boid 模拟 z-up（照搬参考）；地形回避用 z-up 平面坐标（与 TerrainHeight 纹理坐标系一致）；渲染 model = `scale(0.2) * rotate(-90X)`。
- 三个 ComputePass（attr/inst/sim）必须是成员 `unique_ptr`（局部释放导致 #921，`ERRORS.md` #5）。

### 7.13 Fur（AnkhaFur）（`src/Renderers/FurRenderer.h/cpp`）

**职责**：ShellFur 壳层毛。`AnkhaFur.fbx` + 8 壳层实例化（当前 5 层）。

原理（VS）：

```hlsl
// 每实例向外推一层壳
position += normal * (instanceID + 1) * shellInterval; // shellInterval=0.03
// FS：FractalWorleyNoise 生长噪声裁剪
if (max(1.5 - noise, 0) < (instanceID + 0.5) * rcp(instanceCount)) clip(-1);
```

CBV：b0 FurUniform（model/invTransModel/viewProjection/objectID/instanceCount/shellInterval，208B）+ b1 FurGlobalUniform（cameraPosition/dirLightDirection/dirLightColor，48B）。纹理：t0 albedo + t1 normal + t2 FractalWorley(R32F) + t3 FractalNormal(SNORM)。

绘制：`DrawIndexedInstanced(indexCount, instanceCount)`。FS 的 worldPosition 是 y-up（model 含 rotate -90X）→ cameraPosition/dirLight 传 y-up。

> 已知限制：AnkhaFur.fbx 含衣服几何，毛会覆盖衣服（用户接受）。根治需重新导出不含衣服的 fbx 或加 mask。参考 ShellFur_FS 实际用草地纹理做主色，本工程用 Ankha 身体纹理（橙对橙对比弱，但用户选择保留）。

### 7.14 Particle（`src/Renderers/ParticleRenderer.h/cpp`）

**职责**：4096 粒子（照搬参考 `ParticleExample.mat`）。

数据流：

```text
ParticleExampleInitializer_CS（一次性）
  → ParticleExample_CS（每帧：风场/摩擦/生命周期/重生，读 TerrainHeight+Meteorograph）
  → GS 公告板（POINT → TriangleStream 4 顶点）
  → FS（圆形 alpha + sceneColor 折射混合）
```

**点精灵方案**：D3D12 `SV_PointSize` 不可靠 → 用 **GS 公告板**生成 quad：

```hlsl
// ParticleExample_GS.hlsl：POINT → TriangleStream
[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<GSOutput> stream) {
    float2 ndcHalf = pointSize * ndcHalfPerPixel; // pointSize 由 VS 算（随深度反比）
    // 生成 4 个顶点（±ndcHalf），nointerpolation 传 centerPosition 给 FS 画圆
    ...
}
```

必要修复（参考版没有但必须的，否则效果崩）：
- **camDelta**：粒子随相机平移（`positionOut = p.position + (cameraPosition - prevCameraPosition)`），否则粒子留原地、转视角看不到。
- init `elapseTime` 随机（`maxLifeTime * Hash`）——同步重生 = 周期性全透明。
- index=0 除零（`1.0/(index+1.0)`）。
- GS 跳过 `P.w <= 0`（相机后方粒子投影退化聚线）。
- pointSize clamp 上限（贴相机粒子巨大 quad 覆盖屏幕）。
- `boundarySize > regeneratedRange`（相等 → 边缘粒子异常重生 alpha 常 0）。

PSO：alpha blend + depthWrite false + DepthFunc LESS + cull BACK。渲染前把 offscreen 拷贝到 sceneCopy（折射采样，D3D12 禁止同资源 RTV+SRV）。

> 已知遗留：平视时粒子在屏幕中间集中（球体分布投影到中间的固有问题，用户接受当前效果）。

### 7.15 Water（`src/Renderers/WaterRenderer.h/cpp`）

**职责**：水面（照搬参考 `WaterMesh.mat`）。独立 256² 网格跟随相机（globalOffset）。

渲染顺序：场景渲染到离屏后 → `copySceneColor`（offscreen → waterSceneCopy）→ 水 pass（离屏 RT + 只读 DSV）。

**Water_VS**：采样 TerrainHeight.y 水位高度位移 + globalOffset（网格跟随相机）。

**Water_FS**：折射（waterSceneCopy 采样）+ TerrainNormal.zw 水法线 + WaterSediment/WaterVelocity + FractalSimplexNoise 泡沫 + depthFade + DefaultShading。

**三个必要适配**（参考项目为 reverse depth + z-up）：
1. 深度淡出：参考 `min(input.position.z, 0.1)` → 本工程标准深度 **`min(1.0 - input.position.z, 0.1)`**。
2. 高度差用 `.y`（参考 `.z`）；cameraVector 水平用 `.xz`（参考 `.xy`）。
3. `SV_ISFRONTFACE` 只能作为 PS 输入参数。

描述符堆 9 槽：sceneCopy/depth/height/normal/sediment/velocity/fractal SRV/ambient/fractal UAV。PSO cullMode=NONE + depthWrite=false + depth test LESS。

### 7.16 PostProcess / Bloom / Chromatics（`src/Passes/PostProcess.*` + `Shaders/`）

**Bloom（6 级金字塔，照搬参考 AfterglowBloomPassSet）**：

```text
down[0..5] = 6 级降采样（1/2 → 1/64，4-tap box 平均）
从最细级（down[5]）向上：
  horz[i] = HorizontalBlur(down[i], 乘 bloomIntensity[i])   // gaussianSigma3 15-tap
  combined[i] = VerticalBlur(horz[i]) + combined[i+1]       // 叠加上一级（最小级不叠加）
最终 combined[0] = 最终 bloom，供 PostProcess 采样
```

分级强度：`{0.02, 0.03, 0.03, 0.06, 0.07, 0.175}`（down[0..5]，最小级最亮）。每级有独立 RT 和 BloomParams cbuffer（intensity + useCombinedTexture）。

**Chromatics（ACES 完整管线，照搬参考）**：

- C++ `src/Utils/ACES.cpp`：`GenerateOutputTransformTables()` 生成 ACES2 参数 + 三张表（ReachM/UpperHullGamma/GamutCusps，各 362 个 float4）。
- 上传 4 个 StructuredBuffer（params + 3 表）→ chromaticsHeap（6 desc：0-3 表 SRV，4=LUT UAV）。
- `Chromatics_CS.hlsl` 一次性烘焙 **32³ RGBA16F 3D LUT**（dispatch 4×4×4）→ barrier 回 SRV → LUT SRV 写共享堆 index 6。
- PostProcess `LUTMapping`：`LinearToLogColor` + 3D LUT 采样 + **LinearToSRGB**（swapchain 是 UNORM，参考靠 sRGB 自动 EOTF）。

**PostProcess 最终合成**（照搬参考 PostProcess_FS 顺序）：

```text
sceneColor → 色差(0.0025) → 重建世界坐标
→ ambientTexture(DefaultSky) 方向采样（envColor，含 HeightFog 高度雾）
→ UnderwaterMask（水下：近平面 vs TerrainHeight 水面）
→ + Bloom combined + SkylightShaft（光轴，8 层）
→ AtmosphericDust（尘埃）→ Vignette（暗角）
→ LUTMapping（ACES 3D LUT）→ LinearToSRGB
```

根签名：b0 参数 CBV + t0-t6 SRV 表（scene/bloom/depth/noise/ambient/TerrainHeight/ColorLUT）+ s0-s3 采样器表。

调试键：`1/2/3` 开关色差/尘埃/暗角；`5/6/7/8` 显示 noise/场景原色/bloom/LUT；`0` 全关 `4` 全开。

### 7.17 GUI（`src/GUI/GuiRenderer.h/cpp`）

**职责**：ImGui 集成（照搬参考 AfterglowGUI 的浅色圆角风格）。

```cpp
class GuiRenderer {
public:
    GuiRenderer(D3D12Context& context, GLFWwindow* window);
    void beginFrame();  // ImGui NewFrame + `~` 键切换 Console
    void render();      // ImGui::Render + RenderDrawData（须先设 heap + RTV）
    bool wantsCaptureMouse() const;    // GUI hover 时相机暂停
    bool wantsCaptureKeyboard() const; // InputText 聚焦时快捷键暂停
    double consumeScrollOffset();      // 滚轮（imgui 接管 GLFW 回调后从此读）
    bool consoleOpen;
};
```

- **样式**：`StyleColorsLight()` + `WindowBg 白 0.8` + 圆角 16 + `ScaleAllSizes(1.2)` + 标题居中。
- **字体**：`calibrib.ttf` 14px（参考 `font::defaultFontPath`），并**合并 Windows 中文字体**（探测 `msyh.ttc`/`simhei.ttf`/`simsun.ttc`，`MergeMode + GetGlyphRangesChineseSimplifiedCommon`，Oversample=1）。
- **输入**：`ImGui_ImplGlfw_InitForVulkan(false)` + 手动转发 GLFW 回调（key/mouseButton/cursorPos/scroll/char）。
- **D3D12 后端**：`ImGui_ImplDX12_InitInfo`（填主 command queue；独立 shader-visible SRV 堆 1 描述符；用 `LegacySingleSrvCpu/GpuDescriptor` 迁移字段）。
- **绘制**：`render()` 内 RenderDrawData 前必须 `SetDescriptorHeaps(imgui 堆)` + `OMSetRenderTargets(后台缓冲)`（imgui_impl_dx12 不自己做，否则 `ERRORS.md` #708）。

**Console 面板**（Main.cpp 构建）：`~` 切换。结构照搬参考：
- NoTitleBar + 自定义标题行 + 红色关闭按钮。
- 输出区（日志，反射按钮返回值显示）。
- RenderStatus / System / Scene / Audio 四个 checkbox 子面板开关。
- RenderStatus：设备名/FPS/PlotLines + 7 个后处理 checkbox + 全开/全关按钮。
- System：相机信息 + 切换相机/重置相机按钮 + 反射控制按钮。
- Scene：实体 Selectable 列表 + 详情。
- Audio：播放/暂停/上/下/停止/音量/循环/曲目列表。
- 底部 `>` 输入框。

### 7.18 反射按钮（`src/Utils/GuiInspect.h` + `vsbuild/Libraries/Inreflect/`）

**职责**：把类的成员函数"反射"成 GUI 按钮（仿参考 AfterglowPanel 的反射上下文）。

用法三步：

```cpp
// 1. 定义控制类并注册（INR_CLASS/INR_FUNCS 宏）
class GuiControl {
public:
    void toggleCameraMode();
    void setLightDirection(glm::vec3 dir);
    float getLightIntensity() const;
    ...
};
INR_CLASS(GuiControl) {
    INR_FUNCS(
        INR_FUNC(toggleCameraMode),
        INR_FUNC(setLightDirection),
        INR_FUNC(getLightIntensity),
        ...
    );
};

// 2. 实例化并绑定
GuiControl guiControl;
guiControl.cameraModePtr = &cameraMode;
...

// 3. 在 GUI 里渲染成按钮
guiInspect::renderFunctions<GuiControl>(guiControl, logCallback);
```

**Inreflect 原理**（照搬参考项目）：
- `INR_CLASS` 宏生成 `Inreflect<Class>` 特化，`INR_FUNCS/INR_FUNC` 把成员函数指针 + 名称打包成 `std::tuple`（编译期元数据）。
- `FunctionTraits` 推导函数的返回类型/参数类型；`forEachParam` 遍历参数。
- `renderFunctions` 遍历所有注册函数，为每个生成参数输入控件 + Call 按钮；点击调用，返回值经 `formatValue` 写日志。

`renderInput<T>` 支持类型：bool/int/uint/float/double/glm::vec3/vec4/枚举（枚举用 Inreflect 枚举属性生成 Combo）。**NOMINMAX 必须在 include Inreflect.h 前定义**（`ERRORS.md` 5.1）。

### 7.19 Audio（`src/Audio/AudioSystem.h/cpp`）

**职责**：音乐播放（miniaudio 单头文件库，`#define MINIAUDIO_IMPLEMENTATION` 在 .cpp 定义实现）。

```cpp
class AudioSystem {
public:
    bool scan(const std::string& directory); // 扫描 mp3/wav/flac/ogg/m4a，按名排序
    void play(int index); void pause(); void resume(); void stop();
    void next(); void previous();
    void setVolume(float v); void setLoopCurrent(bool loop);
    void update();  // 每帧调用：处理自动切歌（音频线程回调只置原子标志）
    bool isPlaying() const; bool isPaused() const;
    int currentIndex() const;
    const std::vector<std::string>& titles() const; // UTF-8 歌名
    const std::string& currentTitle() const;
};
```

实现要点：
- `ma_engine` + `ma_sound`（`MA_SOUND_FLAG_STREAM` 流式；失败回退非流式）。
- **暂停 = 记录 pcm cursor + stop；恢复 = seek + start**（miniaudio 无原生 pause）。
- 自动切歌：`ma_sound_set_end_callback` 音频线程回调只置 `std::atomic<bool> _autoAdvance`，主线程每帧 `update()` 处理切下一首。
- **中文路径**：必须用 **`ma_sound_init_from_file_w`**（wide API，内部 `_wfopen_s`）——`ma_sound_init_from_file` 用 `fopen_s`（ANSI 码页）中文路径失败（`ERRORS.md` 5.2）。
- `_playlist` 存 `std::wstring`（wide 路径），`_titles` 存 UTF-8（imgui 显示 + 中文字体）。

---

## 7.20 HLSL Shader 解读

> 本章逐个解读本项目核心 HLSL 的算法逻辑。所有 shader 都**逐行照搬参考项目**（仅做 D3D12 适配：显式 register、cbuffer、`mul` 顺序、y-up 坐标）。读懂本章就理解了"渲染效果是怎么算出来的"。

### 7.20.1 公共库：Common.hlsl

被几乎所有 shader include 的数学/工具函数：

| 函数 | 作用 |
|---|---|
| `Snorm(v) / Unorm(v)` | 归一化单位 [0,1] ↔ [-1,1]（法线纹理编码） |
| `ReconstructNormal(xy)` | 从法线贴图 XY 重建 Z：`(xy, sqrt(1-dot(xy,xy)))` |
| `ScaleNormal(n, i)` | 法线强度缩放（XY 放大，Z 保持） |
| `BlendAngleCorrectedNormals(a, b)` | 角校正法线混合（两个法线贴图叠加），UE 标准做法 |
| `LoadNormalTexture` | 采样法线贴图并 Snorm 解码 |
| `ClampVectorWithLength(v, maxLen)` | 限制向量长度（风向/风速用） |
| `ProjectVector/ProjectDirection` | 向量/方向投影到平面（EndfieldFace 把光方向投影到脸平面用） |
| `InverseRigidTransform(m)` | 刚体变换求逆（旋转转置 + 平移回移，Boid/Grass 实例矩阵用） |
| `SafePow/CopySign/WrapTo360/MidPoint` | ACES/Chromatics 数学工具 |

**重点理解 `BlendAngleCorrectedNormals`**（草地/地形多层法线混合核心）：

```hlsl
float3 BlendAngleCorrectedNormals(float3 baseNormal, float3 additionalNormal) {
    float3 n0 = float3(baseNormal.xy, baseNormal.z + 1.0);
    float3 n1 = float3(-additionalNormal.xy, additionalNormal.z);
    return normalize(dot(n0, n1) * n0 - n0.z * n1);
}
```

它把两个单位法线"角度正确"地混合：`n0` 把 base 的 z+1（拉成半球向量），`n1` 把 additional 的 xy 取反，然后做球面投影。比简单 lerp 在陡坡处不会拉平法线。

### 7.20.2 公共库：ColorConversion.hlsl

颜色空间工具：

- `Grayscale`：亮度（0.299/0.587/0.114）。
- `Desaturation(c, v)`：按 v 向灰度插值（v>0 去饱和，v<0 增饱和——参数负值时 `lerp(c, gray, -0.35)` 会超出灰度方向增强饱和度）。
- `HSVToLinearRGB / LinearRGBToHSV / HueShift`：HSV 颜色空间转换。`HueShift(c, offset)` = 转 HSV → hue 加 offset（wrap）→ 转回。**VariantTerrainSurface 用它做沙滩/湿度变色**。

### 7.20.3 公共库：ShadingModels.hlsl（PBR 核心）

这是整个项目的**光照核心**。所有材质最终都调到它的 `DefaultShading` 系列函数。

**`ShadingContext`（输入结构）**：baseColor / metallic / specular / roughness / ambientOcclusion / anisotropy / normal / tangent / bitangent / view / subsurfaceColor / opacity。

**`SpecularGGX`（高光）**：

```hlsl
float3 SpecularGGX(in BxDFContext bxdf, float roughness, float anisotropy, float3 specularColor) {
    float alpha = roughness * roughness;
    float2 anisotropicAlpha = AnisotropicSquaredRoughness(alpha, anisotropy);
    // 三项乘积：分布 × 可见性 × Fresnel
    float3 distribution = DistributionAnisotropicGGX(anisotropicAlpha.x, anisotropicAlpha.y, bxdf.noh, bxdf.xoh, bxdf.yoh);
    float3 visibility   = VisibilityAnisotropicSmithJoint(...);
    float3 fresnel      = FresnelSchlick(specularColor, bxdf.voh);
    return distribution * visibility * fresnel;
}
```

标准 Cook-Torrance 微表面模型。各向异性版本用于**头发**（anisotropy=0.85）。

**`EnvLighting`（IBL 环境光）**——用 DefaultSky equirect 贴图做环境反射：

```hlsl
// 反射向量 → equirect UV（方位/俯仰）
float normalizePhi = atan2(reflectionVector.y, reflectionVector.x) * 0.5/π + 0.5;
float normalizeTheta = acos(reflectionVector.z) / π;
// 粗糙度决定采样 mip 级（越粗糙越模糊）
float envMipLevel = clamp(sqrt(roughness) * 10, 0, mipCount-1);
float3 envColor = ambientTex.SampleLevel(samp, float2(phi, theta), envMipLevel).xyz;
// 高光 = 环境色 × EnvBRDF 近似（F0/粗糙度/NoV）
lighting.specular = envColor * EnvBRDFApprox(specularColor, roughness, nov);
// 漫反射 = 简化（参考原样）：非金属 × 基础色 × 0.08
lighting.diffuse = max(1 - metallic, 0) * baseColor * 0.08;
```

> 地形"反射天空的紫色"就来自这里（DefaultSky 晚霞紫色，见 `ERRORS.md` 3.4）。

**`DefaultShading`（最终合成）**：

```hlsl
float3 specularColor = ComputeF0(specular, baseColor, metallic); // 菲涅尔 F0
// 方向光：漫反射(Burley) + 高光(GGX)，乘 dirLightColor.rgb * w(强度)
lighting.diffuse += directionalLighting.diffuse * directionalLightScale;
lighting.specular += directionalLighting.specular * directionalLightScale;
// 环境光：IBL 反射
lighting.diffuse += envLighting.diffuse * ambientOcclusion;
lighting.specular += envLighting.specular * ambientOcclusion;
```

还提供 `SubsurfaceShadingAnisotropic`（次表面版，Ankha 用）——多了透射项：inScatter（透过材质的光，`pow(NdotL反,12)`）、backScatter（wrap 光照）、transmittedColor（次表面颜色衰减）。

**`BxDFContext` 初始化**（`InitBxDFContext`）：计算 `noh/nol/nov/voh/xoh/yoh` 等点积，供 GGX 用。

### 7.20.4 公共库：EndfieldCommon.hlsl（Toon 专用）

**`EndfieldRimLighting`（边缘光）**——Yvonne 皮肤/脸的轮廓光：

```hlsl
// 边缘判定：1 - NoV，smoothstep 按 width 卡阈值（width 越小边缘带越窄）
float rimStepMin = clamp(0.9 - width, 0, 0.99);
float rimStepMax = clamp(1.0 - width, 0.01, 1.0);
float rimLighting = smoothness * smoothstep(rimStepMin, rimStepMax, 1.0 - nov);
// 光源方向因子：背光侧也有（Vol + Nol 各 +0.5 wrap）
rimLighting *= max(vol, 0) * max(nol + 0.5, 0) * 2.0;
// 光源颜色 + 遮挡
rimLighting *= dirLightColor.xyz * dirLightColor.z * occlusion;
```

### 7.21 角色着色器

### 7.21.1 CharacterCommon.hlsl（角色公共）

定义两个 cbuffer + 顶点结构：

- **b0 MeshUniform**（每物体）：`model / invTransModel / minAABB / objectID / maxAABB / indexCount` + `materialParams / materialParams2 / materialParams3 / outlineColor / objectForward / objectRight / objectUp / outlineParams`。
- **b1 GlobalUniform**（每帧）：完整照搬参考 GlobalUniform（view/proj/viewProjection/inv×3、dirLightDirection/Color、cameraPosition/cameraVector、screenResolution、time/deltaTime、frustumPlane×6、cameraFov/aspect）。
- `VSInput`：pos/uv/normal/color/tangent；`VSOutput`：SV_POSITION + worldPosition/worldNormal/worldTangent/worldBitangent/color/texCoord0/objectID。
- `UnlitVSOutput/UnlitFSInput`：描边/眼影等 unlit 材质用（position + texCoord0 [+ isFrontFace]）。

### 7.21.2 CharacterVS.hlsl（= 参考 DefaultLit_VS）

```hlsl
VSOutput VSMain(VSInput input) {
    float4 worldPosition = mul(model, float4(input.pos, 1.0));
    output.position = mul(viewProjection, worldPosition);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul((float3x3)invTransModel, input.normal));
    output.worldTangent = normalize(mul((float3x3)invTransModel, input.tangent));
    output.worldBitangent = cross(output.worldNormal, output.worldTangent); // 从法线×切线推
    output.color = input.color;       // 顶点色（描边外扩权重/bitangent 翻转标记等）
    output.objectID = (uint)objectID;
    output.texCoord0 = input.uv;
    return output;
}
```

所有角色主材质共用这个 VS。`invTransModel` 保证非均匀缩放下法线/切线仍正确。

### 7.21.3 ArcToon_FS（BattleMage 卡通材质）

3 纹理：albedo(base) + sss + ilm。核心逻辑：

```hlsl
// 1. 硬色阶漫反射（动漫风）：N·L 在 0.49~0.50 之间做阶梯
float diffuse = bxdfContext.nol * input.color.x;
diffuse = smoothstep(0.49, 0.50, diffuse);

// 2. 高光：GGX，用 ilm.z 控制（ilm 贴图 R/G/B 通道分别控制漫反射/高光/色阶）
float3 ggx = SpecularGGX(bxdfContext, roughness, specularColor);
ggx = smoothstep(0.04, 0.05, ggx * ilm.z);  // 高光也硬色阶
ggx += envLighting.specular;

// 3. 合成：sss 色 × 漫反射 + 高光 + 环境漫反射
float3 finalColor = lerp(sssColor, baseColor, diffuse).xyz;
finalColor += ggx + envLighting.diffuse;
// 4. ilm 通道控制：w 通道乘整个色（内部线条），y 通道整体亮度
finalColor *= ilm.w;
finalColor *= dirLightColor.xyz * dirLightColor.w; // 光强
finalColor *= ilm.y;
```

> **ilm（illumination map）是这张材质的灵魂**：贴图里烘焙了固定的明暗/线条信息，shader 用 R/G/B/A 通道分别调制漫反射、高光、亮度、线条，让卡通角色有手绘感。

### 7.21.4 EndfieldBody_FS（Yvonne 皮肤）

```hlsl
// TBN：mesh tangent 有问题时用顶点色 x 翻转 bitangent；背面取反
float3x3 tbn = { worldTangent, color.x > 0.5 ? worldBitangent : -worldBitangent, worldNormal };
tbn = isFrontFace ? tbn : -tbn;
// 采样法线贴图 → 切线空间法线
float3 normal = mul(LoadNormalTextureFromXY(normalTex, samp, uv), tbn);

// 视衰减：法线向光分量随视角减弱（materialParams.w）
float viewFactor = lerp(1.0, max(vertNov, 0.0), materialParams.w);
radiance *= viewFactor;

// color ramp（色阶贴图）：radiance 高 → ramp 坐标靠左（更亮色）；rampOffset=0.2 控制饱和度
float2 rampCoord = float2(clamp((1-0.2) - radiance*(0.5-0.1), 0.1, 0.9), 0.5);
float4 rampColor = rampTex.Sample(samp, rampCoord);

// 合成 = baseColor × (radiance + rampColor×alpha×min(次表面MFP, baseColor))
float3 finalColor = baseColor.rgb * (radiance + (rampColor.rgb * rampColor.a) * min(fadedSubsurfaceMFP, baseColor.xyz));
// + 边缘光 × 强度参数
finalColor += EndfieldRimLighting(vertNov, vol, vertNol, 1.0, 1.0, materialParams.x) * materialParams.y;
finalColor *= dirLightColor.xyz * dirLightColor.w * 0.5;
```

> 皮肤是"半卡通"：既有 color ramp 的色阶感，又有法线贴图的细节和边缘光。

### 7.21.5 EndfieldFace_FS（脸——SDF 光照）

最讲究的脸部光照。核心是 **SDF（Signed Distance Field）贴图**：

```hlsl
// 1. 把光方向投影到"脸平面"（垂直于 objectUp），算右侧分量 rol
float3 projectedLightDir = ProjectDirection(dirLightDirection.xyz, objectUp.xyz);
float rol = dot(projectedLightDir, objectRight.xyz);
// 光在右半边时翻转 SDF 采样 U（SDF 贴图只存半张脸）
if (rol > 0) sdfCoord.x = 1.0 - sdfCoord.x;

// 2. 光角度：atan2(rol, -fol)；SDF 值减光角度 → 光照落点
float normalizedlightAngle = atan2(rol, -fol) / π;
float faceLightAngle = Unorm(((sdf.x + sdf.y) * 0.5) - abs(normalizedlightAngle));
float faceRadiance = smoothstep(0.5, 0.6, faceLightAngle); // 硬边缘（卡通脸光）
faceRadiance = lerp(faceRadiance, max(NdotL, 0), mask.y);  // mask 控制

// 3. 唇高光：高光 mask 纹理按 rol 偏移采样
float4 highlightMask = highlightMaskTex.Sample(samp, uv - rol*0.025);
float lipHighlihgt = highlightMask.r * faceRadiance;
```

> SDF 贴图存了脸部轮廓到光源的"距离场"——光从哪边来，脸的明暗交界线就画在哪边，形成柔和而精准的卡通脸部阴影。这是 Yvonne 脸看起来"会动"的关键。

### 7.21.6 EndfieldHair_FS（头发——各向异性高光）

```hlsl
// 双法线：hn 纹理 RG 存主法线，BA 存高光法线
float3 normal  = mul(ReconstructNormal(DecodeNormal(hn.xy)), tbn);
float3 normalH = mul(ReconstructNormal(DecodeNormal(hn.zw)), tbn);
// Gram-Schmidt 正交化切线
float3 tangent = normalize(tbn[0] - dot(tbn[0], normal) * normal);
// 根据 property.x 选主/次法线做 bitangent（发丝方向）
float3 bitangent = cross(property.x >= 0.5 ? normal : normalH, tangent);

// 高光 ramp：NdotL 绝对值 → hairRamp 贴图（发丝高光通常比皮肤亮得多）
float2 hairRampCoord = saturate(float2(abs(dot(normal, dirLight)), 0.5));
float4 hairRamp = hairRampTex.Sample(samp, hairRampCoord);

// 各向异性 PBR 光照（anisotropy=0.85，沿发丝方向拉长高光）
ShadingContext sc; sc.anisotropy = 0.85; ...
LightingResult r = DefaultShadingAnisotropic(sc, dirLight, dirLightColor, ambient, ambientSampler);

// 合成：diffuse + 高光×hairRamp×st.x + 透射 + 边缘光
finalColor = r.diffuse + smoothstep(0.1, 0.9, r.specular) * (hairRamp * property.w) * st.x + r.transmission;
```

### 7.21.7 EndfieldIris / Eyelash / EyeShadow / HairShadow（Yvonne 其余部位）

| 材质 | 逻辑 |
|---|---|
| Iris（眼睛） | 简化版：用 tangent-camera 偏移让虹膜跟随视线（irisDepth 参数）+ DefaultShading |
| Eyelash（睫毛） | 半透明：albedo + ramp，depthWrite=false（睫毛不遮挡） |
| EyeShadow（眼影） | Unlit：黑色，alpha = `1 - uv.y`（上眼睑实、下缘淡） |
| HairShadow（发影） | 半透明：发丝在脸上的投影，阴影色用 `outlineColor` 字段传（0.02,0.002,0,0.5） |

### 7.21.8 DefaultLit_FS（Ankha / Ball / BoxB——标准 PBR + 次表面）

```hlsl
// 法线贴图 TBN + Gram-Schmidt
float3 normal = mul(DecodeNormal(normalTex.Sample(samp, uv).xyz), tbn);
float3 tangent = normalize(tbn[0] - dot(tbn[0], normal) * normal);
float3 bitangent = cross(normal, tangent);
// PBR 参数从 materialParams2 来（metallic/specular/roughness/anisotropy）
ShadingContext sc;
sc.metallic = materialParams2.x; sc.specular = materialParams2.y;
sc.roughness = materialParams2.z; sc.anisotropy = materialParams2.w;
sc.subsurfaceColor = baseColor.xyz; sc.opacity = materialParams3.x;
// 次表面各向异性光照
LightingResult r = SubsurfaceShadingAnisotropic(sc, normalize(dirLightDirection.xyz), dirLightColor, ambient, ambientSampler);
finalColor = r.diffuse + r.specular + r.transmission;
```

### 7.21.9 描边（ArcOutline / EndfieldOutline）

两种描边都输出 `UnlitVSOutput`（PS 直接返回颜色，不光照），区别在**外扩权重**：

**ArcOutline_VS（BattleMage）**——双因子外扩：

```hlsl
// 顶点色 b 通道：距离因子（远离相机部分外扩少）；a 通道：悬挂因子（发梢等悬空处）
float3 faceFadedDir = (worldPosition.xyz - cameraPosition) * (1.0 - input.color.b);
float3 suspensionFadedDir = worldNormal * input.color.a;
worldPosition.xyz += outlineParams.x * (distanceFade * fovFade) * (faceFadedDir + suspensionFadedDir);
```

**EndfieldOutline_VS（Yvonne）**——stTex 权重：

```hlsl
float4 st = stTex.SampleLevel(samp, uv, 0);
worldPosition.xyz += outlineParams.x * distanceFade * fovFade * worldNormal * st.x;
```

- PS（EndfieldOutline_FS）：`return float4(outlineColor.xyz, 1.0)`（每材质描边色）。
- PS（ArcOutline_FS）：`return float4(color.xyz, ...)`（参考 ArcOutline_FS 用顶点色 w 反相透明度，本工程简化为固定黑）。

**渲染原理回顾**：PSO `CullMode=FRONT` 只画背面，顶点沿法线向外扩 → 背面外扩形成轮廓线；主体 pass 后画用深度覆盖正面。所以描边永远只在物体轮廓上出现。

### 7.21.10 ShellFurMask_VS（Ankha 身体 mask）

```hlsl
// 顶点色 x 作为毛发生长 mask；sizeExpand 沿法线外扩，depthOffset 向相机推
position += input.normal * input.color.x * materialParams3.z; // sizeExpand
float4 worldPosition = mul(model, float4(position, 1.0));
output.position = mul(viewProjection, worldPosition);
output.position.z -= materialParams3.w / output.position.w * input.color.x; // depthOffset
```

作用是让 Ankha 身体稍微"胀大 + 向相机推一点"，使 ShellFur 的毛壳能正确长在身体表面。

### 7.21.11 ArcCardShadow_FS（BattleMage 卡面阴影）

```hlsl
// 参考 ArcCardShadow_FS：RGB=材质颜色（黑），A = (1-顶点色w) × color.a
output.color = float4(outlineColor.xyz, (1.0 - input.color.w) * outlineColor.w);
```

Transparency 半透明黑卡，顶点色 w 做渐变淡出 → 角色脚下的柔和阴影。

## 7.22 地形与计算着色器

### 7.22.1 TerrainCommon.hlsl（地形常量 + 采样）

关键常量（严格对齐参考）：`terrainMeshInterval=4.0`、`terrainMeshSideElements=256`、`terrainDataSideElements=4096`、`terrainTexCoordScaling=0.05`、`waterBaseHeight=0`、`worldSideLength=2048`、`worldCenterOffset=-1024`。

- `LoadTerrain(data, worldPos)`：把世界坐标 → 数据纹理整数索引（`(worldPos - center)/interval`），`Load` 点采样（CS 用）。
- `SampleTerrain(data, sampler, worldPos)`：同坐标 → UV，`SampleLevel` 双线性（渲染用）。
- `CalculateTerrainNormal`：从高度相邻差计算法线（存 XY 分量）。
- `VariantTerrainSurface(color, surface, temperature)`：按 surface 通道变色——`g` 通道 → 沙滩（去饱和+色相偏移）、`b` 通道 → 湿度（色相反转）、温度低 → 雪（白）。

### 7.22.2 Terrain.hlsl（地形渲染 VS/FS）

**VS**：

```hlsl
// globalOffset：让网格随相机滑动（64×64 网格"无限延伸"效果，LOD 视差）
float2 globalOffset = floor(cameraPos.xy * (1/terrainMeshInterval)) * terrainMeshInterval;
float2 gridPos = input.pos.xy + globalOffset;
float gridHeight = LoadTerrain(TerrainHeightTex, gridPos).x; // 采样高度
o.worldPos = mul(model, float4(gridPos, gridHeight, 1.0));   // rotate -90X 转 y-up
o.pos = mul(viewProj, o.worldPos);
```

**FS**：6 纹理采样 + slope 混合 + PBR：

```hlsl
// 表面类型 + 法线（从 normal 纹理 XY 重建 Z）
float4 terrainSurface = SampleTerrain(TerrainSurfaceTex, sampler, wp);
float3 normal = ReconstructNormal(SampleTerrain(TerrainNormalTex, sampler, wp).xy);
// 草地质地（×4 提亮）+ 法线 + 属性
texColor = grassAlbedoTex.Sample(sampler, uv) * 4.0;
texNormal = LoadNormalTexture(grassNormalTex, sampler, uv);
// 温度 → 变色（雪/沙滩/湿度）
texColor.xyz = VariantTerrainSurface(texColor.xyz, terrainSurface, meteorograph.w);
// slope 混合：terrainSurface.r 权重混入坡地纹理（岩石），角校正法线混合
if (terrainSurface.r != 0) {
    texColor = lerp(texColor, float4(Desaturation(slopeAlbedo, 0.5), w), terrainSurface.r);
    texNormal = BlendAngleCorrectedNormals(texNormal, slopeNormal);
}
normal = BlendAngleCorrectedNormals(normal, mul(tbn, texNormal));
// PBR：DefaultShading（metallic 0.1, roughness 由 property.y 和 surface.a 取小）
```

### 7.22.3 地形数据生成 CS（TerrainHeight / TerrainNormal / TerrainSurface）

- **TerrainHeight_CS**：fBm + Perlin/Simplex/Worley 噪声生成 4096² 高度（照搬参考 TerrainHeightInitializer）。
- **TerrainNormal_CS**：从高度相邻差分算法线，存 `R8G8B8A8_SNORM`（XY 分量）。
- **TerrainSurface_CS**：从高度/法线算表面类型（slope/beach/humidity/snow），存 4 通道。
- **TerrainData_CS**（水文模拟，Once）：浅水方程 pipe 模型——每格从 4 邻居流出入水量，更新水位/沉积物/流速；加降雨（Pluviometer）、蒸发、热侵蚀。输出 height/normal/surface/waterFlow/sediment/velocity。
- **Meteorograph_CS**（每帧）：128² 风迭代 + 湿度传输 + 蒸发 + 降雨（写 Pluviometer）。
- **GenerateMip_CS**：双线性 blit 生成 mip（`srcTex.SampleLevel(uv, 0)`，SRV `MostDetailedMip` 指定源级）。

## 7.23 特效着色器

### 7.23.1 SkySphere.hlsl（天空盒）

```hlsl
// VS：世界空间球心 = 相机位置（无穷远天空效果）
float3 worldPos = mul(world, float4(input.pos, 1.0)).xyz + cameraPosition.xyz;
float3 dir = normalize(worldPos - cameraPosition.xyz);
// equirect 投影（行业标准，不依赖 fbx UV）：
// u = 0.5 + atan2(x, z)/2π（水平方位）
// v = 0.5 - asin(y)/π（天顶→地面）
// PS：直接采样 DefaultSky 贴图，无光照
```

### 7.23.2 Water_FS（水面）

逐段解读（本工程 y-up 适配）：

```hlsl
// 1. 泡沫形状：FractalSimplexNoise 双层扰动（随时间流动）
float fractalSimplex = lerp(A, B, abs(Snorm(frac(time))));       // A/B 双层
fractalSimplex = saturate((min(fractalSimplex, C - 0.075) - 0.25) * 6.0); // 阈值化出泡沫形状

// 2. 水法线：TerrainNormal 纹理 zw 存水法线；水流快时法线更强
float3 normal = ReconstructNormal(SampleTerrain(TerrainNormalTex, sampler, wp).zw);
normal = lerp(normal, normalize(float3(normal.xy * 2, normal.z)), waterSpeedFactor);

// 3. 折射：沿法线偏移 UV（深度近似 = min(1-z, 0.1)，参考 reverse depth 用 min(z,0.1)）
float2 refractedUVOffset = normal.xy * min(1.0 - input.position.z, 0.1) * 5.0;
// 采样场景深度 → 深度差（水面以下的物体深度）
float depthFade = sceneDepth - pixelDepth;
float2 refractedUV = input.position.xy * invScreenResolution + refractedUVOffset * depthFade;
float3 sceneColor = sceneColorTexture.Sample(sampler, refractedUV); // 折射看到的场景

// 4. 水色：深度越深越蓝（exp 衰减），沉积物越多越棕
baseColor = lerp(浅青, 深蓝, clamp(1 - exp(-depthFade * 0.02), 0, 1));
baseColor = lerp(baseColor, 棕, saturate(waterSediment * 100));

// 5. 光照：DefaultShading（正面金属性高一点、背面更透）
// 6. 深度衰减：水面附近透（lerp 到折射色），远处不透明
float attenuation = isFrontFace ? clamp(1 - exp(-depthFade*0.2), 0, 1) : ...;
float3 outputColor = lerp(sceneColor.xyz, finalColor, attenuation);
// 7. 泡沫：水流快 + 泡沫形状处变白
outputColor = lerp(outputColor, float3(0.7, 0.7, 0.75), waterSpeedFactor * fractalSimplex);
```

### 7.23.3 GrassInstancing_VS/FS（草地）

**VS（风摆动）**：

```hlsl
// 草尖摆动：每实例随机速度 + 正弦时间 + 位置噪声
float randomSpeedFactor = 2.0 * (1.0 + Hash(instancePosID, seed));
float2 tipFrequency = Square(1.0 - abs(sin(time + randomSpeedFactor - 1/worldPos.xy * 0.001 * wind)));
float2 amplitude = Unorm(sin(time*2.5 + tipFrequency*8 + worldPos.xy*0.5)) * 0.2 + tipFrequency*0.8 + 0.2;
// 高度越高摆得越多（pow(顶, grassStiffness=1.2)）；底部（color.x=0）不动
vertexOffset.xy = wind * amplitude * pow(max(input.pos.z, 0), 1.2);
vertexOffset.z = -length(vertexOffset.xy);   // 弯折时略微下沉
vertexOffset *= Square(input.color.x);
```

**FS（光照 + 伪遮挡）**：

```hlsl
// 基部深绿 → 尖部浅（PerlinNoise 扰动）
baseColor = lerp(深绿, 浅绿, saturate(PerlinNoise(wp) - 0.2));
// 法线：地形法线 + FractalNormal 细节法线角校正混合
// 草地纹理（Mip3 采样）× 2 + 表面类型变色
// 光照：DefaultShading（metallic 0, specular 0.1, roughness 0.75）
// 伪遮挡：远处草底部更暗（1 - Pow3(color.x)）
float occlusionFactor = min(input.position.z * grassVisibleDistance, 0.85);
color = lerp(color, baseColor.xyz * 0.2, (1 - Pow3(input.color.x)) * occlusionFactor);
color *= dirLightColor.xyz * dirLightColor.w;
```

### 7.23.4 ParticleExample（粒子）

- **ParticleExample_CS**：每帧模拟。位置 += 速度×dt；风场（Meteorograph 采样）+ 摩擦 + 重力；`elapseTime > maxLife` 或出 boundary → 重生到相机周围 ±10m。
- **ParticleExample_GS**（点精灵替代）：POINT → 4 顶点 quad；跳过 `P.w<=0`（相机后方）；`pointSize = min(尺寸/w, 上限)` 随深度反比。
- **ParticleExample_FS**：圆形 alpha（`1 - distance(center, pos)/size*2` + smoothstep），粒子色 25% 与场景折射色混合，`alpha = color.a * circleAlpha`（淡入淡出）。

### 7.23.5 Boid（鸟群）

- **BoidInstancing_CS**：O(N²) flocking——每只鸟对全群算分离/对齐/聚合，加地形回避 + 好奇心 + 最大速度限制。全 z-up 空间模拟。
- **BoidInstancing_VS**：`SV_InstanceID` 读 InstanceBuffer 的 model 矩阵变换纸飞机；位置 y 随地形高度。

### 7.23.6 ShellFur_VS/FS（AnkhaFur 毛）

- VS：`position += normal * (instanceID+1) * shellInterval`（每实例一层向外推的壳）。
- FS：FractalWorleyNoise 生长噪声，`max(1.5-noise, 0) < (instanceID+0.5)/count` 则 `clip(-1)`（裁剪掉毛之间的缝隙）；毛法线来自 FractalNormal 纹理 zw；光照 + Fresnel 边缘光。

## 7.24 后处理与色调映射

### 7.24.1 PostProcess.hlsl（后处理主链）

全屏三角形由 `SV_VertexID` 生成（无顶点缓冲）。PSMain 逐段：

```hlsl
// 1. 色差：G 通道按径向偏移 intensity、B 通道 2×（uv-0.5 方向）
color.xyz = SceneColorChromaticAberration(uv, color.xyz, 0.0025);

// 2. 线性深度 + 重建世界坐标（逆 viewProjection）
float sceneDepth = LinearDepth(depth);
float3 worldPos = ReconstructWorldPosition(uv, depth);

// 3. 环境色：DefaultSky 按"视线方向"采样 mip7（高度雾的颜色目标）
float3 envVector = normalize(worldPos - cameraPosition.xyz);
float phi = atan2(-envVector.z, envVector.x)*0.5/π + 0.5;   // y-up 适配
float theta = acos(clamp(envVector.y, -1, 1))*invPi;
float3 envColor = ambientTexture.SampleLevel(samp, float2(phi, theta), 7).xyz;

// 4. 高度雾：相机高度减世界高度（+深度淡出），指数衰减 → lerp 到 envColor
float heightFog = HeightFog(worldPos.y, sceneDepth, 0.25, 0.4);
color.xyz = lerp(color.xyz, envColor.rgb, heightFog);

// 5. 水下：UnderwaterMask（近平面 vs TerrainHeight 水面高度），越深越蓝
float underwaterMask = UnderwaterMask(uv);
color.xyz = lerp(color.xyz, 深蓝 + envColor*0.05, saturate(1-exp(-depth*0.05-1)) * underwaterMask);

// 6. Bloom + 光轴：加 bloom 色，SkylightShaft 沿光方向径向模糊 8 层（远处光柱）
color.xyz += bloomColor;
color.xyz += SkylightShaft(bloomColor, uv, sceneDepth, 1.0, 8);

// 7. 尘埃：Hash 噪声过滤（abs>0.4 丢弃）× 深度衰减 → 加
color.xyz += AtmosphericDust(uv, sceneDepth);
// 8. 暗角：乘 Vignette（离中心越远越暗）
color *= Vignette(uv, 0.75, 0.5);

// 9. ACES 色调映射（3D LUT）：log 编码 → 采样 LUT → 线性
color.xyz = LUTMapping(color.xyz);
// 10. 显式 sRGB 编码（swapchain 是 UNORM，参考靠 sRGB 自动）
color.xyz = LinearToSRGB(color.xyz);
```

调试键 5/6/7/8 在末尾直接返回噪声/场景原色/bloom/LUT 采样（诊断用）。

### 7.24.2 Bloom 三 shader（DownSampling / HorizontalBlur / VerticalBlur）

- **DownSampling**：2×2 邻域 4 点平均（1/2 分辨率）。
- **HorizontalBlur**：gaussianSigma3 15-tap 水平高斯，乘 `bloomIntensity`（每级不同）。
- **VerticalBlur**：垂直高斯 + `useCombinedTexture` 时叠加上一级 combined（金字塔向上合成）。

### 7.24.3 Chromatics（ACES 色调映射 + 3D LUT）

**为什么用 LUT**：ACES2 的 RRT+ODT 计算极贵，先在 compute shader 里**离线烘焙**成一个 32³ 3D 查找表，运行时 PostProcess 一次采样即可。

- **Chromatics_CS**：对 LUT 每个体素，把 `(r,g,b)` 做 `OutputTransformFoward`（ACES2 RRT + ODT），写入 `RWTexture3D`。
- **ACES 表生成**（C++ `ACES.cpp`）：`GenerateOutputTransformTables()` 照搬参考 ACESUtilities——生成 `OutputTransformParams`（rgbToCam16c 等矩阵）+ ReachM/UpperHullGamma/GamutCusps 三张表（各 362 个 float4），上传为 StructuredBuffer 给 CS 读。
- **LUTMapping（ChromaticsCommon.hlsl）**：

```hlsl
// 线性色 → log 色（HDR 高动态范围压进 [0,1]）
float3 LinearToLogColor(float3 linearColor) {
    return saturate(log2(linearColor)/14 - log2(0.18)/14 + 444/1023);
}
// 用 log 色坐标采样 32³ LUT（加半格偏移避免插值边界）
float3 LUTMapping(float3 srcColor) {
    return ColorLUTIn.Sample(Sampler, LinearToLogColor(srcColor) + 0.5/32).xyz;
}
```

> 数据结构大坑回顾：C++ 的 `OutputTransformParams` 矩阵字段用 `float[12]` raw 数组（不是 `float4x3`），否则 StructuredBuffer 布局歧义 → LUT 全黑（`ERRORS.md` #4）。ComputePass 的 SRV 表 offset 必须 = 0（`ERRORS.md` #3）。

---

## 8. 关键数据布局

### 8.1 共享描述符堆（sharedSrvHeap，25 个描述符）

```text
0  = offscreen(scene)        1  = combined[0](最终 bloom)
2  = depth                   3  = noise SRV
4  = ambient(DefaultSky IBL) 5  = TerrainHeight(R32G32)
6  = ColorLUT(3D LUT)        7-12  = down[0..5]
13-18 = horz[0..5]          19-23  = combined[1..5]
24 = noise UAV
```

> **顺序极其重要**：后创建的描述符若覆盖前一个 → 全红/崩溃（`ERRORS.md` #9）。改布局要全局核对所有 index。

### 8.2 顶点格式（60 字节）

| 语义 | 格式 | 偏移 |
|---|---|---|
| POSITION | R32G32B32_FLOAT | 0 |
| TEXCOORD | R32G32_FLOAT | 12 |
| NORMAL | R32G32B32_FLOAT | 20 |
| COLOR | R32G32B32A32_FLOAT | 32 |
| TANGENT | R32G32B32_FLOAT | 48 |

### 8.3 cbuffer 对齐规则（极易踩坑）

HLSL cbuffer 按 **16 字节对齐**：

| 类型 | 对齐 | 字节 |
|---|---|---|
| float | 4B | 4 |
| float2 | 8B | 8 |
| float3 | 16B | 16 |
| float4 / float4x4 | 16B | 16 |

**C++ 端写 struct 前必须先看 shader 的成员顺序和类型**，否则字段错位 → 垃圾矩阵/全错（`ERRORS.md` #13：Grass VS 曾把 viewProjection 写 offset 0，shader 在 offset 16）。

### 8.4 StructuredBuffer 布局

避免 `float4x3`/`glm::mat3x4`（布局歧义），用 `float[12]` raw 数组或 `alignas(16) float4[3]`（`ERRORS.md` #4：Chromatics LUT 全黑的根因）。

## 9. 坐标系约定

### 9.1 z-up → y-up

- **参考项目是 z-up**（Forward=(0,1,0)、Up=(0,0,1)、相机 `quatLookAt(dir, (0,0,1))`）；**本工程是 y-up**。
- 转换：所有模型/网格/场景经 `rotate(-90°X)`：`(x,y,z) → (x,z,-y)`。
- 角色位置（z-up → y-up）：BattleMage `(0,0,0)`、Yvonne `(0,0.1,-4)`、Ankha `(0,0.5,-6)`。

### 9.2 角色变换（T(P)*R*S）

```cpp
// 正确：先缩放 → 旋转（z-up 转 y-up）→ 平移到世界位置
const glm::mat4 base = glm::translate(mat4(1), pos)
                     * glm::rotate(mat4(1), radians(-90), X)
                     * glm::scale(mat4(1), vec3(scale));
```

> **坑**：`R*S*T(P)`（`glm::rotate(glm::scale(glm::translate(...)))`）会把平移向量也旋转——Yvonne 设 pos(0,0.1,-4) 实际变 (0,-4,-0.1) 埋地（`WORKLOG` 08-06）。

### 9.3 矩阵与深度

- glm 右手系 + HLSL 列主序，`mul(mvp, pos)`。
- 深度 `SV_POSITION.z ∈ [0,1]`，深度缓冲 D32_FLOAT，`glm::perspective` 输出兼容。
- 面序：`FrontCounterClockwise=TRUE`（CCW 为正面）。
- 相机前向取 view 矩阵**第三行取反**：`(-view[0][2], -view[1][2], -view[2][2])`（`ERRORS.md` 4.2）。

### 9.4 参考语义对照（z-up 渲染的 uniform 需转）

| 参考（z-up） | 本工程（y-up） |
|---|---|
| 地形高度 .z | .y |
| cameraVector 水平 .xy | .xz |
| TerrainHeight 采样（z-up 平面） | 保持 z-up 坐标系（草/粒子/水用） |
| 角色 transform | 渲染 model 含 rotate(-90X)，shader 内 worldPosition 是 y-up |

> 注意：有些计算（草生成、粒子、Boids 地形回避）**故意保持 z-up**，因为数据纹理（TerrainHeight）本来就是 z-up 坐标系；只有"渲染到屏幕"时才转 y-up。移植时先想清楚这段逻辑在哪个坐标系。

---

## 10. 渲染流程（逐帧）

```text
window.update()                    # 轮询 GLFW 事件
context.beginFrame()               # 检测 resize + 等上一帧 + 重置命令列表
├─ 相机输入（imgui 捕获鼠标/键盘时暂停）
├─ 快捷键（F 切换相机 / 1-8 后处理 / ~ 控制台）
├─ compute 模拟：
│    noiseUav dispatch → barrier → SRV（每帧）
│    meteorograph+pluviometer → UAV → Meteorograph_CS → 回 SRV（每帧）
│    （TerrainData 只在初始化跑一次）
├─ offscreen 转 RENDER_TARGET，清屏 + 清深度
├─ 地形渲染（先画并写深度，rotate -90X）
├─ meshRenderer.render(items)      # 天空盒 + 程序化几何
├─ characterRenderer.setLight(...) # 每帧同步光源（反射按钮可改）
├─ characterRenderer.render(charItems)   # 描边先画 + 主体，写深度
├─ boidsRenderer.render            # 群集 sim + 实例化绘制
├─ grassRenderer.render            # 实例生成 + ExecuteIndirect
├─ furRenderer.render              # AnkhaFur 壳层毛
├─ particleRenderer.render         # 粒子（内部先拷贝 sceneCopy 供折射）
├─ offscreen → COPY_SOURCE → water.copySceneColor → 回 RENDER_TARGET
├─ 深度转 DEPTH_READ（水 pass 只读深度）
├─ waterRenderer.render            # 水面
├─ offscreen → PIXEL_SHADER_RESOURCE
├─ Bloom 6 级金字塔（down → horz → combined）
├─ 后台缓冲 PRESENT→RENDER_TARGET
├─ PostProcess（后处理）→ 输出后台缓冲
├─ audio.update() + Gui（beginFrame → 面板构建 → render）
├─ 后台缓冲 RENDER_TARGET→PRESENT
│    depth 恢复 DEPTH_WRITE
context.endFrame()                 # ExecuteCommandLists + Present + Signal
```

### 10.1 为什么渲染顺序这样安排

- **地形先画**：写深度，后续物体/角色被地形正确遮挡。
- **天空盒画在 items[0]**：disable-depth 球必须在最前（后画会覆盖所有物体，`DepthFunc=ALWAYS` 永远通过）。
- **描边先画、主体后画**：`CullMode=FRONT` 背面外扩的描边被主体正面覆盖，形成干净的轮廓线。
- **半透明后画**：角色半透明材质（薄膜/睫毛/眼影/发影/卡面阴影）在所有不透明之后；水/粒子也在后段（混合需要）。
- **水在粒子后、后处理前**：水 pass 需要"当前场景色"做折射 → 先拷贝 offscreen 到 sceneCopy。

---

## 11. 场景内容

| 实体 | 说明 |
|---|---|
| SkySphere | 天空球（fbx 半径 50000 scale 0.01 = 半径 500），Unlit equirect（程序化 UV），`DepthFunc=ALWAYS` 不写深度，画在 items[0] |
| BattleMage | 4 mesh：mesh0 身体(ArcToon+描边) / mesh1 阴影卡(ArcCardShadow) / mesh2 武器(ArcToon W00) / mesh3 头部(ArcToon 共享身体纹理)，scale 0.01，位置 (0,0,0) |
| Yvonne | 12 mesh（Body/Neck/Cloth/Tail/ThinFilm/EyeShadow/Face/Eyelash/Iris/Hair/HairShadow），位置 (-4,0,5)，部分半透明 |
| Ankha | 11 mesh 共享 DefaultLit + AnkhaMask（ShellFurMask_VS），scale 0.02，位置 (4,0,5) |
| AnkhaFur | ShellFur 壳层毛（5 层实例） |
| Ball | Sphere.fbx + BallPBR（metallic 0.5/specular 0.5/roughness 0.4/anisotropy -0.5） |
| Box/BoxB | Box.fbx（BattleMage 子物体，W00 武器材质）+ BoxB（DefaultLit） |
| Boids | 1024 纸飞机群集 |
| Grass | InstancingGrass 实例化草地 |
| Particle | 4096 粒子 |
| 地形 | 4096² 高度图 + 水文 + 6 纹理 PBR |
| 水面 | 跟随相机的水网格 |

### 11.1 相机默认值（对齐参考）

- Orbit：target `(0,0.8,0)`（= BattleMage + (0,0,0.8) z-up → y-up）、distance 2.5、`yaw=0 pitch=-6°`（近平视略俯视）、fov 45°、near 0.1 far 1000。
- Free：position `(0,1,-2)`、yaw 90°、pitch -6°。
- 方向光：颜色 `0xFFEEDD00`、强度 π、方向 `normalize(-0.5,0.7,0.4)`。

---

## 12. 材质系统

### 12.1 参考格式

- **`.mat`（材质资产）**：json，含 shader 路径、`scalars`（metallic/roughness 等）、`vectors`、`textures`（albedoTex/normalTex 等）、`cullMode`、`domain`（Forward/Transparency）。
- **`.mati`（材质实例）**：json，`parentMaterialName` 继承 .mat，可覆写参数/纹理路径。

### 12.2 本工程的组织方式

本工程没有运行时材质系统，而是在 `Main.cpp` 里**对照参考 .mat/.mati 手工组织**：

- **纹理组**（`materialGroups`）：每组 5 槽 + ambient 槽，对应每材质的 t0-t4 + t5。
- **材质参数**：对照 .mati 的 scalars/vectors 填 `materialParams*`。
- **PSO 配置**：对照 .mat 的 cullMode / domain 填 `CharacterMaterialDesc`。

### 12.3 参数含义速查

| 参数 | 含义 |
|---|---|
| `materialParams` | Endfield 系：x=rimLightWidth y=rimLightIntensity z=subsurfaceMFP w=viewAttenuation |
| `materialParams2` | DefaultLit PBR：x=metallic y=specular z=roughness w=anisotropy |
| `materialParams3` | x=subsurfaceOpacity y=irisDepth z=sizeExpand(Ankha) w=depthOffset(Ankha) |
| `outlineColor` | 描边颜色 / HairShadow 阴影色 / CardShadow 颜色 |
| `outlineParams` | x=outlineWidth y=outlineDistanceFadeFactor |

### 12.4 参考示例对照

- `BattleMage.mati` → ArcToon：albedo `19A_Body_base.png` + sss + ilm。
- `BattleMageCardShadow.mati` → ArcCardShadow：color `(0,0,0,0.65)`，Transparency。
- `BattleMageWeapon.mati` → ArcToon + `19A_Body_W00` 纹理。
- `YvonneFace.mati` → EndfieldFace：rimLightWidth 1.0 / Intensity 0.5 / subsurfaceMFP 0.4 / viewAttenuation 1.0 + 5 纹理。
- `AnkhaBody.mati` → Ankha：metallic 0.2 / specular 0.5 / roughness 0.5 + sizeExpand 0.18 / depthOffset 0.0004。

---

## 13. 调试指南

### 13.1 调试按键

| 键 | 功能 |
|---|---|
| `F` | 相机模式切换 |
| `~` | ImGui 控制台 |
| `1/2/3` | 色差/尘埃/暗角开关 |
| `0` / `4` | 后处理全关/全开 |
| `5/6/7/8` | noise/场景原色/bloom/LUT 调试视图 |

### 13.2 fatal.log

程序 fatal error 写 `fatal.log`（**trunc 模式**，每次覆盖）。运行崩溃后看这个文件。

### 13.3 D3D12 调试层

Debug 构建启用验证层（`ID3D12Debug`），VS 输出窗口打印 D3D12 ERROR（如 `#708`、`#660`、`#921`）。**报错码是第一排错线索**，全部踩过的坑和对应错误码见 `ERRORS.md`。

### 13.4 dxc 预编译验证

改 shader 后先预编译，避免启动才报错：

```powershell
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe' -I Shaders -E PSMain -T ps_6_0 Shaders/xxx.hlsl -Fo NUL
```

### 13.5 readback 诊断

怀疑 compute 没执行/写错时，用 READBACK 缓冲读回 GPU 数据验证（粒子/草/LUT 都用过）。读 R16 纹理用 `uint16_t` + half→float 解析（`ERRORS.md` #7）。

### 13.6 隔离测试

固定输入看输出：例如粒子问题时把 GS 固定 `ndcHalf=0.05` + FS 纯红 → 屏幕出现分散红块即证明 GS 正常、问题在 FS。**比反复调参高效 10 倍**。

### 13.7 启动日志阅读

`[AfterglowRender DX12]` 前缀日志：shader 字节数、纹理尺寸、`[Model] mesh material:`（mesh 材质名）、`[scene] xxx world pos`（角色位置核对）。排查加载类问题重点看这些。

---

## 14. 常见问题 FAQ

| 问题 | 排查方向 |
|---|---|
| 编译 LNK2038 | glfw 静态库 /MT vs 项目 /MTd 不匹配，别改运行时库 |
| 启动报 dxcompiler.dll missing | 确认输出目录有 dxcompiler.dll（构建后自动复制） |
| 启动报 Failed to create graphics pipeline state | ① DXC 版本过旧（换 1.9.x）② cbuffer/描述符不匹配 |
| 画面全红/全黑 | ① 描述符堆 index 覆盖（#9）② SRV Shader4ComponentMapping 未设（#4）③ barrier 缺失（#6） |
| 画面拉伸大三角形 | ① cbuffer 布局错位（#13）② 资源状态错误 |
| 中文路径文件打不开 | 库的编码约定：miniaudio 用 wide API（#21） |
| GUI 报 ERROR #708 | imgui 绘制前必须 SetDescriptorHeaps + OMSetRenderTargets（#19） |
| 找不到模型/贴图 | 工作目录必须是 DX12/ 根 |
| 启动很慢 | Debug 加载大纹理（4K 草地）+ 模型（Yvonne 12 mesh）需要时间，Release 快很多 |

---

## 15. 项目路线图

```text
[✓] 阶段 1-2：基础框架 + 立方体绘制
[✓] 阶段 3-4：纹理 + 相机
[✓] 阶段 5：多 Mesh + 材质数据系统
[✓] 阶段 6-7：assimp 模型 + 多材质纹理
[✓] 阶段 8：法线 + 方向光 + Toon
[✓] 阶段 9：描边（材质级）
[✓] 阶段 10：后处理（Bloom/ACES/色差/尘埃/暗角/光轴/雾/水下）
[✓] 阶段 11：compute pipeline（TerrainData/Meteorograph/FractalNoise）
[✓] 场景复刻：角色材质、地形、草、鸟群、毛、粒子、水面、GUI、音频
[ ] 已知待办：
    - ParticleExample 平视时粒子屏幕中间集中（球体分布投影固有）
    - git commit 收尾（改动大量积累，需用户要求）
```

---

## 16. 附录

### 16.1 参考文件路径速查

| 内容 | 路径 |
|---|---|
| 场景搭建 | `AfterglowRender-main\vsbuild\AfterglowApplication.cpp` |
| pass 链 | `AfterglowRender-main\vsbuild\AfterglowPassManager.cpp` |
| 角色 shader | `AfterglowRender-main\vsbuild\Shaders\Toon\*.hlsl` |
| 标准 PBR | `AfterglowRender-main\vsbuild\Shaders\DefaultLit_FS.hlsl` / `DefaultLit_VS.hlsl` |
| 地形 | `AfterglowRender-main\vsbuild\Shaders\Terrain\*` |
| 后处理 | `AfterglowRender-main\vsbuild\Shaders\PostProcess_FS.hlsl` / `Chromatics\*` |
| 材质资产 | `AfterglowRender_v0.1_x64_release\Assets\...\*.mat*` |
| 开发清单 | `AfterglowRender-main\vsbuild\Documents\DevelopmentChecklist.md` |

### 16.2 相关文档

- `ERRORS.md` —— 全部踩坑记录（现象/根因/修复/教训 + 错误码速查）
- `WORKLOG.md` —— 开发工作记录（按日期）
- `AUDIT_DIFF.md` —— 与参考项目的差异审计报告




