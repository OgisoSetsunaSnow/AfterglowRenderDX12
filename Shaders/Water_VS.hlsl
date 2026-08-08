// Water mesh VS, ported 1:1 from reference project Terrain/Water_VS.hlsl.
// z-up grid (xy plane + z = water height) -> y-up via model rotate -90X (same as terrain).
#include "TerrainCommon.hlsl"
#include "Common.hlsl"

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

Texture2D<float2> TerrainHeightTex : register(t2);

struct VSInput {
	float3 pos : POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
	float4 color : COLOR;
};

struct VSOutput {
	float4 position : SV_POSITION;
	float4 worldPosition : POSITION;
};

VSOutput VSMain(VSInput input) {
	VSOutput output;

	// Follow camera in water-mesh-interval steps (reference Water_VS).
	float2 globalOffset = floor(cameraPosition.xy * (1.0 / waterMeshInterval)) * waterMeshInterval;
	float2 gridPos = input.pos.xy + globalOffset;

	// Water surface height = TerrainHeight .y (waterHeight).
	float waterHeight = LoadTerrain(TerrainHeightTex, gridPos).y;

	output.worldPosition = mul(model, float4(gridPos, waterHeight, 1.0));
	output.position = mul(viewProj, output.worldPosition);

	return output;
}
