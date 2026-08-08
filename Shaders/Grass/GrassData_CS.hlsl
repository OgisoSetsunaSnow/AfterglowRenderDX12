// GrassData 生成（一次性）：照搬参考项目 Grass/GrassData_CS.hlsl。
// 输出 2048x2048 R8G8B8A8_UNORM（textureMode 2）：
//   .r = Grass Terrain Possibility（坡度剔除）
//   .g = Grass Hash
//   .b = Grass Scaling（干旱度缩放）
//   .a = Unused
#include "GrassCommon.hlsl"
#include "../Random.hlsl"
#include "../TerrainCommon.hlsl"
#include "../MeteorographCommon.hlsl"

cbuffer GrassDataParams : register(b0) {
	float frameIndex;
	float3 _pad;
};

Texture2D<float2> TerrainHeight : register(t0);
RWTexture2D<float4> GrassDataOut : register(u0);

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	// .r: Grass Terrain Possibility
	// .g: Grass Hash
	// .b: Grass Scaling
	// .a: Unused

	static const float maxGrassScaling = 1.6;
	static const float minGrassScaling = 0.4;

	if (int(frameIndex) == 0) {
		// Terrain data
		float2 worldPos = WorldPositionFromTileID(threadID.xy, grassInvDataSideElements);
		float2 terrainHeight = LoadTerrain(TerrainHeight, worldPos);

		float grassPossibility = 0.0;
		if (terrainHeight.x > terrainHeight.y + 1e-1) {
			float2 terrainHeightT = LoadTerrain(TerrainHeight, worldPos + float2(terrainDataInterval, 0.0));
			float2 terrainHeightR = LoadTerrain(TerrainHeight, worldPos + float2(0.0, terrainDataInterval));
			float4 terrainNormal = CalculateTerrainNormal(terrainHeight, terrainHeightR, terrainHeightT);

			float slopeFactor = min(length(terrainNormal.xy) * 2.0, 1.0);
			grassPossibility = 1.0 - slopeFactor;
		}

		float grassHash = Hash(float(threadID.x * grassInvDataSideElements + threadID.y * (grassInvDataSideElements * grassInvDataSideElements)), randomSeed0) * 0.5;

		float aridityFactor = clamp((terrainHeight.x - terrainHeight.y) / maxWaterSoilDiffuseDistance, 0.0, 1.0);
		float grassScaling = lerp(maxGrassScaling, minGrassScaling, aridityFactor);

		GrassDataOut[threadID.xy] = float4(grassPossibility, grassHash, grassScaling, 0.0);
	} else {
		// 参考项目 GrassData 只有 Once（frameIndex==0 分支），每帧用旧值（此处不做拷贝——本工程只 dispatch 一次）。
	}
}
