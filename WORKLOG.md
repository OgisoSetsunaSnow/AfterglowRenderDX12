# AfterglowRender DX12 — 开发工作记录（Work Log）

> 版本：1.0（2026-08-09）
> 按日期整理从项目启动到当前的开发过程。技术细节与坑见 `TDD.md` / `ERRORS.md`。
> 项目目标：把 Vulkan 渲染器 AfterglowRender 移植到 D3D12，**严格照搬参考项目实现（除 API 差异外不改方案）**。

---

## 2026-08-03：发布准备与仓库重构

- 准备把工程上传 GitLab，编写 `prepare_release.ps1` 精简依赖与资源。
- **仓库根重构**：取消中间层 `DX12_GitLab/`，仓库根直接设为 `DX12/`；vcxproj 相对路径 `..\vsbuild\` → `vsbuild\`。
- `.gitignore` 完善（x64/、Afterglo.*/、*.obj/*.pdb 等）。
- **CopyRuntimeDLLs target**：构建后自动复制 `dxcompiler.dll` + `zlibd1.dll` 到输出目录（修复 clone 后缺 DLL 运行崩溃）。
- 发现 DXC 版本坑（1.8.x DXIL 在部分驱动 PSO 失败，改用 1.9.x）。

## 2026-08-04：渲染基础四大阶段

- **阶段 8：法线 + 方向光 + Toon**——MeshVertex 加 normal；三档 Toon 分段着色。
- **阶段 9：描边（材质级）**——大教训：曾全局描边走弯路（改 winding/two-sided 越改越乱），最终照参考 `ArcOutline.mat`：`CullMode=FRONT + CCW` 单 pass 背面外扩，仅角色启用。
- **阶段 10a：离屏渲染 + 后处理框架**——OffscreenRT(HDR) + PostProcess 全屏 pass；PSO RTVFormat 必须匹配离屏格式；全屏三角形 UV 需翻转 pos.y。
- **阶段 10b：Bloom**——DownSampling + HorizontalBlur + VerticalBlur；共享描述符堆（SetDescriptorHeaps 限制）。
- **阶段 10c：ACES tonemap**——确认参考项目 Chromatics ACES2 是未完成实现（编译不过）；先用 Narkowicz Fitted ACES（去色彩矩阵，避免输入色域错位墨绿）。
- **阶段 10d：depth 采样 + 距离雾**。
- **SkySphere 排查**：FBX 球半径 50000（不是 UV 问题）→ 参考 scale 0.01 + SkySphere_VS 加 cameraPosition → 天空正常。
- 崩溃事故教训：乱用 PowerShell grep + 反复 read_file 导致 IDE 卡死——**完成任务后不主动扩 scope，排错用工具要克制**。

## 2026-08-05：天空方向 + 角色导入 + 后处理增强

- **SkySphere 方向修复**：fbx UV 朝向不可靠 → 程序化 equirect UV（方向向量算 UV）。
- **Yvonne 导入**：12 mesh + 6 张 albedo 纹理，位置 (-4,0,5)。
- **描边三轮调试**：① 描边被天空覆盖（顺序错）② 角色全黑（CullMode 用错）③ 大面积描边（depth 配置错）——最终回归参考标准做法：`CullMode=FRONT` 先画 outline、main 后画、默认 depth。
- **描边调细 + 距离衰减**：distanceFade clamp 0.002；BattleMage 全 mesh 描边。
- **Ankha 导入**：11 mesh + 4 纹理，scale 0.02，位置 (4,0,5)。
- **10d-2 后处理增强**：色差/尘埃/暗角/光轴；加按键 1/2/3、0/4 对比开关；尘埃几经调参（最终回归参考方式）。
- 装饰模型（PaperAirplane/DebugBox）+ 纸飞机 CPU 环形轨迹动画。

## 2026-08-06：compute pipeline + 程序化地形

- **compute 基础设施**：ComputePass + OffscreenRT::createUav + CS 编译 + dispatch。
- **调通（关键）**：readback 全链路验证 CS 执行 + UAV 写入。真正根因是 **PostProcess 根签名描述符 range offset 全 0** → t0-t3 全绑 index 0（单 range NumDescriptors=4 修复）。
- **程序化地形阶段 1**：TerrainHeight_CS（高度生成）+ Terrain.hlsl + TerrainRenderer + 256×256 网格。
- 地形倒置修复（rotate -90X）。
- 角色位置对齐参考（T(P)*R*S 修复——原 R*S*T(P) 会连平移一起旋转，角色埋地）。
- **地形阶段 2**：Terrain_FS 完善（照搬 VariantTerrainSurface）。
- **真实资产路径修正**：参考资产在 `AfterglowRender_v0.1_x64_release\Assets\`（非 vsbuild）。
- **纹理质量大坑**：mip 链缺失（UpdateSubresources 参数错位全黑）→ sampler MaxLOD=0 锁死 mip0 → anisotropic 16 → **GPU mip 生成**（等价参考 vkCmdBlitImage）。
- **完整照搬地形**（用户批评"没按参考做"后）：Common/ColorConversion/BxDF/ShadingModels/TerrainCommon 等全套 shader 库。

## 2026-08-07：全面审计 + 角色材质 + 地形水文 + Chromatics

- **全面审计**：输出 `AUDIT_DIFF.md`（差异清单），用户要求严格按参考项目实现。
- **BattleMage 接入 CharacterRenderer**：ArcToon 材质（albedo+sss+ilm 三纹理）。
- 修复 5 个角色 shader 编译错误（角色 shader 之前从未真正运行过，dxc 预编译必须做）。
- **CharacterRenderer 重构**：每材质独立 VS/FS/PSO 配置；11 材质枚举；描边 PSO 修复 VS-PS 签名 #660（UnlitVSOutput 公共结构）。
- 灯光对齐参考：dirLight 0xFFEEDD00 + π 强度 + normalize(-0.5,0.7,0.4)。
- **Yvonne 12 mesh 切 CharacterRenderer**（Endfield 全套材质 + .mati 参数）；**Ankha 切 CharacterRenderer**（DefaultLit PBR 参数）。
- **Meteorograph 拆 Init/Sim/Pluviometer**（dispatchFrequency：Meteorograph 每帧、TerrainData Once）。
- **地形 4096 + TerrainData 水文模拟**（浅水方程/侵蚀/热侵蚀，Once）。
- **后处理补 HeightFog/Underwater**（简化版，envColor 先回退固定色）。
- **Bloom 6 级金字塔**（分级 intensity）。
- **Chromatics 完整 ACES + 3D LUT 移植**：C++ 表生成 + Chromatics_CS 烘焙 32³ LUT + LUTMapping。三个大坑：Shader4ComponentMapping（#27 设备移除）、readback 用 uint16 解析、**StructuredBuffer float4x3 布局歧义 → float[12]**、**ComputePass offset=i → 0**。
- TerrainData dispatchFrequency 修正（Once，否则地形每帧崩坏）。

## 2026-08-08：场景补全（多任务冲刺）

- **Chromatics 验证通过**（用户确认 ACES 色调）。
- **TerrainData/Meteorograph 平滑采样 + 格式对齐**：ComputePass sampler 支持 + SampleMeteorograph 模板双线性 + Meteorograph→RGBA32F、Pluviometer→RG16F。
- **水面 WaterMesh 完整实现**（折射/泡沫/深度淡出；非 reverse depth 用 min(1-z,0.1)；y-up 适配；SV_ISFRONTFACE PS 输入；折射先拷贝场景色）。用户确认按 main 完整版。
- **相机对齐**：FOV 45°、near 0.1、Orbit 默认、FreeCamera 默认位置；修复拉远"水下"（Orbit pitch -6° + Underwater 采样 y-up）。
- **envColor 恢复**：参考确用 DefaultSky LDR 方向采样（修正此前"HDR 天空"误判）。
- **FractalNoise 完整数据流**（Simplex+Worley+Normal）。
- **Ball**：Sphere + BallPBR。
- **Boids 群集**（1024 纸飞机 O(N²) sim + instancing）；修 #921（ComputePass 成员化）。
- **Box/BoxB**（BattleMage 子物体，W00 武器材质）。
- **Grass 实例化草地**：四个 bug 连修——① cbuffer 布局错位（拉伸大三角形）② FS b1 z-up 转换 ③ cameraVector 语义（pos-target）+ 相机前向第三行取反 ④ 视锥平面归一化。**用户确认"好了"**。
- **AnkhaFur（ShellFur）**：8 壳层毛（后调 5）；AnkhaMask（ShellFurMask_VS）按 debug 版实现；拆衣服方案几何不可行最终回退（毛覆盖衣服，用户接受）。
- **ParticleExample 粒子**：4096 粒子 compute + GS 公告板。多次调试：init 除零/随机重生/camDelta/pointSize clamp/regeneratedRange=10 等，最终**恢复到参考版**（球体分布 ±8m + 参考 FS），用户接受。
- **CharacterRenderer cbvOffset** 加固（共享 CBV 分次渲染不污染）。
- 交接文档整理（HANDOVER.md）。

## 2026-08-09：GUI、反射、音频、收尾

- **ImGui 集成**（参考 AfterglowGUI 浅色圆角风格）：imgui-1.92.3 + imgui_impl_glfw + imgui_impl_dx12；calibrib.ttf 字体；`~` 切换 Console 面板；修 ERROR #708（RenderDrawData 前 SetDescriptorHeaps + OMSetRenderTargets）。
- **Console 面板升级**：参考结构（NoTitleBar + 红色关闭按钮、输出区、RenderStatus/System/Scene/Audio 子面板、后处理 checkbox、相机按钮、实体列表、输入框）。
- **反射驱动按钮**：完整移植参考 Inreflect 反射库 + `GuiInspect.h`，`GuiControl` 注册 8 函数自动生成参数输入+Call 按钮；修 NOMINMAX 坑。
- **音频系统（新增）**：miniaudio 单头文件库 + `AudioSystem`（播放/暂停/切歌/音量/循环/自动切歌）；imgui Audio 面板；修中文歌名乱码（UTF-8 显示 + 中文字体合并）+ 中文路径播放（wide API `ma_sound_init_from_file_w`）。
- **BattleMage 材质分配对齐参考**：mesh1=ArcCardShadow（新增材质+shader）、mesh2=W00 武器纹理、mesh3 头部共享身体纹理（红色眼睛/头发）、仅 mesh0 描边；改为基于 fbx 材质名自动判断。
- **Ankha mask 参数按材质**（Body 0.18/0.0004、Dress 0.25/0.0002）。
- **收尾整理**：src 目录按功能分子目录（Core/Renderers/Passes/Utils/Audio/GUI）+ vcxproj 同步 + 垃圾清理（imgui.ini/fatal.log/嵌套 .git）+ README/TDD 更新。
- **文档**：重写 TDD.md（完整新手向）+ ERRORS.md + WORKLOG.md。

---

## 复刻完成度

对照参考项目 `AfterglowApplication.cpp` 全部实体：

- ✅ BattleMage / Yvonne / Ankha / AnkhaFur / Ball / Box+BoxB / Boids / Grass / ParticleExample / SkySphere / 地形 / 水面 / Meteorograph / TerrainData / FractalNoise / 后处理全套 / GUI / 音频
- ⏭️ 跳过（合理）：GlitchedMosaics / GreedySnake（参考 release 无资产）、InteractiveTest（开发者调试组件）、stencil（参考 [Skip]）
- ⏳ 待办：ParticleExample 平视粒子屏幕中间集中（固有）、git commit 收尾（需用户要求）
