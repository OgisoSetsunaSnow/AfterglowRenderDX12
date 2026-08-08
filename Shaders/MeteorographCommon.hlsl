// Ported 1:1 from reference project Meteorograph/MeteorographCommon.hlsl.
#ifndef METEOROGRAPH_COMMON_HLSL
#define METEOROGRAPH_COMMON_HLSL

#include "WorldDefinitions.hlsl"
#include "Constants.hlsl"

static const uint meteorographSideElements = 128;
static const float meteorographInvSideElements = 1.0 / meteorographSideElements;
static const float meteorographInterval = worldSideLength / meteorographSideElements;
static const float meteorographInvInterval = 1.0 / meteorographInterval;

static const float absoluteZeroTemperature = -273.15;
static const float polarTemperature = -10.0;
static const float equatorialTemperature = 30.0;
static const float equatorialEvaporationRate = 1.2e-4f;
static const float environmentalLapseRate = -0.325;
static const float baseAirConvection = 0.2;
static const float baseAirResistance = 1.0e-4;

static const float maxWaterSoilDiffuseDistance = 1.0;
static const float soilEvaporationFactor = 0.2;
static const float invMaxWaterSoilDiffuseDistance = 1.0 / maxWaterSoilDiffuseDistance;

float SunlightRatio(float worldPosY) {
	float y = abs(worldPosY * worldInvSideLength * 2.0);
	return dot(float2(1.0, 0.0), float2(1.0 - y, y));
}

float SurfaceTemperature(float worldPosY, float terrainHeight) {
	float seaLevelTemperature = lerp(polarTemperature, equatorialTemperature, SunlightRatio(worldPosY));
	return max(seaLevelTemperature + terrainHeight * environmentalLapseRate, absoluteZeroTemperature);
}

float SoilMoisture(float deltaWaterHeight) {
	return soilEvaporationFactor - clamp(-deltaWaterHeight * (invMaxWaterSoilDiffuseDistance * soilEvaporationFactor), 0.0, soilEvaporationFactor);
}

float DeltaEvaporation(float windSpeed, float2 humiture, float soilMoisture, float area, float deltaTime) {
	float clampedTemperature = clamp(humiture.y, 0.0, 100.0);
	float vaporPressure = 610.78 * exp((17.27 * clampedTemperature) / (clampedTemperature + 237.3));
	float vpd = max(vaporPressure - humiture.x * vaporPressure, 0.0);
	return equatorialEvaporationRate * max(windSpeed, baseAirConvection) * vpd * soilMoisture * area * deltaTime;
}

template<typename ElementType>
ElementType LoadMeteorograph(Texture2D<ElementType> data, float2 worldPosition) {
	uint2 dataIndex = clamp((worldPosition - worldCenterOffset) * meteorographInvInterval, 0, meteorographSideElements - 1);
	return data.Load(uint3(dataIndex, 0));
}

template<typename ElementType>
ElementType SampleMeteorograph(Texture2D<ElementType> data, SamplerState dataSampler, float2 worldPosition) {
	float2 uv = clamp((worldPosition - worldCenterOffset) * (meteorographInvInterval * meteorographInvSideElements), 0.0, 1.0);
	return data.SampleLevel(dataSampler, uv, 0);
}

#endif
