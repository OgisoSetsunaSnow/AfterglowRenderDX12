// Water mesh FS, ported 1:1 from reference project Terrain/Water_FS.hlsl.
// D3D12 adaptation: explicit registers, cbuffer globals, non-reverse depth (linearized in shader),
// y-up (reference z-up): water height comparison uses .y.
#include "TerrainCommon.hlsl"
#include "Common.hlsl"
#include "Random.hlsl"
#include "ShadingModels.hlsl"

cbuffer WaterCB : register(b0) {
	float4x4 viewProj;
	float4x4 model; // z-up grid -> scene y-up (rotate -90 X)
	float4 cameraPosition;
	float4 cameraVector;   // camera view direction (global)
	float4 dirLightDirection;
	float4 dirLightColor;
	float2 screenResolution;
	float2 invScreenResolution;
	float nearZ;
	float farZ;
	float time;
	float _pad;
};

// Water_VS 输出 + 深度（t1）。与 VS 共享根签名：VS 用 t2 TerrainHeight。
Texture2D<float2> TerrainHeightTex : register(t2);
Texture2D<float4> TerrainNormalTex : register(t3);
Texture2D<float> WaterSedimentTex : register(t4);
Texture2D<float2> WaterVelocityTex : register(t5);
Texture2D<float> FractalSimplexNoiseTex : register(t6);
Texture2D sceneColorTexture : register(t0); // 离屏场景色（折射采样）
Texture2D depthTexture : register(t1);      // 场景深度（深度淡出）
Texture2D ambientTexture : register(t7);    // IBL 环境贴图（DefaultSky equirect，DefaultShading 用）
SamplerState waterSampler : register(s0);
SamplerState sceneColorTextureSampler : register(s1);

struct VSOutput {
	float4 position : SV_POSITION;
	float4 worldPosition : POSITION;
};

// 线性深度（标准 depth 0..1，非 reverse；与参考 Depth.hlsl 数学等价）。
float LinearDepth(float perspectiveDepth) {
	float ndcZ = perspectiveDepth * 2.0 - 1.0;
	return (2.0 * nearZ * farZ) / (farZ + nearZ - ndcZ * (farZ - nearZ));
}

// 深度采样（非 MSAA：直接 Load raw depth，再线性化）。
float SampleSceneDepth(int2 pixelCoord) {
	return LinearDepth(depthTexture.Load(int3(pixelCoord, 0)).r);
}
float SceneDepth(float perspectiveDepth) {
	return LinearDepth(perspectiveDepth);
}

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	static const float edgeFadeDistance = 0.4;

	float waterSediment = SampleTerrain(WaterSedimentTex, waterSampler, input.worldPosition.xy).x;
	float2 waterVelocity = SampleTerrain(WaterVelocityTex, waterSampler, input.worldPosition.xy);
	float waterSpeedFactor = saturate(length(waterVelocity) * 16.0);

	float3 viewDir = normalize(cameraPosition.xyz - input.worldPosition.xyz);

	// Velocity Noise Tex（照搬参考：FractalSimplexNoise 采样做泡沫形状）。
	float2 baseNoiseUV = input.worldPosition.xy * 0.05;
	float2 noiseVelocityOffset = clamp(waterVelocity, -0.1, 0.1) * 2.0;
	float noiseTimeScale = 0.5;
	float noiseTime = time * noiseTimeScale;
	float fractalSimplexA = FractalSimplexNoiseTex.Sample(waterSampler, baseNoiseUV - noiseVelocityOffset * frac(noiseTime));
	float fractalSimplexB = FractalSimplexNoiseTex.Sample(waterSampler, baseNoiseUV + 0.5 - noiseVelocityOffset * frac(noiseTime + 0.5));
	float fractalSimplex = lerp(fractalSimplexA, fractalSimplexB, abs(Snorm(frac(noiseTime))));
	float fractalSimplexC = FractalSimplexNoiseTex.Sample(waterSampler, baseNoiseUV * 0.5 + noiseTime * 0.025 + fractalSimplex * 0.5);
	fractalSimplex = saturate((min(fractalSimplex, fractalSimplexC - 0.075) - 0.25) * 6.0);

	// Water normal: Dither coord offset to reduce aliasing.
	float2 coordOffset = Snorm(Hash2D(ceil((input.position.xy + input.worldPosition.xy) * 128.0) * 0.001, randomSeed0)) * 0.35;
	float3 normal = ReconstructNormal(SampleTerrain(TerrainNormalTex, waterSampler, input.worldPosition.xy + coordOffset).zw);
	normal = lerp(normal, normalize(float3(normal.xy * 2.0, normal.z)), waterSpeedFactor);

	// Refraction offset（参考：normal.xy * min(z,0.1) * 5.0，z=SV_Position.z 屏幕深度近似。
	// 参考项目 REVERSE_DEPTH=true（近=1 远=0）；本工程标准深度（近=0 远=1），用 1-z 对齐）。
	float2 refractedUVOffset = normal.xy * min(1.0 - input.position.z, 0.1) * 5.0;

	float sceneDepth = SampleSceneDepth(int2(clamp(input.position.xy + refractedUVOffset * screenResolution, 0.0, screenResolution - 1.0)));
	float pixelDepth = SceneDepth(input.position.z);
	float depthFade = sceneDepth - pixelDepth;

	float2 refractedUV = input.position.xy * invScreenResolution;
	refractedUV += refractedUVOffset * depthFade;
	refractedUV = clamp(refractedUV, invScreenResolution, 1.0 - invScreenResolution);
	float3 sceneColor = sceneColorTexture.Sample(sceneColorTextureSampler, refractedUV);

	// Depth color
	float4 baseColor = lerp(
		float4(0.25, 0.4, 0.4, 1.0),
		float4(0.01, 0.1, 0.3, 1.0),
		clamp(1 - exp(-depthFade * 0.02), 0.0, 1.0)
	);

	// Sediment
	baseColor = lerp(
		baseColor,
		float4(0.4, 0.3, 0.2, 1.0),
		saturate(waterSediment * waterInvSedimentCapability * 100.0)
	);

	float matallic = lerp(0.15, 0.1, waterSpeedFactor);
	float specular = lerp(0.5, 0.1, waterSpeedFactor);
	float roughness = lerp(0.1, 0.3, waterSpeedFactor);

	ShadingContext shadingContext = (ShadingContext)0;
	shadingContext.baseColor = baseColor.xyz;
	shadingContext.metallic = isFrontFace ? matallic : 0.0;
	shadingContext.specular = isFrontFace ? specular : 0.01;
	shadingContext.roughness = roughness;
	shadingContext.ambientOcclusion = 0.5;
	shadingContext.normal = normal; // input.worldNormal;
	shadingContext.view = viewDir;

	LightingResult lightingResult = DefaultShading(shadingContext,
		normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, waterSampler);
	float3 finalColor = lightingResult.diffuse + lightingResult.specular + lightingResult.transmission;

	// Depth attenuation（参考 y-up 适配：高度差用 .y）。
	float deltaHeightToCamera = cameraPosition.y - input.worldPosition.y;
	float attenuation =
		isFrontFace
		? clamp(1.0 - exp(-depthFade * 0.2), 0.0, 1.0)
		: clamp(-deltaHeightToCamera * 0.01 + 0.5, 0.8, 1.0);

	attenuation -= 1.0 - clamp(depthFade, 0.0, edgeFadeDistance) * (1.0 / edgeFadeDistance);
	attenuation = max(attenuation, 0.0);
	// Camera fade for underwater（参考 z-up 用 .xy 水平；本工程 y-up 水平面是 xz）。
	float cameraHorizontality = pow(length(cameraVector.xz), 0.2);
	attenuation *= saturate(9.0 + 6.0 * log(length(input.worldPosition.xyz - cameraPosition.xyz)) * cameraHorizontality);

	float3 outputColor = lerp(sceneColor.xyz, finalColor, attenuation);

	// Velocity foam
	outputColor = lerp(outputColor, float3(0.7, 0.7, 0.75), waterSpeedFactor * fractalSimplex);

	return float4(outputColor, 1.0);
}
