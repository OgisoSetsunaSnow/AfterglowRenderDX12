# AfterglowRender DX12 — 错误与踩坑记录（Errors Log）

> 版本：1.0（2026-08-09）
> 本文档汇总本项目开发过程中遇到的全部坑与错误：**现象 → 根因 → 修复 → 教训**。
> 排错优先看这里，再结合 `TDD.md` 第 13 章调试指南。
> 按主题分类：D3D12 API 类 / 描述符与资源类 / 着色器与数据类 / 渲染流程类 / 编码与工具类。

---

## 1. D3D12 API 类

### 1.1 PSO 零初始化危险（DepthFunc=NEVER / DSVFormat=UNKNOWN）
- **现象**：天空盒不显示、绘制失败。
- **根因**：`D3D12_GRAPHICS_PIPELINE_STATE_DESC{}` 零初始化后，`DepthFunc=0=D3D12_COMPARISON_FUNC_NEVER`（即使 `DepthEnable=FALSE` 某些驱动仍按 NEVER 拒绝），`DSVFormat=UNKNOWN` 与绑定的 D32_FLOAT 不匹配。
- **修复**：所有字段显式设置——天空盒用 `DepthFunc=ALWAYS + DepthWriteMask=ZERO`；DSVFormat 必须 = 实际 DSV 格式；RTVFormat 必须 = 实际 RT 格式。
- **教训**：PSO 描述结构体字段多，零初始化必踩坑；创建时逐字段核对。

### 1.2 每帧等待 GPU（无多帧在途）
- 本工程 `beginFrame` 用 fence 等上一帧完成（简单正确但慢）。不是 bug，是设计简化，后续可做 N 帧在途优化。

### 1.3 DXC 版本坑
- **现象**：`Failed to create graphics pipeline state`。
- **根因**：旧版 dxcompiler.dll（1.8.0.4650）编译的 DXIL 在部分驱动上 PSO 失败。
- **修复**：用新版 1.9.x DXC；发布包带 1.4.350.0 的 DXC。
- **教训**：DXC 版本影响 DXIL 兼容性，勿随意替换。

---

## 2. 描述符与资源类（最多坑的地方）

### 2.1 SetDescriptorHeaps 只能一个 CBV_SRV_UAV + 一个 SAMPLER
- **现象**：`D3D12 ERROR #554 SET_DESCRIPTOR_HEAP_INVALID`。
- **根因**：同时设置多个 CBV_SRV_UAV 堆。
- **修复**：所有 SRV/UAV 放**同一个共享堆**（`OffscreenRT::create` 接受 `sharedSrvHeap + srvIndex`），每个 RT 的 SRV 写不同 index。
- **教训**：D3D12 同一时刻只能一个 CBV_SRV_UAV 堆 + 一个 SAMPLER 堆。

### 2.2 描述符 range offset 两面性
- **现象**：ComputePass 的 ACES 表全部错位，LUT 全黑；或 PostProcess 的 t0-t3 全绑到 index 0。
- **根因**：
  - 多个 range **独立 root param（每表 1 描述符）** → offset 必须 = **0**（曾用 `offset=i` → t1 读 base+2、t2 读 base+4(UAV)、t3 越界）。
  - 多个 range **同一 root param（连续描述符）** → 用单 range `NumDescriptors=N` 或 offset 递增（曾 4 个独立 range 且 offset 全 0 → 全指向起点）。
- **修复**：独立表 offset=0；同一表内才递增。
- **教训**：**offset 语义取决于"是不是同一个表"**，写反必错。

### 2.3 StructuredBuffer 布局歧义
- **现象**：Chromatics LUT 全黑，HLSL 读到矩阵 = -65504。
- **根因**：C++ `alignas(16) glm::mat3x4` 与 HLSL `float4x3` 在 StructuredBuffer 中布局不一致（float4 数组在 HLSL StructuredBuffer 也有歧义）。
- **修复**：矩阵字段改 **`float[12]` raw 数组**（4B 对齐无歧义）或 `alignas(16) float4[3]` + 转置 helper。
- **教训**：StructuredBuffer 布局用 `float[12]` 或 `alignas(16) float4` 数组最安全，避免矩阵类型对齐歧义。

### 2.4 SRV desc 必须设 Shader4ComponentMapping
- **现象**：`D3D12 ERROR #27 CREATESHADERRESOURCEVIEW_INVALIDDESC` → DEVICE_REMOVAL。
- **根因**：SRV desc 零初始化 `Shader4ComponentMapping=0`（无效值）。
- **修复**：每个 `D3D12_SHADER_RESOURCE_VIEW_DESC` 显式设 `Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING`。
- **教训**：任何 SRV desc 都要设此字段（包括 3D LUT、深度纹理等非标准 SRV）。

### 2.5 root CBV 资源生命周期（ERROR #921）
- **现象**：`D3D12 ERROR #921 OBJECT_DELETED_WHILE_STILL_IN_USE` → DEVICE_HUNG/TDR。
- **根因**：root CBV（`SetGraphicsRootConstantBufferView`）只记录 GPU 虚拟地址、**不持有资源引用**；局部 ComPtr 出作用域释放，但命令列表还没 Close/Execute。同理也适用于局部 ComputePass（PSO 被释放）。
- **修复**：上传堆 CBV/PSO 的生命周期覆盖到命令列表 Close+Execute；初始化一次性资源提为函数级/成员变量，或等 GPU 完成再释放。
- **教训**：**任何被命令列表引用的对象都要活到 Execute 之后**。

### 2.6 每个被初始化 UAV 纹理要显式 barrier 回采样态
- **现象**：`D3D12 ERROR #527 RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH`。
- **根因**：compute 初始化写 UAV 后没 barrier 回采样态；下帧假设采样态→UAV，before 实际是 UAV。
- **修复**：init dispatch 后补 `UAV → kSampling` barrier（pluviometer 曾漏）。
- **教训**：compute 初始化后的状态必须与后续每帧假设一致。

### 2.7 READBACK 堆只支持 buffer
- **现象**：创建 readback 纹理失败 `E_INVALIDARG`。
- **根因**：READBACK 堆只能创建 Buffer。
- **修复**：读纹理用 `CopyTextureRegion`（SDK 26100 用 `D3D12_TEXTURE_COPY_LOCATION`），读 R16 数据用 `uint16_t` 解析（不要用 float*，字节错位）。
- **教训**：R16G16B16A16_FLOAT 每像素 8B，CPU 端用 `uint16_t` + half→float 手动转。

### 2.8 共享堆描述符 index 必须全程唯一
- **现象**：全红/崩溃。
- **根因**：后创建的描述符覆盖前一个（如 combined[5] SRV 被 noise UAV 覆盖）。
- **修复**：仔细规划共享堆布局（当前 25 个 index），init 顺序敏感，改布局全局核对。
- **教训**：描述符 index 是共享资源，必须全局唯一。

### 2.9 共享 CBV 分次渲染必须加 base offset
- **现象**：BattleMage 串到 Ankha 位 / Ankha 身体变黑。
- **根因**：`CharacterRenderer::render()` 单块 CBV、slot 0 复用——同帧第二次调用写 slot 0 污染第一次 drawMesh(0)。
- **修复**：`render()` 加 `cbvOffset` 参数，`writeMeshUniform/drawMesh` 用 `cbvOffset+i`；同帧多次调用传不同偏移。
- **教训**：共享 CBV 分次渲染必须错开 slot。

---

## 3. 着色器与数据类

### 3.1 cbuffer 布局必须与 shader 声明逐字段对齐
- **现象**：Grass 无草 + 拉伸大三角形；Boids 异常。
- **根因**：C++ `VsCB` 把 `viewProjection` 写 offset 0，shader 声明在 offset 16 → 垃圾矩阵。
- **修复**：按 shader 成员顺序写 C++ struct（float 4B / float2 8B / float3·float4·mat4 16B 对齐）。
- **教训**：C++ 写 struct 前先看 shader 成员顺序，不能想当然"矩阵写前面"。

### 3.2 VS 输出与 PS 输入结构必须完全一致
- **现象**：`D3D12 ERROR #660`（TEXCOORD 硬件寄存器不匹配）。
- **根因**：ArcOutline_VS 输出 `{position, texCoord0}`（TEXCOORD 位置 1），ArcOutline_FS 只声明 `float2 texCoord0 : TEXCOORD`（位置 0）→ 语义同名但寄存器号不同 → PSO 链接失败。
- **修复**：统一用公共结构 `UnlitVSOutput { position, texCoord0 }` / `UnlitFSInput { position, texCoord0, isFrontFace }`。
- **教训**：VS 输出与 PS 输入结构必须逐字段对齐；自定义 FSInput 易踩坑，用公共结构体。

### 3.3 HLSL 向量构造元素数必须匹配
- **现象**：`float4(frac(i.uv*10.0), frac(i.uv.yx*7.0), 0.5, 1.0)` 6 元素 → 编译失败。
- **修复**：逐元素构造。
- **教训**：向量构造元素数必须匹配，否则 dxc 编译失败。

### 3.4 照搬参考 shader 必须确认资源等价
- **现象**：整片画面粉红。
- **根因**：参考 PostProcess_FS `envColor = ambientTexture.SampleLevel(mip7)`，参考的 ambientTexture 是 HDR 天空；本工程 DefaultSky 是 LDR 暖色 PNG，mip7 采样粉红 → 高度雾把场景全染粉红。
- **修复**：先确认资源性质（HDR/LDR、equirect/cube）；后用 C++ 代码/资产定义核实（`GlobalAssets.cpp` 证实参考就是 DefaultSky LDR），恢复方向采样。
- **教训**：判断参考资源性质必须看代码/资产定义，不能凭截图臆测。

### 3.5 HLSL StructuredBuffer 布局 vs C++（见 2.3）

### 3.6 模板函数 / 标量广播
- HLSL 模板函数（参考 LoadTerrain template）DXC 支持；`float2(标量)` 非法，标量隐式广播（PerlinNoise scale 传标量即可）。

---

## 4. 渲染流程类

### 4.1 坐标语义必须对齐参考（cameraVector 方向）
- **现象**：草长在身后、平视不可见。
- **根因**：参考 `cameraVector = cameraPos - target`（指向相机），`-cameraVector.xy` 放草在镜头前方；本工程误传 `target-pos`（符号相反）。
- **修复**：Grass SimCB/GlobalCB 的 cameraVector 写 `-fwd_zup`（pos-target）。
- **教训**：移植全局 uniform 必须确认语义符号，不能凭直觉。

### 4.2 GLM view 矩阵取前向要第三行取反
- **现象**：相机看 ±X 时草被剔除。
- **根因**：`-view[2]` 是矩阵第三列 `(s.z,u.z,-f.z)`，仅看 +Z 时≈前向；绕 Y 后偏。
- **修复**：`glm::vec3(-view[0][2], -view[1][2], -view[2][2])`（第三行取反 = -f）。
- **教训**：GLM view 矩阵"相机前向" = `(-view[0][2],-view[1][2],-view[2][2])`。

### 4.3 视锥平面必须归一化
- **现象**：俯视时视锥边缘草被过度剔除。
- **根因**：Gribb-Hartmann 提取的平面未归一化，`dist < -radius` 比较失效（有效半径 = radius/|n|）。
- **修复**：`normPlane = p / |p.xyz|`。
- **教训**：球剔除比较依赖单位法线，必须归一化。

### 4.4 后处理顺序/结构
- 参考项目 postEffects 与 DebugView 顺序、Bloom 6 级金字塔、PostProcess 合成顺序都要照搬参考（见 TDD 7.16），否则效果偏离。
- 教训：渲染流程"结构差异"（如单级 bloom vs 6 级金字塔）比参数差异影响更大。

### 4.5 ComputePass 的 dispatchFrequency
- **现象**：地形每帧剧烈变化崩坏。
- **根因**：TerrainData.mat `dispatchFrequency:1=Once`（参考 `{Never=0, Once=1, PerFrame=2}`），本工程误每帧跑。
- **修复**：TerrainData_CS 移入初始化只跑一次；Meteorograph 每帧。
- **教训**：compute 的 dispatch 频率必须对齐参考 .mat 定义。

### 4.6 水面非 reverse depth
- 参考 reverse depth 用 `min(input.position.z, 0.1)`；本工程标准深度必须 `min(1.0 - input.position.z, 0.1)`，否则折射偏移近/远相反。
- 同理 y-up 高度用 `.y`、水平用 `.xz`、`SV_ISFRONTFACE` 只能 PS 输入。

### 4.7 折射需先拷贝场景色
- D3D12 禁止同一资源同 pass 既 RTV 又 SRV → 先把离屏拷贝到 `waterSceneCopy`/`sceneCopy` 再采样。

### 4.8 D3D12 无 Draw 只有 DrawInstanced / SV_PointSize 不可靠
- 点精灵用 GS 公告板（POINT → TriangleStream 4 顶点）替代；`kInitGroups` 不能引用类私有成员（C2737）→ 硬编码。

### 4.9 ExecuteIndirect 相关
- `CreateCommandSignature` 的 `DRAW_INDEXED` 参数类型不改 root 参数 → `pRootSignature` 传 nullptr（否则 #743）。
- GrassInstancing_CS 声明了 sampler 但 ComputePass 构造没传 → #882 CS_ROOT_SIGNATURE_MISMATCH → 构造要传 `numSamplerRanges`。

---

## 5. 编码与工具类

### 5.1 Windows min/max 宏冲突
- **现象**：`std::max` 报语法错误（C2589）。
- **根因**：windows.h 定义 `max` 宏，展开 `std::max(a,b)` 出错。
- **修复**：Main.cpp 顶部 `#define NOMINMAX`；GuiInspect.h include Inreflect.h 前 `#undef max/min`。
- **教训**：Windows 编程第一行先 NOMINMAX（或全程 `(std::min)` 括号）。

### 5.2 Windows 中文路径编码
- **现象**：歌名显示乱码 / 中文文件打不开。
- **根因**：
  - `std::filesystem::path::string()` 用系统 ANSI（GBK）→ 中文丢字符。
  - miniaudio `ma_sound_init_from_file` 用 `fopen_s`（ANSI 码页）→ UTF-8 中文路径打不开。
- **修复**：路径用 `path::wstring()` + `WideCharToMultiByte(CP_UTF8)` 转 UTF-8 显示；给 miniaudio 用 **wide API `ma_sound_init_from_file_w`**（`_wfopen_s`）。
- **教训**：给第三方库传路径前先确认其编码约定（fopen_s=ANSI、_wfopen=wide、miniaudio 走 UTF-8）。imgui 显示中文还需合并系统中文字体。

### 5.3 imgui 1.92.3 imgui_impl_dx12 集成
- `ImGui_ImplDX12_InitInfo` 没有 FontSrvCpuDescriptorHandle，用 `LegacySingleSrvCpuDescriptor/LegacySingleSrvGpuDescriptor`（1.91.5 迁移字段）。
- CommandQueue 字段必须填主 queue（font 上传用它执行+等待）。
- **RenderDrawData 前必须 SetDescriptorHeaps(imgui SRV 堆) + OMSetRenderTargets(后台缓冲)**，否则 `ERROR #708 SET_DESCRIPTOR_TABLE_INVALID`（imgui_impl_dx12 不自己做）。
- GuiRenderer.h 显式 include `<d3d12.h>`（后端头文件不包含）。

### 5.4 GLFW 回调覆盖
- GLFW 每事件类型仅一个回调。imgui_impl_glfw(install_callbacks=false) 手动接管 scroll/key/mouse/char 后覆盖引擎自身回调（如 Window::scrollCallback）→ 相机 zoom 改从 `GuiRenderer::consumeScrollOffset()` 读。

### 5.5 角色 mesh fallback 不能默认 Standard 白
- **现象**：BattleMage 头部红色眼睛/头发丢失。
- **根因**：BattleMage.fbx mesh3（GD_Hairbase/19A_Body.001）共享主 albedo（含红色眼睛/头发），用 Standard 白就白。
- **修复**：基于 fbx 材质名自动判断 mesh 用途（mesh1 固定 CardShadow、武器看 W00/VTX、其余身体/头部共享主纹理）。
- **教训**：角色头部/眼睛纹理往往在主 albedo 贴图里，fallback 别用纯白。

### 5.6 工具使用教训（流程性）
- 不要用 PowerShell Select-String 反复 grep 文档（易卡死/死循环）；read_file 一次定位 + replace_in_file。
- read_file 同一文件超过 5 次要停下来反思。
- 任务完成验证通过后不要主动扩大 scope。
- 长中文注释做 replace_in_file 易 JSON 超限 → 用短英文注释，改完 read 确认。
- Debug 构建模型加载很慢（Yvonne 12 mesh ~15s+），自动测试需 ≥18s 才到主循环；4K 草地 Debug 加载很慢，Release 快。

---

## 附录：错误码速查

| 错误码 | 含义 | 本工程对应 |
|---|---|---|
| #554 | 同时设多个同类描述符堆 | 2.1 |
| #660 | VS/PS 语义硬件寄存器不匹配 | 3.2 |
| #708 | 描述符表句柄不在当前绑定堆 | 5.3 |
| #921 | 对象在命令列表使用期间被删 | 2.5 |
| #527 | 资源屏障 before/after 状态不匹配 | 2.6 |
| #743 | CommandSignature 无效 | 4.9 |
| #882 | CS 根签名不匹配 | 4.9 |
| #27 | SRV 描述符无效（Shader4ComponentMapping） | 2.4 |
| C2589 | max 宏冲突 | 5.1 |
