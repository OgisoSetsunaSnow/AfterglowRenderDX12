// Terrain constants + utilities, ported 1:1 from reference project Terrain/TerrainCommon.hlsl.
#ifndef TERRAIN_COMMON_HLSL
#define TERRAIN_COMMON_HLSL

#include "ColorConversion.hlsl"
#include "WorldDefinitions.hlsl"

static const float terrainMeshInterval = 4.0; // 严格对齐参考项目（TerrainVertexInitializer 256, 4.0）
static const uint terrainMeshSideElements = 256;
static const uint terrainDataSideElements = 4096; // 对齐参考项目 TerrainData.mat numElements=16777216

static const float terrainTexCoordScaling = 0.05;

static const float waterMeshInterval = 4.0;
static const uint waterMeshSideElements = 256;
static const uint waterDataSideElements = 4096;

static const float waterBaseHeight = 0.0;
static const float waterSedimentCapability = 0.2;
static const float waterInvSedimentCapability = 1.0 / waterSedimentCapability;

static const float terrainDataInterval = worldSideLength / terrainDataSideElements;
static const float waterDataInterval = worldSideLength / waterDataSideElements;
static const float terrainDataCellArea = terrainDataInterval * terrainDataInterval;

static const uint terrainMeshIndexCount = ((terrainMeshSideElements - 1) * (terrainMeshSideElements - 1)) * 6;

static const float terrainInvMeshInterval = 1.0 / terrainMeshInterval;
static const float terrainInvDataInterval = 1.0 / terrainDataInterval;
static const float terrainInvDataSideElements = 1.0 / float(terrainDataSideElements);
static const float terrainInvMeshSideElements = 1.0 / float(terrainMeshSideElements);

// For external shader to read terrain data (float2 height / float4 data overloads, avoid template).
float2 LoadTerrain(Texture2D<float2> data, float2 worldPosition) {
	uint2 dataIndex = clamp((worldPosition - worldCenterOffset) * terrainInvDataInterval, 0, terrainDataSideElements - 1);
	return data.Load(uint3(dataIndex, 0));
}
float4 LoadTerrain(Texture2D data, float2 worldPosition) {
	uint2 dataIndex = clamp((worldPosition - worldCenterOffset) * terrainInvDataInterval, 0, terrainDataSideElements - 1);
	return data.Load(uint3(dataIndex, 0));
}
float SampleTerrain(Texture2D<float> data, SamplerState dataSampler, float2 worldPosition) {
	float2 uv = clamp((worldPosition - worldCenterOffset) * (terrainInvDataInterval * terrainInvDataSideElements), 0.0, 1.0);
	return data.SampleLevel(dataSampler, uv, 0);
}
float2 SampleTerrain(Texture2D<float2> data, SamplerState dataSampler, float2 worldPosition) {
	float2 uv = clamp((worldPosition - worldCenterOffset) * (terrainInvDataInterval * terrainInvDataSideElements), 0.0, 1.0);
	return data.SampleLevel(dataSampler, uv, 0);
}
float4 SampleTerrain(Texture2D data, SamplerState dataSampler, float2 worldPosition) {
	float2 uv = clamp((worldPosition - worldCenterOffset) * (terrainInvDataInterval * terrainInvDataSideElements), 0.0, 1.0);
	return data.SampleLevel(dataSampler, uv, 0);
}

// height: .x terrainHeight; .y waterHeight
float4 CalculateTerrainNormal(float2 height, float2 heightR, float2 heightT) {
	float4 adjacentHeight = float4(heightR.x, heightT.x, heightR.y, heightT.y);
	float4 deltaTerrainHeight = (float4(height.xx, height.yy) - adjacentHeight) * terrainInvDataInterval;
	return float4(
		normalize(float3(deltaTerrainHeight.xy, 1.0)).xy,
		normalize(float3(deltaTerrainHeight.zw, 1.0)).xy
	);
}

float3 VariantTerrainSurface(float3 srcColor, float4 terrainSurface, float temperature) {
	float3 outColor = 0.0;
	// Beach
	outColor = lerp(srcColor, Desaturation(HueShift(srcColor, 0.95), 0.5) * 4.0, terrainSurface.g);
	// Humidity
	outColor = lerp(outColor, Desaturation(HueShift(outColor, 0.95), -0.35) * 4.0, terrainSurface.b);
	// Snow
	outColor = lerp(outColor, float3(0.7, 0.7, 0.75), saturate(-temperature * 0.5));
	return outColor;
}

#endif
