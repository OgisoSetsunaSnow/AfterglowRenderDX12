// Terrain surface generation, ported from reference Terrain/TerrainData_CS.hlsl terrainSurface block.
// Output R8G8B8A8_UNORM: .r slope, .g beach, .b humidity, .a roughness.
#include "TerrainCommon.hlsl"
#include "MeteorographCommon.hlsl"

Texture2D<float2> TerrainHeightIn : register(t0);
Texture2D<float4> TerrainNormalIn : register(t1);
RWTexture2D<float4> TerrainSurfaceOut : register(u0);

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	const float2 terrainHeightIn = TerrainHeightIn.Load(uint3(threadID.xy, 0));
	const float terrainHeight = terrainHeightIn.x;
	const float waterHeight = terrainHeightIn.y;
	const float4 terrainNormal = TerrainNormalIn.Load(uint3(threadID.xy, 0));

	const float waterToTerrainHeight = terrainHeight - waterHeight;
	float4 terrainSurface = 0.0;
	terrainSurface.r = smoothstep(0.3, 0.5, length(terrainNormal.xy)); // Slope
	terrainSurface.g = max(1.0 - max(waterToTerrainHeight * 10.0, 0.0), 0.0); // Beach
	terrainSurface.b = max((waterToTerrainHeight - 0.5) * invMaxWaterSoilDiffuseDistance, 0.0); // Humidity
	terrainSurface.a = lerp(0.95, 0.2, -(1.0 / 0.4) * min(abs(waterToTerrainHeight + 0.2) - 0.4, 0.0)); // Roughness

	TerrainSurfaceOut[threadID.xy] = terrainSurface;
}
