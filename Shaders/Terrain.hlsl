// Terrain render shader, ported 1:1 from reference project Terrain/Terrain_VS.hlsl + Terrain_FS.hlsl.
#include "TerrainCommon.hlsl"
#include "Common.hlsl"
#include "ShadingModels.hlsl"
#include "MeteorographCommon.hlsl"

cbuffer TerrainCB : register(b0) {
	float4x4 viewProj;
	float4x4 model; // z-up grid -> scene y-up (rotate -90 X)
	float4 cameraPos;
	float4 dirLightDirection;
	float4 dirLightColor;
};

// 数据纹理（compute 生成）。
Texture2D<float2> TerrainHeightTex : register(t0);
Texture2D<float4> TerrainNormalTex : register(t1);
Texture2D<float4> TerrainSurfaceTex : register(t2);
Texture2D<float4> MeteorographTex : register(t3);
// 表面贴图（参考项目 TerrainMesh.mat 引用）。
Texture2D grassAlbedoTex : register(t4);
Texture2D grassNormalTex : register(t5);
Texture2D grassPropertyTex : register(t6);
Texture2D slopeAlbedoTex : register(t7);
Texture2D slopeNormalTex : register(t8);
Texture2D slopePropertyTex : register(t9);
Texture2D ambientTexture : register(t10); // IBL 环境贴图（DefaultSky equirect）

SamplerState terrainSampler : register(s0);
SamplerState ambientTextureSampler : register(s1);

struct VSInput {
	float3 pos : POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
	float4 color : COLOR;
};

struct VSOutput {
	float4 pos : SV_POSITION;
	float4 worldPos : POSITION;
};

VSOutput VSMain(VSInput input) {
	VSOutput o;
	// 参考 Terrain_VS: globalOffset 让网格随相机滑动（LOD 视差）。
	float2 globalOffset = floor(cameraPos.xy * (1 / terrainMeshInterval)) * terrainMeshInterval;
	float2 gridPos = input.pos.xy + globalOffset;
	float gridHeight = LoadTerrain(TerrainHeightTex, gridPos).x;
	o.worldPos = mul(model, float4(gridPos, gridHeight, 1.0));
	o.pos = mul(viewProj, o.worldPos);
	return o;
}

float4 PSMain(VSOutput input) : SV_TARGET {
	float2 wp = input.worldPos.xy;

	// 参考 Terrain_FS.
	float4 terrainSurface = SampleTerrain(TerrainSurfaceTex, terrainSampler, wp);
	float3 normal = ReconstructNormal(SampleTerrain(TerrainNormalTex, terrainSampler, wp).xy);

	float3x3 tbn = {
		cross(normal, float3(1.0, 0.0, 0.0)),
		float3(1.0, 0.0, 0.0),
		normal
	};

	float4 texColor = 0.0;
	float3 texNormal = float3(0.0, 0.0, 1.0);
	float4 texProperty = 0.0;

	float2 terrainTexCoord = wp * terrainTexCoordScaling;

	texColor = grassAlbedoTex.Sample(terrainSampler, terrainTexCoord) * 4.0;
	texNormal = LoadNormalTexture(grassNormalTex, terrainSampler, terrainTexCoord);
	texProperty = grassPropertyTex.Sample(terrainSampler, terrainTexCoord);

	float4 meteorograph = SampleMeteorograph(MeteorographTex, terrainSampler, wp);
	texColor.xyz = VariantTerrainSurface(texColor.xyz, terrainSurface, meteorograph.w);
	terrainSurface.r = lerp(terrainSurface.r, min(terrainSurface.r, 0.2), saturate(-meteorograph.w * 0.5));

	if (terrainSurface.r != 0.0) {
		float4 slopeTexColor = slopeAlbedoTex.Sample(terrainSampler, terrainTexCoord);
		float3 slopeTexNormal = LoadNormalTexture(slopeNormalTex, terrainSampler, terrainTexCoord);
		texColor = lerp(texColor, float4(Desaturation(slopeTexColor.xyz, 0.5), slopeTexColor.w), terrainSurface.r);
		texNormal = BlendAngleCorrectedNormals(texNormal, slopeTexNormal);
		texProperty = lerp(texProperty, slopePropertyTex.Sample(terrainSampler, terrainTexCoord), terrainSurface.r);
	}

	normal = BlendAngleCorrectedNormals(normal, mul(tbn, texNormal));

	float3 view = normalize(cameraPos.xyz - input.worldPos.xyz);

	ShadingContext shadingContext = (ShadingContext)0;
	shadingContext.baseColor = texColor.rgb;
	shadingContext.metallic = 0.1;
	shadingContext.specular = texProperty.z;
	shadingContext.roughness = min(texProperty.y, terrainSurface.a);
	shadingContext.ambientOcclusion = texProperty.x;
	shadingContext.normal = normal;
	shadingContext.view = view;

	LightingResult lightingResult = DefaultShading(shadingContext,
		normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, ambientTextureSampler);
	float3 finalColor = lightingResult.diffuse + lightingResult.specular + lightingResult.transmission;

	return float4(finalColor, 1.0);
}
