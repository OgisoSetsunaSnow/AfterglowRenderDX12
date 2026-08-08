// 全屏后处理：照搬参考项目 PostProcess_FS.hlsl 完整效果链。
//   sceneColor → ChromaticAberration → HeightFog(envColor 从 ambientTexture 采样 mip7)
//   → Underwater(UnderwaterMask 采样 TerrainHeight) → Bloom + SkylightShaft
//   → AtmosphericDust → Vignette → Cursor → LUTMapping。
// 注：参考 z-up，本工程 y-up（envVector/height 用 .y）。
// LUTMapping：Chromatics 完整 ACES+3D LUT 管线为独立移植项（ACESTables_CS + Chromatics_CS 生成 32³ LUT），
// 当前保留占位 + Narkowicz ACES 临时 tonemap（本工程 swapchain 为 UNORM，需显式 tonemap；Chromatics 完成后替换）。
cbuffer PostParams : register(b0) {
	float4 cameraPosition;
	float4 envColor;             // 保留字段（不再用于 HeightFog，参考从 ambientTexture 采样）
	float4x4 view;
	float4x4 invViewProjection;
	float4 dirLightDirection;
	float4 params;               // nearZ, farZ, fogDensity, time
	float4 screenParams;         // invScreenWidth, invScreenHeight, cursorX, cursorY
	float2 aspectRatio;          // screenAspectRatio.x（宽高比）, .y
	uint effects;                // 调试：bit3 = noise 显示（参考效果无条件执行）
	uint _pad;
};

Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
Texture2D depthTexture : register(t2);
Texture2D noiseTexture : register(t3);     // compute 验证输出（调试键 5）
Texture2D ambientTexture : register(t4);   // IBL 环境贴图（equirect，envColor 采样）
Texture2D<float2> TerrainHeightTex : register(t5); // UnderwaterMask 水面高度
Texture3D ColorLUTIn : register(t6);       // Chromatics 32^3 3D LUT（ACES RRT+ODT 烘焙）
SamplerState sceneSampler : register(s0);
SamplerState ambientTextureSampler : register(s1);
SamplerState terrainSampler : register(s2);
SamplerState ColorLUTInSampler : register(s3);
#include "Chromatics/ChromaticsCommon.hlsl" // LUTMapping（3D LUT 采样）+ LinearToLogColor

static const float invPi = 0.31830988618f;
static const float randomSeed0 = 1312.21551f;

float Square(float x) { return x * x; }

float2 Snorm(float2 x) { return x * 2.0 - 1.0; }
float3 Snorm(float3 x) { return x * 2.0 - 1.0; }

float Grayscale(float3 color) {
	return color.r * 0.299 + color.g * 0.587 + color.b * 0.114;
}
float3 Desaturation(float3 color, float value) {
	return lerp(color, Grayscale(color), value);
}

float3 Hash3D(float3 xyz, float seed) {
	return frac(sin(float3(
		dot(xyz, float3(83.7247, 71.7823, 24.274)),
		dot(xyz, float3(64.4634, 49.4349, 82.263)),
		dot(xyz, float3(94.262, 20.9245, 34.8256))
	)) * 52567.0925 + seed);
}
float2 Hash2D(float2 xy, float seed) {
	return frac(sin(float2(
		dot(xy, float2(46.531, 91.4653)),
		dot(xy, float2(64.4634, 49.4349))
	)) * 13127.643 + seed);
}

// 重建 worldPos（参考 Depth.hlsl ReconstructWorldPosition，非 reverse depth：ZeroNearOneFarDepth=原值）。
float3 ReconstructWorldPosition(float2 uv, float depth) {
	float ndcX = uv.x * 2.0 - 1.0;
	float ndcY = 1.0 - uv.y * 2.0;
	float4 clip = float4(ndcX, ndcY, depth, 1.0);
	float4 world = mul(invViewProjection, clip);
	return world.xyz / world.w;
}

// 线性深度（标准 Z，逆向透视公式，与参考 Depth.hlsl LinearDepth 数学等价）。
float LinearDepth(float depth) {
	float ndcZ = depth * 2.0 - 1.0;
	return (2.0 * params.x * params.y) / (params.y + params.x - ndcZ * (params.y - params.x));
}

// HeightFog（参考 PostEffects.hlsl）：y-up 用 cameraPosition.y。
float HeightFog(float worldHeight, float sceneDepth, float distanceFade = 0.25, float intensity = 0.4) {
	float heightFog = Square(max(cameraPosition.y - worldHeight + sceneDepth * distanceFade, 0.0) * 0.01) * intensity;
	return saturate(1.0 - exp(-2.0 * heightFog));
}

// SampleTerrain(TerrainHeight)（参考 TerrainCommon.hlsl；PostProcess 不 include TerrainCommon 避免 Desaturation 重名，
// 手写等价：uv = clamp((worldPos - worldCenterOffset) * terrainInvDataInterval * terrainInvDataSideElements)。
// worldSideLength=2048, worldCenterOffset=-1024, terrainInvDataInterval*terrainInvDataSideElements=1/2048。
float2 SampleTerrainHeight(float2 worldPos) {
	float2 uv = clamp((worldPos + 1024.0) * (1.0 / 2048.0), 0.0, 1.0);
	return TerrainHeightTex.SampleLevel(terrainSampler, uv, 0);
}

// UnderwaterMask（参考 PostEffectUnderwater.hlsl）：近平面在水平面以下 → 水下。
// 参考 z-up：平面坐标 nearWorldPos.xy，高度 nearWorldPos.z。
// 本工程 y-up：平面坐标是 xz（且 z-up→y-up 后 z 反号 → float2(x, -z)），高度 nearWorldPos.y。
float UnderwaterMask(float2 uv, float softness = 0.01, float offset = 0.02) {
	float4 nearWorldPos = mul(invViewProjection, float4(Snorm(uv), 0.0, 1.0));
	nearWorldPos.xyz /= nearWorldPos.w;
	float2 nearPlaneTerrainHeight = SampleTerrainHeight(float2(nearWorldPos.x, -nearWorldPos.z));
	return smoothstep(0.0, softness, max(nearPlaneTerrainHeight.y - nearWorldPos.y + softness + offset, 0.0));
}

// Chromatic aberration（参考 PostEffects.hlsl 默认 intensity=0.0025）。
float3 SceneColorChromaticAberration(float2 uv, float3 sceneColor, float intensity = 0.0025) {
	float2 uvOffset = (uv - float2(0.5, 0.5)) * intensity;
	float sceneColorG = sceneTexture.Sample(sceneSampler, uv - uvOffset).g;
	float sceneColorB = sceneTexture.Sample(sceneSampler, uv - uvOffset * 2).b;
	return float3(sceneColor.r, sceneColorG, sceneColorB);
}

// Atmospheric dust（参考 PostEffects.hlsl：min(sceneDepth * 0.001, 0.1)）。
float3 AtmosphericDust(float2 uv, float sceneDepth, float intensity = 0.4) {
	float timeSeed = frac(floor(params.w * 20.0) * 0.05);
	float3 baseNoise = Snorm(Desaturation(Hash3D(float3(uv, timeSeed), randomSeed0), 0.5));
	baseNoise = any(abs(baseNoise) > intensity) ? 0 : baseNoise;
	return baseNoise * min((sceneDepth * 0.001), 0.1);
}

// Vignette（参考 PostEffects.hlsl）。
float Vignette(float2 uv, float intensity = 0.75, float hardness = 0.5) {
	hardness *= 0.5;
	return 1.0 - smoothstep(min(hardness, 0.5), max(1.0 - hardness, 0.501), saturate(distance(0.5, uv) * intensity));
}

// SkylightShaft（参考 PostEffects.hlsl；PostProcess_FS 调用传 intensity=1.0, layerCount=8）。
// 深度条件：参考 reverse depth 用 (SampleDepth < 0.0001)=远平面；本工程非 reverse depth 用 raw depth > 0.9 等价。
float3 SkylightShaft(float3 bloomColor, float2 uv, float sceneDepth, float intensity = 1.0, uint layerCount = 8) {
	const float invLayerCount = rcp(layerCount);
	const float layerInterval = 0.32 * invLayerCount;
	const float hashScale = 0.16 * invLayerCount;
	float3 radialBlurColor = 0.0;
	const float3 dirLightDirectionCS = mul(view, dirLightDirection).xyz;
	const float2 radialBlurOffset = float2(layerInterval, -layerInterval) * dirLightDirectionCS.xy * (1.0 - dirLightDirectionCS.z)
		+ Snorm(uv) * min(dirLightDirectionCS.z, 0.0) * 0.02;
	const float2 screenLightDirection = normalize(dirLightDirectionCS.xy + 1e-6);
	for (uint index = 1; index < layerCount + 1; ++index) {
		float2 radialBlurUV = uv + index * radialBlurOffset;
		const float2 radialHash = hashScale * Snorm(Hash2D(uv + index, frac(params.w) * randomSeed0)) * screenLightDirection;
		radialBlurUV += radialHash;
		radialBlurUV = clamp(radialBlurUV, screenParams.xy, 1.0 - screenParams.xy);
		const float isFar = step(0.9, depthTexture.Sample(sceneSampler, radialBlurUV).r);
		radialBlurColor += bloomColor * isFar;
	}
	radialBlurColor *= invLayerCount;
	return radialBlurColor * intensity * min(sceneDepth * 0.5, 1.0);
}

// Linear -> sRGB（参考靠 swapchain sRGB 自动 EOTF；本工程 swapchain 是 UNORM，需显式编码——API 差异必要修改）。
float3 LinearToSRGB(float3 c) {
	float3 low = c * 12.92;
	float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
	return lerp(high, low, step(c, float3(0.0031308, 0.0031308, 0.0031308)));
}

// 全屏三角形
struct VSOut {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
};

VSOut VSMain(uint vid : SV_VertexID) {
	VSOut o;
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
	float2 uniformUV = i.uv;
	uniformUV.x *= aspectRatio.x; // screenResolution.x / screenResolution.y

	float4 color = sceneTexture.Sample(sceneSampler, i.uv);

	// Chromatic aberration（参考：无条件执行）
	color.xyz = SceneColorChromaticAberration(i.uv, color.xyz);

	// Depth
	float depth = depthTexture.Sample(sceneSampler, i.uv).r;
	float sceneDepth = LinearDepth(depth);

	// Reconstruct world position（参考 PostProcess_FS）。
	float3 worldPos = ReconstructWorldPosition(i.uv, depth);

	// envColor：照搬参考 PostProcess_FS = ambientTexture.SampleLevel(equirect方向UV, mip7)。
	// 参考 GlobalAssets.cpp 确认 ambientTexture = DefaultSky.png（LDR，非 HDR）；
	// 之前"参考是 HDR equirect"判断错误导致回退固定色。
	// 参考（z-up）公式：phi=atan2(env.y,env.x)*0.5/π+0.5, theta=acos(env.z)*invPi。
	// 本工程 y-up：z-up 的 (x,y,z) → y-up (x,-z,y)，故 atan2(env.y,env.x)→atan2(-env.z,env.x)，acos(env.z)→acos(env.y)。
	float3 envVector = normalize(worldPos - cameraPosition.xyz);
	float normalizePhi = atan2(-envVector.z, envVector.x) * (0.5 * invPi) + 0.5;
	float normalizeTheta = acos(clamp(envVector.y, -1.0, 1.0) + 1e-2) * invPi;
	float3 envColor = ambientTexture.SampleLevel(ambientTextureSampler, float2(normalizePhi, normalizeTheta), 7).xyz;

	// Height Fog（参考：lerp 到 envColor，cbuffer 字段 float4 → .rgb float3）。
	float heightFog = HeightFog(worldPos.y, sceneDepth);
	color.xyz = lerp(color.xyz, envColor.rgb, heightFog);

	// Underwater（参考：UnderwaterMask + exp 衰减，y-up）。
	float underwaterMask = UnderwaterMask(i.uv);
	color.xyz = lerp(color.xyz, float3(0.02, 0.05, 0.12) + envColor.rgb * 0.05,
		saturate(1.0 - exp(-sceneDepth * 0.05 - 1.0)) * underwaterMask);

	// Bloom + Light Shaft（参考：无条件；SkylightShaft intensity 1.0, layerCount 8）。
	float3 bloomColor = bloomTexture.Sample(sceneSampler, i.uv).xyz;
	color.xyz += bloomColor;
	color.xyz += SkylightShaft(bloomColor, i.uv, sceneDepth, 1.0, 8);

	// Effect Blending（参考：无条件）。
	color.xyz += AtmosphericDust(i.uv, sceneDepth);
	color *= Vignette(i.uv);

	// Cursor test（参考 PostProcess_FS）。
	color += saturate(0.05 - distance(float2(aspectRatio.x, 1.0) * screenParams.zw * screenParams.xy, uniformUV));

	// LUTMapping：Chromatics 3D LUT 采样（ACES RRT+ODT 色调映射）→ sRGB 编码（swapchain UNORM）。
	color.xyz = LUTMapping(color.xyz);
	color.xyz = LinearToSRGB(color.xyz);

	// debug（直接输出 sceneTexture/bloomTexture 原始色，HDR clamp 到 [0,1]，足够诊断色相）。
	if ((effects & 16u) != 0u) { // KEY_6
		return float4(sceneTexture.Sample(sceneSampler, i.uv).rgb, 1.0);
	}
	if ((effects & 32u) != 0u) { // KEY_7
		return float4(bloomTexture.Sample(sceneSampler, i.uv).rgb, 1.0);
	}
	if ((effects & 64u) != 0u) { // KEY_8：显示 ColorLUT 固定采样（诊断 LUT 是否有内容）
		return float4(ColorLUTIn.Sample(ColorLUTInSampler, float3(0.25, 0.5, 0.75)).rgb, 1.0);
	}
	if ((effects & 8u) != 0u) { // KEY_5（noise 验证）
		return float4(noiseTexture.Sample(sceneSampler, i.uv).rgb, 1.0);
	}
	return float4(color.xyz, 1.0);
}
