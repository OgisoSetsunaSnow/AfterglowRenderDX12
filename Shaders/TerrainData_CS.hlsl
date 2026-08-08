// TerrainData 每帧水文模拟：照搬参考项目 Terrain/TerrainData_CS.hlsl（浅水方程 + 侵蚀 + 蒸发 + 热侵蚀）。
// 参考项目用单 SSBO 读+写；D3D12 用 RWTexture2D 读+写（Load/Store），避免 SRV+UAV 同资源状态冲突。
// 输入：TerrainHeight(u0 读+写) WaterFlow(u2 读+写) WaterSediment(u3 读+写) WaterVelocity(u4 读+写)
//       Meteorograph(t0 SRV 采样) Pluviometer(t1 SRV 采样)
// 输出：TerrainHeight(u0) TerrainNormal(u1) WaterFlow(u2) WaterSediment(u3) WaterVelocity(u4) TerrainSurface(u5)
#include "TerrainCommon.hlsl"
#include "Common.hlsl"
#include "MeteorographCommon.hlsl"

cbuffer TerrainDataParams : register(b0) {
	float time;
	float deltaTime;
	float2 _pad;
};

Texture2D<float4> Meteorograph : register(t0);
Texture2D<float2> Pluviometer : register(t1);

SamplerState MeteorographSampler : register(s0);
SamplerState PluviometerSampler : register(s1);

RWTexture2D<float2> TerrainHeight : register(u0);
RWTexture2D<float4> TerrainNormal : register(u1);
RWTexture2D<float4> WaterFlow : register(u2);
RWTexture2D<float> WaterSediment : register(u3);   // 参考 textureMode 6 = R32Float
RWTexture2D<float2> WaterVelocity : register(u4);  // 参考 textureMode 4 = RG16Float
RWTexture2D<float4> TerrainSurface : register(u5);

// 双线性 Load（参考 SampleTerrain，D3D12 RWTexture2D 无 Sample，用 Load 手写双线性）。
// 模板支持 float（WaterSediment R32）和 float4（通用）两种元素类型。
template<typename T>
T LoadTerrainBilinear(RWTexture2D<T> tex, float2 worldPos) {
	float2 uv = clamp((worldPos - worldCenterOffset) * (terrainInvDataInterval * terrainInvDataSideElements), 0.0, 1.0);
	float2 texelPos = uv * float(terrainDataSideElements) - 0.5;
	uint2 base = uint2(floor(texelPos));
	float2 f = frac(texelPos);
	uint2 c00 = clamp(base, 0, terrainDataSideElements - 1);
	uint2 c10 = clamp(base + uint2(1, 0), 0, terrainDataSideElements - 1);
	uint2 c01 = clamp(base + uint2(0, 1), 0, terrainDataSideElements - 1);
	uint2 c11 = clamp(base + uint2(1, 1), 0, terrainDataSideElements - 1);
	T s00 = tex[c00], s10 = tex[c10], s01 = tex[c01], s11 = tex[c11];
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	static const float meteorographSideRatio = float(meteorographSideElements) / terrainDataSideElements;

	// waterFlowDamping: To avoid water over oscillation.
	static const float waterFlowDamping = 0.005;
	static const float waterViscosity = 1.0;
	static const float soilDissolvingFactor = 0.4;
	static const float sedimentDepositingFactor = 0.025;
	static const float maxSedimentSlope = 0.85;
	static const float deltaTerrainDistance = 0.001;
	static const float hydrologicalCycleRate = 0.5;
	// Compensation factor from the meteorogragh resolution error.
	static const float evaporationFactor = 0.65;

	static const float thermalUnderwaterFactor = 0.1;
	static const float thermalWindIntensity = 0.1;
	static const float thermalHeatIntensity = 0.2;
	static const float thermalErosionRate = 0.2;

	static const uint windEffectWaveSize = 64;
	static const float windEffectIntensity = 0.65;

	static const float waterFlowPipeArea = terrainDataCellArea / waterViscosity;

	float2 worldPos = WorldPositionFromTileID(threadID.xy, terrainInvDataSideElements);

	// Fetch terrain data
	float2 terrainHeightIn = TerrainHeight[threadID.xy];
	float4 waterFlow = WaterFlow[threadID.xy];

	// Immediate variables for output.
	float terrainHeight = terrainHeightIn.x;
	float waterHeight = terrainHeightIn.y;

	// Fetch meteorograph data
	uint2 meteorographID = uint2(threadID.xy * meteorographSideRatio);
	// Sampling for smooth result.（照搬参考 SampleMeteorograph 双线性；D3D12 CS 需 sampler 表支持）
	float4 meteorograph = SampleMeteorograph(Meteorograph, MeteorographSampler, worldPos);
	float rainfall = SampleMeteorograph(Pluviometer, PluviometerSampler, worldPos);
	waterHeight += max(0.0, rainfall * hydrologicalCycleRate);

	// Fetch neighbours data (Cartesian Coords, clamp edges)
	uint2 idT = clamp(threadID.xy + uint2(0, 1), 0, terrainDataSideElements - 1);
	uint2 idB = clamp(threadID.xy + uint2(0, terrainDataSideElements - 1), 0, terrainDataSideElements - 1);
	uint2 idR = clamp(threadID.xy + uint2(1, 0), 0, terrainDataSideElements - 1);
	uint2 idL = clamp(threadID.xy + uint2(terrainDataSideElements - 1, 0), 0, terrainDataSideElements - 1);

	float2 terrainHeightT = TerrainHeight[idT];
	float2 terrainHeightB = TerrainHeight[idB];
	float2 terrainHeightR = TerrainHeight[idR];
	float2 terrainHeightL = TerrainHeight[idL];

	float4 neighbourSurfaceHeights = float4(
		max(terrainHeightT.x, terrainHeightT.y),
		max(terrainHeightB.x, terrainHeightB.y),
		max(terrainHeightR.x, terrainHeightR.y),
		max(terrainHeightL.x, terrainHeightL.y)
	);

	// High deltaTime yields unstable simulation result, so slow down the simulation if framerate lower than 60 per second.
	float waterDeltaTime = min(deltaTime, 1.0 / 60.0);

	float surfaceHeight = max(terrainHeight, waterHeight);
	float4 surfaceDeltaHeights = surfaceHeight - neighbourSurfaceHeights;

	// Wind affect to the water surface.
	if (all(threadID % windEffectWaveSize) == 0) {
		float4 maxDisturbance = max(neighbourSurfaceHeights - float4(terrainHeightT.x, terrainHeightB.x, terrainHeightR.x, terrainHeightL.x), 0.0) * waterDeltaTime;
		float2 resizedWind = ClampVectorWithLength(meteorograph.xy, 0.8, 64.0);
		float4 windEffect = float4(-resizedWind.x, resizedWind.x, -resizedWind.y, resizedWind.y);
		surfaceDeltaHeights -= clamp(windEffect * windEffectIntensity * max(length(surfaceDeltaHeights), 0.001), -maxDisturbance, maxDisturbance);
	}

	// Shallow water equation with pipe model.
	// Overflow (SWE) — WaterFlow: (m^3)/s
	waterFlow = max(0.0, waterFlow + waterDeltaTime * waterFlowPipeArea * ((worldGravity * surfaceDeltaHeights) / terrainDataInterval));
	float idealFlow = (waterFlow.x + waterFlow.y + waterFlow.z + waterFlow.w);
	// Limit the max waterFlow never greater than the total water.
	float flowScalingFactor = min(
		1.0 - waterFlowDamping, (max(waterHeight - terrainHeight, 0.0) * (terrainDataInterval * terrainDataInterval)) / (idealFlow * waterDeltaTime)
	);
	waterFlow *= flowScalingFactor;
	WaterFlow[threadID.xy] = waterFlow;

	// Waiting for WaterFlow write in.
	GroupMemoryBarrierWithGroupSync();

	// InFlow
	float4 waterFlowT = WaterFlow[idT];
	float4 waterFlowB = WaterFlow[idB];
	float4 waterFlowR = WaterFlow[idR];
	float4 waterFlowL = WaterFlow[idL];

	// Component Order: T, B, R, L.
	float inFlow = waterFlowT.y + waterFlowB.x + waterFlowR.w + waterFlowL.z;
	float outFlow = waterFlow.x + waterFlow.y + waterFlow.z + waterFlow.w;
	float deltaWaterVolume = waterDeltaTime * (inFlow - outFlow);
	// Delta volume
	waterHeight += deltaWaterVolume * rcp(terrainDataInterval * terrainDataInterval);

	// Regenerate terrain normal
	float4 terrainNormal = CalculateTerrainNormal(terrainHeightIn, terrainHeightR, terrainHeightT);

	// Delta terrain height
	float4 deltaTerrainHeights = lerp(
		terrainHeightIn.xxxx, float4(terrainHeightR.x, terrainHeightL.x, terrainHeightT.x, terrainHeightB.x), deltaTerrainDistance
	);
	float2 partialTerrainHeightXY = (deltaTerrainHeights.xz - deltaTerrainHeights.yw) * (terrainInvDataInterval * (0.5 * (1.0 / deltaTerrainDistance)));
	float terrainSlopeMagnitude = length(partialTerrainHeightXY);
	float terrainSlope = terrainSlopeMagnitude * rcp(sqrt(1.0 + terrainSlopeMagnitude * terrainSlopeMagnitude));

	// Sediment dissolution and deposition.
	// To avoid sediment in terrain surface
	if (waterHeight > terrainHeight) {
		// WaterVelocity: m/s
		// Flow limitation
		// 照搬参考：((terrainHeightIn.y - terrainHeightIn.x, 0.0) + (waterHeight - terrainHeight)) * 0.5
		// 参考的逗号表达式求值为 0.0，实际即 (waterHeight - terrainHeight) * 0.5。
		float avgWaterHeight = (waterHeight - terrainHeight) * 0.5;
		float2 deltaWaterFlow = float2(
				((waterFlowL.z - waterFlow.w) + (waterFlow.z - waterFlowR.w)),
				((waterFlowB.x - waterFlow.y) + (waterFlow.x - waterFlowT.y))
			) * 0.5;
		float2 waterVelocity = deltaWaterFlow / (max((abs(avgWaterHeight) * terrainDataInterval), 1e-1) * sign(avgWaterHeight)) * waterDeltaTime;

		// Sediment transportation (Semi-Lagrangian method)
		// Sediment is interporated from the source pos.
		float sediment = LoadTerrainBilinear(WaterSediment, worldPos + (0.5 * terrainDataInterval) - WaterVelocity[threadID.xy]);

		float sedimentCapability = waterSedimentCapability * length(waterVelocity) * min(terrainSlope, maxSedimentSlope);
		float deltaDissolvingSediment = (sedimentCapability - sediment);
		deltaDissolvingSediment *= deltaDissolvingSediment > 0.0 ? soilDissolvingFactor : sedimentDepositingFactor;
		terrainHeight -= deltaDissolvingSediment;
		sediment += deltaDissolvingSediment;
		WaterSediment[threadID.xy] = sediment;
		WaterVelocity[threadID.xy] = waterVelocity;
	}

	// Thermal Erosion
	float soilMoisture = SoilMoisture(waterHeight - terrainHeight);
	float thermalThreashold = 1.0;
	if (waterHeight > terrainHeight) { // Underwater
		thermalThreashold *= thermalUnderwaterFactor;
	} else { // Climate
		// WindErosion
		thermalThreashold += max(dot(-meteorograph.xy, terrainNormal.xy) * (1.0 - soilMoisture), 0.0) * thermalWindIntensity;
		// HeatErosion
		thermalThreashold -= min(max(meteorograph.w - polarTemperature, 0.0) * (thermalHeatIntensity * 0.01), thermalHeatIntensity);
		thermalThreashold = saturate(thermalThreashold);
	}
	// Apply Thermal Erosion
	if (terrainSlope > thermalThreashold) {
		terrainHeight = lerp(
			terrainHeight,
			(terrainHeightT.x + terrainHeightB.x + terrainHeightR.x + terrainHeightL.x) * 0.25,
			saturate(waterDeltaTime * thermalErosionRate * (terrainSlope - thermalThreashold))
		);
	}

	// Water Evaporation.
	waterHeight -= DeltaEvaporation(length(meteorograph.xy), meteorograph.zw, soilMoisture, hydrologicalCycleRate * evaporationFactor, deltaTime);

	// Outputs
	// Balance sealevel from edge
	bool isEdgeCell = threadID.x >= (terrainDataSideElements - 1)
		| threadID.y >= (terrainDataSideElements - 1)
		| threadID.x <= 0
		| threadID.y <= 0;

	TerrainHeight[threadID.xy] = float2(terrainHeight, isEdgeCell ? max(waterHeight, waterBaseHeight) : waterHeight);
	TerrainNormal[threadID.xy] = terrainNormal;

	float waterToTerrainHeight = terrainHeight - waterHeight;
	float4 terrainSurface = 0.0;
	// .r Slope
	terrainSurface.r = smoothstep(0.3, 0.5, length(terrainNormal.xy));
	// .g Beach
	terrainSurface.g = max(1.0 - max((waterToTerrainHeight) * 10.0, 0.0), 0.0);
	// .b Humidity
	terrainSurface.b = max((waterToTerrainHeight - 0.5) * invMaxWaterSoilDiffuseDistance, 0.0);

	// use terrainSurface.a as roughness
	static const float surfaceRoughnessRange = 0.4;
	terrainSurface.a = lerp(
		0.95,
		0.2,
		-(1.0 / surfaceRoughnessRange) * min(abs(waterToTerrainHeight + 0.2) - surfaceRoughnessRange, 0.0)
	);

	TerrainSurface[threadID.xy] = terrainSurface;
}
