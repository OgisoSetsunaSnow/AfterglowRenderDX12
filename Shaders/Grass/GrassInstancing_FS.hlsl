// Grass instanced fragment shader, ported 1:1 from reference Grass/GrassInstancing_FS.hlsl.
// z-up 语义：worldPosition.xy 是地形平面坐标（FractalNormal/TerrainNormal/Surface/Meteorograph 采样）。
#include "GrassCommon.hlsl"
#include "../Random.hlsl"
#include "../Constants.hlsl"
#include "../ColorConversion.hlsl"
#include "../TerrainCommon.hlsl"
#include "../MeteorographCommon.hlsl"
#include "../ShadingModels.hlsl"
#include "../Common.hlsl"

cbuffer GrassUniform : register(b0) {
	float time;
	float deltaTime;
	float2 _pad0;
	float4x4 viewProjection;
	float objectID;
	float3 _pad1;
};

cbuffer GrassGlobalUniform : register(b1) {
	float3 cameraPosition;
	float _gpad0;
	float3 dirLightDirection;
	float _gpad1;
	float4 dirLightColor;
	float3 cameraVector;
	float _gpad2;
};

// GrassInstancing.mat textures: grassAlbedoTex
Texture2D grassAlbedoTex : register(t0);
SamplerState grassAlbedoTexSampler : register(s0);
// 数据纹理
Texture2D FractalNormalTex : register(t1);
SamplerState FractalNormalSampler : register(s1);
Texture2D<float4> TerrainNormalTex : register(t2);
SamplerState TerrainNormalSampler : register(s2);
Texture2D<float4> TerrainSurfaceTex : register(t3);
SamplerState TerrainSurfaceSampler : register(s3);
Texture2D<float4> MeteorographTex : register(t4);
SamplerState MeteorographSampler : register(s4);
Texture2D ambientTexture : register(t5);
SamplerState ambientTextureSampler : register(s5);

struct VSOutput {
	float4 position : SV_POSITION;
	float3 worldPosition : POSITION;
	float3 worldNormal : NORMAL;
	float4 color : COLOR;
	uint objectID : OBJECT_ID;
};

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	float3 baseColor = lerp(float3(0.08, 0.1, 0.04), float3(0.2, 0.06, 0.02),
		saturate(PerlinNoise(input.worldPosition.xy, float2(0.2, 0.2), randomSeed0) - 0.2));
	baseColor = mad(baseColor, (input.color.y * 2.0), baseColor);
	baseColor = Desaturation(baseColor, -(input.color.y * 0.5));

	float4 fractalNormal = FractalNormalTex.Sample(FractalNormalSampler,
		float2(input.worldPosition.x + input.worldPosition.y, input.color.x) * 4.0);

	float3 normal = isFrontFace ? input.worldNormal : -input.worldNormal;
	float4 terrainNormal = SampleTerrain(TerrainNormalTex, TerrainNormalSampler, input.worldPosition.xy);
	normal = normalize(lerp(normal, ReconstructNormal(terrainNormal.xy), 0.75));
	normal = BlendAngleCorrectedNormals(normal, ReconstructNormal(fractalNormal.xy * 2.0));

	// Blend terrain
	float2 terrainTexCoord = input.worldPosition.xy * terrainTexCoordScaling;
	float4 texColor = grassAlbedoTex.SampleLevel(grassAlbedoTexSampler, terrainTexCoord, 3) * 2.0;
	float4 terrainSurface = SampleTerrain(TerrainSurfaceTex, TerrainSurfaceSampler, input.worldPosition.xy);
	float4 meteorograph = SampleMeteorograph(MeteorographTex, MeteorographSampler, input.worldPosition.xy);
	texColor.xyz = VariantTerrainSurface(texColor.xyz, terrainSurface, meteorograph.w);
	baseColor.xyz = lerp(baseColor.xyz, texColor.xyz, 0.85) * 0.75;

	// Lighting
	ShadingContext shadingContext = (ShadingContext)0;
	shadingContext.baseColor = baseColor.xyz;
	shadingContext.metallic = 0.0;
	shadingContext.specular = 0.1;
	shadingContext.roughness = 0.75;
	shadingContext.ambientOcclusion = 1.0;
	shadingContext.normal = normal;
	shadingContext.view = normalize(cameraPosition - input.worldPosition.xyz);

	LightingResult lightingResult = DefaultShading(shadingContext,
		normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, ambientTextureSampler);
	float3 color = max(lightingResult.diffuse, baseColor.xyz * 0.5) + lightingResult.specular + lightingResult.transmission;

	// Fake occlusion（远处草底部更暗）
	float occlusionFactor = min(input.position.z * grassVisibleDistance, 0.85);
	color = lerp(color, baseColor.xyz * 0.2, (1.0 - Pow3(input.color.x)) * occlusionFactor);

	color *= dirLightColor.xyz * dirLightColor.w;

	return float4(color, 1.0);
}
