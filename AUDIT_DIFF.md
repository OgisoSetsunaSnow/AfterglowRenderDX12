# AfterglowRender DX12 与参考项目差异清单（审计报告）

> 日期：2026-08-07
> 依据：参考项目源码 `C:\Users\32991\Desktop\AfterglowRender-main\vsbuild\`、运行版资产 `C:\Users\32991\Desktop\AfterglowRender_v0.1_x64_release\`、当前工程 `DX12\`。
> 原则：**严格按参考项目实现，除 API 差异（Vulkan→D3D12）外不改方案。**

---

## 0. 参考项目关键事实（已核实）

### 0.1 坐标系（z-up）
`AfterglowTransform.h` cardinal：Forward=(0,1,0)、Up=(0,0,1)、Left=(-1,0,0)。相机 `quatLookAt(dir, Up=(0,0,1))`。

### 0.2 渲染 pass 链（AfterglowPassManager.cpp）
```
Forward → Transparency → [自定义 BloomPassSet: DownSampling×6 → Bloom×6] → PostProcess → UserInterface
```
- Bloom 在 Transparency 之后、PostProcess 之前执行（`AfterglowPostProcessComponent::onRenderBegin` 安装）。
- Forward pass 离屏 RT 格式 `VK_FORMAT_B10G11R11_UFLOAT_PACK32`（LHDR）。

### 0.3 GlobalUniform（UniformBufferObjects.h，shader 全局可见）
view / projection / viewProjection / invView / invProjection / invViewProjection
dirLightDirection(vec4) / dirLightColor(vec4, w=intensity)
cameraPosition(vec3)+cameraNear / cameraVector(vec3)+cameraFar
screenResolution / invScreenResolution / cursorPosition / time / deltaTime
frustumPlaneL/R/B/T/N/F + cameraFov / screenAspectRatio / invScreenAspectRatio / frameIndex

### 0.4 MeshUniform（每 mesh）
model / invTransModel / minAABB / objectID / maxAABB / indexCount

### 0.5 顶点结构（VertexStructs.hlsl）
StandardFSInput：position(SV_POSITION) / worldPosition / objectID / worldNormal / worldTangent / worldBitangent / color / texCoord0 + `isFrontFace(SV_ISFRONTFACE)`。
模型导入需 tangent（BattleMage `addImportFlags(GenerateTangent)`）。

---

## 1. 角色材质 shader（最重大偏差）⭐

### 1.1 BattleMage
| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| 材质 | `BattleMage.mati` → **ArcToon.mat** | Cube.hlsl 三档 toon | ❌ |
| VS | `DefaultLit_VS.hlsl` | Cube.hlsl VSMain | ❌ |
| FS | `Toon/ArcToon_FS.hlsl` | Cube.hlsl PSMain | ❌ |
| 纹理 | **3 张**：albedoTex(`19A_Body_base.png`) + sssTex(`19A_Body_Sss.png`) + ilmTex(`Base/19A_Body_ilm.png`) | 2 张（base/decal）单纹理采样 | ❌ |
| 色阶 | `diffuse=smoothstep(0.49,0.50, nol*color.x)`（硬动漫色阶） | if/else 三档 0.85/0.45/0.35 | ❌ |
| 高光 | `SpecularGGX` + `EnvLighting`(IBL) | 无 | ❌ |
| ilm 控制 | `ggx*smoothstep(0.04,0.05,ggx*ilm.z)`、`final*=ilm.w`、`*ilm.y` | 无 | ❌ |
| 武器材质 | `BattleMageWeapon.mati`（W00 纹理+Twisted_N 法线） | 未实现 | ❌ |
| CardShadow | `BattleMageCardShadow.mati`（draw 1） | 未实现 | ❌ |

### 1.2 Yvonne（12 mesh 各用不同材质）
| mesh | 参考材质 | shader | 纹理 | 当前实现 | 状态 |
|---|---|---|---|---|---|
| 0 Body | YvonneBody.mati → EndfieldBody | EndfieldBody_FS | albedo+**normal**+**ramp** | Cube 单纹理 | ❌ |
| 1 Neck | YvonneNeck.mati → EndfieldBody | 同上 | 同上 | 同上 | ❌ |
| 2 Cloth | YvonneCloth.mati → **EndfieldPBR** | EndfieldPBR_FS | albedo+normal+**property**(+mask+ramp 薄膜) | 同上 | ❌ |
| 3 Tail | YvonneTail.mati | EndfieldPBR_FS | 同上 | 同上 | ❌ |
| 4 ThinFilm | YvonneThinFilm.mati | EndfieldPBR_FS | 薄膜干涉 | 同上 | ❌ |
| 5 Cloth | YvonneCloth.mati | EndfieldPBR_FS | 同上 | 同上 | ❌ |
| 6 EyeShadow | YvonneEyeShadow.mati | EndfieldEyeShadow_FS | 单 albedo | tint 近似 | ❌ |
| 7 Face | YvonneFace.mati → **EndfieldFace** | EndfieldFace_FS | albedo+**sdf**+ramp+**mask**+**highlightMask** | tint 近似 | ❌ |
| 8 Eyelash | YvonneEyelash.mati | EndfieldEyelash_FS/VS | 专用 | tint 近似 | ❌ |
| 9 Iris | YvonneIris.mati | EndfieldIris_FS/VS | 专用 | albedo | ❌ |
| 10 Hair | YvonneHair.mati → **EndfieldHair** | EndfieldHair_FS | albedo+**hn**+property+**st**+hairRamp | albedo | ❌ |
| 11 HairShadow | YvonneHairShadow.mati | EndfieldHairShadow_FS/VS | 专用 | tint 近似 | ❌ |

- EndfieldBody_FS 核心：法线贴图 TBN（`tbn = isFrontFace ? tbn : -tbn`）+ `EndfieldRimLighting` + subsurface(`subsurfaceMFP`×viewFactor) + **color ramp**(`rampOffset=0.2` 系数)。
- EndfieldFace_FS 核心：**SDF 光照**（sdfTex + objectUp/Right/Forward 投影光方向）、唇高光、mask 控制。
- EndfieldHair_FS 核心：双法线(hn.xy/zw) + **anisotropic GGX**(anisotropy=0.85) + hairRamp。
- 参数：rimLightWidth/Intensity、subsurfaceMFP、viewAttenuation 均来自 .mati（如 Body: 0.2/0.5/0.25/0.75；Face: 1.0/0.5/0.4/1.0；Cloth: 0.15）。

### 1.3 Ankha
| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| 材质 | Ankha.mat（ShellFurMask_VS + **DefaultLit_FS**）| Cube 单纹理 | ❌ |
| 纹理 | albedoTex + **normalTex**(DefaultNormal 或各部位法线) | 仅 albedo | ❌ |
| 参数 | metallic/specular/roughness（Body 0.2/0.5/0.5、Hair 0.75/0.5/0.4、Eye 0.5/0/1.0） | 无 | ❌ |
| 着色 | DefaultLit_FS：法线贴图 + SubsurfaceShadingAnisotropic | 三档 toon | ❌ |

### 1.4 必需前置
- **MeshVertex 增加 tangent（+bitangent 可推导）**；Model.cpp 加 `aiProcess_CalcTangentSpace`。
- **顶点输入布局加 TANGENT**；shader 输出 worldTangent/worldBitangent。
- **D3D12 需要每个材质独立 PSO/根签名**（当前 MeshRenderer 只有 1 个主 PSO + outline + sky）。

---

## 2. Meteorograph（照搬错 shader）⭐

| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| 每帧模拟 | `Meteorograph/Meteorograph_CS.hlsl`（风迭代/湿度传输/蒸发/降雨，dispatchFrequency=2）| ❌ 当前 `Shaders/Meteorograph_CS.hlsl` 内容 = **MeteorographInitializer_CS**（一次性初始化） | ❌ |
| 纹理 | Meteorograph(128² RGBA16F) + **Pluviometer**(128²) | 仅 Meteorograph | ❌ |
| 初始化 | MeteorographInitializer_CS + PluviometerInitializer_CS（initMode:5） | 无独立 init | ❌ |
| 外部输入 | TerrainHeight（externalSSBO） | 无 | ❌ |

**修复**：新建 `Shaders/MeteorographInitializer_CS.hlsl`（照搬 MeteorographInitializer）、`Shaders/MeteorographSim_CS.hlsl`（照搬 Meteorograph_CS 每帧模拟）、`Shaders/PluviometerInitializer_CS.hlsl`（照搬）；每帧 dispatch MeteorographSim + 写入 Pluviometer；TerrainData 读 Pluviometer 降雨。

---

## 3. 地形（分辨率 + 水文模拟）⭐

### 3.1 分辨率
| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| terrainDataSideElements | **4096**（TerrainCommon.hlsl；TerrainData.mat numElements=16777216） | **1024**（TerrainCommon.hlsl 硬编码） | ❌ |
| waterDataSideElements | 4096 | 1024 | ❌ |
| 高度纹理格式 | TerrainHeight RG16F(.x 地形 .y 水面) | RG16F 同 | ✅ |

### 3.2 水文模拟（TerrainData_CS，dispatchFrequency:1 = 每帧）
参考 `Terrain/TerrainData_CS.hlsl` 完整实现：降雨(Pluviometer×hydrologicalCycleRate) → 风扰动 → **浅水方程(pipe model)** → 侵蚀/沉积(WaterSediment/WaterVelocity) → **热侵蚀** → 蒸发 → 输出 TerrainHeight/TerrainNormal/TerrainSurface/WaterFlow。
当前实现只做了一次性 height/normal/surface 生成，**无 Water 数据、无每帧更新**。

### 3.3 Terrain_FS 对比（当前已大体照搬）
✅ 6 表面纹理采样、VariantTerrainSurface、slope 混合、DefaultShading —— 保留。
❌ 但 meteorograph.w 温度数据来自错误 shader（见 §2）。

---

## 4. 描边 shader（偏离参考）⭐

| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| BattleMage(ArcOutline_VS) | `faceFadedDir=(worldPos-camPos)*(1-color.b)` + `suspensionFadedDir=normal*color.a` 双因子外扩；clamp 0.01；FS 固定黑 | 仅 `normal` 外扩，clamp 0.002 | ❌ |
| Yvonne(EndfieldOutline_VS) | **stTex 采样 st.x** 做外扩权重；clamp 0.001；FS=outlineColor | 仅 worldNormal | ❌ |
| BattleMage outlineWidth | 10.0 | 5.0 | ❌ |
| Yvonne outlineWidth | 16.0（BodyOutline.mati） | 5.0 | ❌ |
| outlineDistanceFadeFactor | BattleMage 0.001 / Yvonne 0.0002 | 全局 0.001 | ⚠️ |

**修复**：Outline.hlsl 增加双 shader（ArcOutline 用 color.b/a；EndfieldOutline 用 stTex），或按材质选择。

---

## 5. 后处理（缺 HeightFog / Underwater / LUTMapping）⭐

参考 `PostProcess_FS.hlsl` 顺序：
```
sceneColor → SceneColorChromaticAberration(0.0025)
→ ReconstructWorldPosition → ambientTexture(DefaultSky) 采样
→ HeightFog(高度雾：cameraPosition.z - worldPos.z + depth*0.25, 强度0.4, exp 衰减)
→ UnderwaterMask(水下)
→ [bloomEnabled] bloomCombinedTexture + SkylightShaft(bloom, intensity 1.0, 8 层)
→ AtmosphericDust(0.4, depth*0.001 clamp 0.1)
→ Vignette(0.75, 0.5)
→ LUTMapping(Chromatics ACES 3D LUT, lutSideLength=32)
```

| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| 雾 | HeightFog（高度雾 lerp 到 envColor 环境色） | 距离雾 lerp fogColor | ❌ |
| ambientTexture 采样 | PostProcess 直接采样 DefaultSky 做环境色/雾 | 无 | ❌ |
| Underwater | PostEffectUnderwater.hlsl | 无 | ❌ |
| LUTMapping | Chromatics_CS 烘焙 32³ 3D LUT + ACESTables | Narkowicz Fitted ACES | ❌ |
| 色差强度 | 0.0025 | 0.01 | ⚠️ |
| 尘埃系数 | depth*0.001 clamp 0.1 | depth*0.03 clamp 0.2 | ⚠️ |
| SkylightShaft | intensity 1.0, 8 层 | 0.6, 8 层 | ⚠️ |

> 注意：MEMORY.md 曾记载"参考项目 Chromatics ACES 未完成"。**需复核**：Chromatics 目录含完整 9 个文件（ACESCommon/ACESDisplayEncoding/ACESOutputDevice/ACESOutputTransform/ACESTables_CS/ACESTonescale/Chromatics_CS/ChromaticsCommon）。PostProcess.mat computeTask 引用 Chromatics_CS + ACESTables 外部 SSBO。若可编译则照搬 3D LUT；不可编译再回退 Narkowicz（但强度/雾需按参考参数调）。

---

## 6. Bloom（结构差异）⭐

| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| 降采样 | **6 级** 1/2→1/64（DownSampling_FS 4 点 box） | 单级 1/2 | ❌ |
| 模糊 | 6 级 HorizontalBlur(gaussianSigma3 15-tap) + VerticalBlurCombination(合并上一级) | 单级 3 pass | ❌ |
| bloomIntensity 分级 | {0.175, 0.07, 0.06, 0.03, 0.03, 0.02}（每级不同） | 无 | ❌ |
| 参数 | resolutionScale/resolutionInvScale/useCombinedTexture/bloomIntensity | 无 | ❌ |

**修复**：实现 6 级金字塔（6 个 RT 链）。

---

## 7. 相机 / 灯光（参数偏差）

| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| FOV | 45°（AfterglowCameraComponent 默认） | orbit 60° / free 60° | ❌ |
| near/far | 0.1 / 1000 | 0.01 / 1000（free） | ⚠️ |
| 相机位置 | (0,2,1) z-up（MainCamera） | orbit target 原点 dist 7 | ❌ |
| 相机 target | BattleMage+(0,0,0.8) | 无 | ❌ |
| 方向光 Euler | globalEuler(45°,45°,30°) | lightDir normalize(0.4,1,0.3) | ❌ |
| 光强度 | intensity=π | w=1 | ❌ |
| 光颜色 | 0xFFEEDD00 → (0xFF,0xEE,0xDD)/255 ≈ (1.0,0.933,0.867) 暖白 | 白 (1,1,1) | ❌ |

---

## 8. 场景物体差异

| 物体 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| Box/BoxB | BattleMage 子节点（scale 40/20/20 @ (80,-20,50)，euler 0/15/45） | 无 | ❌ |
| Ball | Sphere.fbx @ (4,0,2) BallPBR.mati | 无 | ❌ |
| Boids | PaperAirplane.fbx scale 0.2 + **BoidInstancing compute 群集** | 纸飞机 CPU 圆周轨迹 | ⚠️ |
| GrassInstances | InstancingGrass.fbx + GrassData/GrassInstancing compute | 无 | ❌ |
| AnkhaFur | AnkhaFur.fbx + ShellFur(8 实例 instancing) | 无 | ❌ |
| GlitchedMosaics | DebugBox.fbx @ (4,4,1) compute | 无 | ❌ |
| FractalNoise/ParticleExample/GreedySnake | compute 组件 | 无 | ❌ |
| SkySphere | scale 0.01，DefaultSky.mati（**fbx UV**） | 程序化 equirect UV（此前为修复 fbx UV 方向问题） | ⚠️ |

---

## 9. 全局 uniform / 渲染框架差异

| 项 | 参考项目 | 当前实现 | 状态 |
|---|---|---|---|
| GlobalUniform | 完整（view/proj/inv×3 + frustum + time/deltaTime + cameraFov/aspect） | FrameCB 简化（无 invViewProj/frustum） | ⚠️ |
| MeshUniform | model/invTransModel/minAABB/objectID/maxAABB/indexCount | PerObjectCB（viewProj/world/invTransModel/baseColor/outline） | ⚠️ |
| 每帧 compute | compute 队列独立提交，先 dispatch 再 draw | 主循环只 dispatch 一次（地形初始化）+ noise 每帧 | ⚠️ |
| MSAA | Forward pass 有 MSAA color + resolve | 无 MSAA | ⚠️ |
| 渲染格式 | B10G11R11_UFLOAT | R16G16B16A16_FLOAT | ⚠️ 可接受 |

---

## 10. 修复优先级总表

| # | 项目 | 工作项 | 优先级 |
|---|---|---|---|
| P0 | 角色材质系统 | tangent 顶点 + ArcToon/EndfieldBody/EndfieldPBR/EndfieldFace/EndfieldHair/DefaultLit shader + 多纹理/多 PSO + 材质参数 | 最高 |
| P0 | Meteorograph | 补 Initializer/Sim(每帧)/Pluviometer，修正照搬错误 | 最高 |
| P0 | 地形 | 4096 分辨率 + TerrainData 每帧水文模拟（Water 4 纹理） | 高 |
| P1 | 描边 | ArcOutline 双因子 / EndfieldOutline stTex + width 对齐 | 高 |
| P1 | 后处理 | HeightFog + ambientTexture + Underwater + 参数对齐（LUT 待复核 Chromatics） | 高 |
| P1 | Bloom | 6 级金字塔 + 分级 bloomIntensity | 中 |
| P2 | 相机/灯光 | FOV 45 / 位置 (0,2,1) z-up 转换 / dirLight euler(45,45,30) π 暖白 | 中 |
| P2 | 场景 | Ball/Box/Boids compute/GlitchedMosaics 等 | 低（后续） |

---

## 附录：参考文件路径速查

| 内容 | 路径 |
|---|---|
| 场景搭建 | `AfterglowRender-main\vsbuild\AfterglowApplication.cpp` |
| pass 链 | `AfterglowRender-main\vsbuild\AfterglowPassManager.cpp` / `AfterglowCommandManager.cpp` |
| GlobalUniform | `AfterglowRender-main\vsbuild\UniformBufferObjects.h` |
| 角色 shader | `AfterglowRender-main\vsbuild\Shaders\Toon\*.hlsl`（ArcToon/EndfieldBody/PBR/Face/Hair/Iris/Eyelash/EyeShadow/HairShadow/Outline） |
| 标准 PBR | `AfterglowRender-main\vsbuild\Shaders\DefaultLit_FS.hlsl` / `DefaultLit_VS.hlsl` |
| 地形 | `AfterglowRender-main\vsbuild\Shaders\Terrain\*`（TerrainData_CS/Height/Normal/Surface/Water*/Mesh_CS） |
| Meteorograph | `AfterglowRender-main\vsbuild\Shaders\Meteorograph\*`（CS/Initializer/Pluviometer） |
| 后处理 | `AfterglowRender-main\vsbuild\Shaders\PostProcess_FS.hlsl` / `PostEffects.hlsl` / `Depth.hlsl` / `PostEffectUnderwater.hlsl` / `Chromatics\*` |
| Bloom | `AfterglowRender-main\vsbuild\Shaders\DownSampling_FS.hlsl` / `HorizontalBlur_FS.hlsl` / `VerticalBlurCombination_FS.hlsl` / `Kernels.hlsl`；`AfterglowBloomPassSet.cpp` |
| 材质资产 | `AfterglowRender_v0.1_x64_release\Assets\Characters\{BattleMage,Yvonne,Ankha}\Materials\*.mat*` + `Assets\Shared\Materials\*.mat` |
