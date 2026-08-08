// Terrain normal generation, ported 1:1 from reference Terrain/TerrainNormalInitializer_CS.hlsl.
// Read height texture -> write normal (RG terrain normal, BA water normal) as R16G16B16A16_FLOAT.
#include "TerrainCommon.hlsl"

Texture2D<float2> TerrainHeightIn : register(t0);
RWTexture2D<float4> TerrainNormalOut : register(u0);

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	const uint2 idxR = min(threadID.xy + uint2(1, 0), terrainDataSideElements - 1);
	float2 height = TerrainHeightIn.Load(uint3(threadID.xy, 0));
	float2 heightR = TerrainHeightIn.Load(uint3(idxR, 0));
	float2 heightT = TerrainHeightIn.Load(uint3(idxR, 0)); // reference uses R direction for T as well
	TerrainNormalOut[threadID.xy] = CalculateTerrainNormal(height, heightR, heightT);
}
