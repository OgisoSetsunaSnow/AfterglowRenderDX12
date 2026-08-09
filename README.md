# AfterglowRender DX12

基于 **DirectX 12** 的实时渲染器，将 Vulkan 版 [AfterglowRender](https://Celumina.github.io/AfterglowRender/) 移植到 D3D12。
支持 D3D12 基础框架、DXC 编译 HLSL、程序化网格、WIC 纹理、轨道/自由相机、assimp 模型加载与多材质纹理。

详细设计见 [DX12/TDD.md](DX12/TDD.md)。

## 目录结构

```
DX12/                            # 仓库根 = 工程根
├── AfterglowRenderDX12.sln
├── AfterglowRenderDX12.vcxproj
├── README.md
├── TDD.md
├── .gitignore
├── src/                         # C++ 源码（按功能分子目录）
│   ├── Main.cpp                 # 场景/渲染循环入口
│   ├── Core/                    # D3D12Context、Window、Mesh、Model、Texture、相机
│   ├── Renderers/               # Mesh/Character/Terrain/Water/Grass/Boids/Fur/Particle
│   ├── Passes/                  # OffscreenRT、PostProcess、FullScreenPass、ComputePass
│   ├── Utils/                   # ACES、FractalNoise、ShaderCompiler、GuiInspect（反射按钮）
│   ├── Audio/                   # AudioSystem（miniaudio 音乐播放）
│   └── GUI/                     # GuiRenderer（imgui 集成）
├── include/d3dx12.h             # CD3DX12 辅助头
├── Shaders/                     # HLSL shader（角色/地形/草/水面/后处理等）
├── Textures/checker.png         # 测试纹理
└── vsbuild/                     # 工程直接引用（路径写死在 vcxproj 中）
    ├── Libraries/               # 精简依赖库（仅构建所需）
    │   ├── glfw-3.4.bin.WIN64/  # 窗口/输入（静态库）
    │   ├── glm-1.0.1/           # 数学（header-only）
    │   ├── VulkanSDK-1.3.290.0/ # 仅 DXC：dxcapi.h / dxcompiler.lib / dxcompiler.dll
    │   ├── assimp-5.4.3/        # 模型加载（头文件随仓库；静态库见"第三方依赖库"）
    │   ├── openimageio-3.0.9.1/ # 仅 assimp 静态依赖链接库 + 运行时 DLL
    │   ├── imgui-1.92.3/        # ImGui + glfw/dx12 后端
    │   ├── Inreflect/           # 反射库（GUI 反射按钮，照搬参考项目）
    │   └── miniaudio/           # 单头文件音频库（音乐播放）
    └── Assets/                  # 运行资源（仅程序引用部分）
        ├── Characters/BattleMage/  # BattleMage.fbx + 贴图
        ├── Shared/Fonts/calibrib.ttf  # GUI 字体
        ├── Shared/Models/Box.fbx
        └── Music/               # 音乐播放列表（mp3/wav/flac/ogg/m4a）
```

## 环境要求

- Windows 10/11（x64）
- Visual Studio 2022 及以上（工程使用 v145 工具集；如本机为旧版 VS，请将
  `AfterglowRenderDX12.vcxproj` 中 `<PlatformToolset>v145</PlatformToolset>` 改为本机可用版本）
- Windows SDK 10.0.26100（如缺少，调整 vcxproj 中 `<WindowsTargetPlatformVersion>`）
- 除 assimp 静态库外全部依赖已包含在 `vsbuild/Libraries/`（见下节）

## 第三方依赖库

仓库已包含：GLFW（静态库）、GLM（头文件）、DXC 1.9.x（HLSL 编译器）、assimp 头文件、
zlib/minizip-ng/lzma/bz2（assimp 链接依赖）及全部运行时 DLL：

| 配置 | 运行时 DLL（构建时自动复制到输出目录） | 位置 |
|---|---|---|
| Debug | `dxcompiler.dll`、`zlibd1.dll` | `vsbuild/Libraries/openimageio-3.0.9.1/debug/bin/` |
| Release | `dxcompiler.dll`、`zlib1.dll`、`bz2.dll`、`liblzma.dll` | `vsbuild/Libraries/openimageio-3.0.9.1/bin/` |

**唯一需要自行下载的：assimp 静态库（约 540MB，不便随仓库分发）**

| 需要文件 | 放置路径 | 用途 |
|---|---|---|
| `assimp-vc143-mtd.lib` | `vsbuild/Libraries/assimp-5.4.3/lib/` | Debug 链接（/MTd） |
| `assimp-vc143-mt.lib`  | `vsbuild/Libraries/assimp-5.4.3/lib/` | Release 链接（/MT） |

获取方式（任选其一）：
1. 从 assimp 官方 GitHub Releases 下载 **5.4.3** 预编译包，把 `lib` 目录中 vc143 版的两个 `.lib` 复制到上表路径；
2. 或用 CMake 自行编译 assimp 5.4.3（MSVC，静态库 `/MTd` 与 `/MT` 各一份），放入上表路径。

> 如果缺这两个 `.lib`，编译会报 `LNK1104: cannot open file 'assimp-vc143-mtd.lib'`。
> assimp 链接时还需要的 zlib/minizip-ng/lzma/bz2 链接库与运行时 DLL 已随仓库提供，无需另装。

## 构建

用 **x64** 平台构建（工程仅配置 Debug|x64 / Release|x64）：

```
MSBuild.exe AfterglowRenderDX12.sln /p:Configuration=Debug /p:Platform=x64 /m
```

或直接用 Visual Studio 打开 `AfterglowRenderDX12.sln` 选择 `Debug` + `x64` 生成。

## 运行

1. 工作目录必须为仓库根 `DX12/`（程序以相对路径引用 `Shaders/`、`Textures/`、`vsbuild/Assets/`）。
2. 构建后 `x64/<Debug|Release>/AfterglowRenderDX12.exe` 可直接运行。
3. 运行时 DLL 已由 vcxproj 中的 **`CopyRuntimeDLLs` target**（`AfterTargets="Build"`）按配置
   自动复制到输出目录（Debug：dxcompiler+zlibd1；Release：dxcompiler+zlib1+bz2+liblzma），无需手动处理。

启动后窗口显示 6 个物体（立方体/球/三棱柱/Box.fbx/地面 + BattleMage 角色）：

- 鼠标左键拖拽旋转（轨道相机） / 右键拖拽转向（自由相机）
- 滚轮缩放，W/A/S/D 移动，E/Q 升降
- `F` 键切换轨道 / 自由相机

## 常见问题

| 现象 | 原因与解决 |
|---|---|
| 编译报 LNK2038 | glfw 静态库为 /MT 编译，项目 Debug 用 `/MTd`、Release 用 `/MT`，不要改动运行时库设置 |
| 启动报 "dxcompiler.dll missing" | 确认输出目录存在 `dxcompiler.dll`（构建后事件已自动复制；也可手动从 `vsbuild/Libraries/VulkanSDK-1.3.290.0/Bin/` 复制） |
| 启动报 "Failed to create graphics pipeline state" | 本仓库内的 DXC 为 1.9.x；若自行替换为旧版（如 1.3.290.0 附带的 1.8.x），其生成的 DXIL 在部分驱动上会 PSO 失败，请保持仓库内版本 |
| 启动报缺 `zlibd1.dll` | Debug 版 assimp 静态依赖，构建时自动复制；请确认用的是 Debug 配置 |
| 启动报缺 `zlib1.dll` / `bz2.dll` / `liblzma.dll` | Release 版 assimp 静态依赖，构建时自动复制；请确认用的是 Release 配置 |
| 找不到模型/贴图 | 运行工作目录必须是仓库根 `DX12/`（资源用相对路径） |
