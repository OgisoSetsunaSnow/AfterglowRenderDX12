// Meteorograph 每帧模拟：照搬参考项目 Meteorograph/Meteorograph_CS.hlsl。
// 参考项目用单 SSBO 同时读+写；D3D12 用 RWTexture2D 读+写（UAV Load 支持），避免 SRV+UAV 同资源状态冲突。
// 输入：TerrainHeight(t0, 读地形/水面高度) + Meteorograph(u0, 读当前状态)
// 输出：Meteorograph(u0, 写风/湿度/温度) + PluviometerOut(u1, 写降雨量)
#include "MeteorographCommon.hlsl"
#include "TerrainCommon.hlsl"
#include "Random.hlsl"

cbuffer MeteorographParams : register(b0) {
	float time;
	float deltaTime;
	float2 _pad;
};

Texture2D<float2> TerrainHeight : register(t0);
RWTexture2D<float4> Meteorograph : register(u0);   // RGBA32Float
RWTexture2D<float2> PluviometerOut : register(u1); // RG16Float

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	static const float humidityCapability = 1.0;
	static const float terrainSideRatio = float(terrainDataSideElements) / meteorographSideElements;
	static const float pressureWindMax = 0.005;
	static const float randomWindFieldIntensity = 0.2;
	static const float windSourceIntensity = 4.0;
	static const float diffuseBaseWeight = 0.01;
	static const float rainFadeoff = 0.5;

	float2 worldPos = WorldPositionFromTileID(threadID.xy, meteorographInvSideElements);

	// Fetch Terrain Data
	uint2 terrainID = threadID.xy * uint2(terrainSideRatio, terrainSideRatio);
	float2 terrainHeight = TerrainHeight.Load(uint3(terrainID, 0));

	// Fetch meteorograph (Cartesian Coords)
	float4 meteorograph = Meteorograph[threadID.xy];
	float4 meteorographT = Meteorograph[(threadID.xy + uint2(0, 1)) % meteorographSideElements];
	float4 meteorographB = Meteorograph[(threadID.xy + uint2(0, meteorographSideElements - 1)) % meteorographSideElements];
	float4 meteorographR = Meteorograph[(threadID.xy + uint2(1, 0)) % meteorographSideElements];
	float4 meteorographL = Meteorograph[(threadID.xy + uint2(meteorographSideElements - 1, 0)) % meteorographSideElements];

	// Wind iteration
	float deltaWindT = -meteorographT.y;
	float deltaWindB = meteorographB.y;
	float deltaWindR = -meteorographR.x;
	float deltaWindL = meteorographL.x;

	float4 deltaTemperature = float4(meteorographT.w, meteorographB.w, meteorographR.w, meteorographL.w) - meteorograph.w;
	float2 pressureWind = clamp(float2(deltaTemperature.z + deltaTemperature.w, deltaTemperature.x + deltaTemperature.y) * pressureWindMax, -pressureWindMax, pressureWindMax);

	float windSpeed = length(meteorograph.xy);

	// TODO: Include Navier-Stokes Equation to replace this.
	// WindSource: Random wind direction and intensity by time interpolation
	float3 windNoiseUV = float3(threadID.xy + time * float2(2.0, 0.0), time) * meteorographInvSideElements;
	float2 wind = lerp(meteorograph.xy, Snorm2(float2(
		PerlinNoise(windNoiseUV, 10.0, randomSeed0),
		PerlinNoise(windNoiseUV, 10.0, randomSeed1)
	)) * windSourceIntensity, deltaTime * randomWindFieldIntensity);

	wind += (float2(deltaWindR + deltaWindL, deltaWindT + deltaWindB) + pressureWind) * deltaTime;

	float2 humitureFlowT = -meteorographT.y * meteorographT.zw;
	float2 humitureFlowB = meteorographB.y * meteorographB.zw;
	float2 humitureFlowR = -meteorographR.x * meteorographR.zw;
	float2 humitureFlowL = meteorographL.x * meteorographL.zw;

	// Solved: Different action in low and high framerate -> texture precision.
	// Transport
	float2 humiture = max(meteorograph.zw + (humitureFlowT + humitureFlowB + humitureFlowR + humitureFlowL) * deltaTime, float2(0.0, absoluteZeroTemperature));
	// Diffuse
	float diffuseWeight = diffuseBaseWeight * deltaTime;
	float concentratedWeight = 1.0 - diffuseWeight * 4.0;
	humiture = humiture * concentratedWeight + (meteorographT.zw + meteorographB.zw + meteorographR.zw + meteorographL.zw) * diffuseWeight;

	// Evaporation and Rainfall
	float deltaWaterHeight = terrainHeight.y - terrainHeight.x;
	humiture.x += DeltaEvaporation(wind, humiture, SoilMoisture(deltaWaterHeight), 1.0, deltaTime);

	// Humidity saturation and rainfall
	float rainFall = max(0.0, humiture.x - humidityCapability) * rainFadeoff * deltaTime;
	humiture.x -= rainFall;

	// Temperature transport and recovery.
	humiture.y = lerp(humiture.y, SurfaceTemperature(worldPos.y, terrainHeight.x), deltaTime);
	// humiture.y = SurfaceTemperature(worldPos.y, terrainHeight.x);

	PluviometerOut[threadID.xy].x = rainFall;
	Meteorograph[threadID.xy] = float4(wind, humiture);
}
