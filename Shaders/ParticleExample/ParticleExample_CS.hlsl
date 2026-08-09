// ParticleExample 每帧模拟（照搬参考 ParticleExample_CS.hlsl，恢复参考球体分布）。
// z-up 平面世界：xy 水平、z 高度（粒子在相机周围重生 + 地形高度 + 气象风场）。
// 单缓冲 in-place（本工程 Boids 同款），参考用 In/Out 双缓冲。
#include "../Common.hlsl"
#include "../Random.hlsl"
#include "../MeteorographCommon.hlsl"
#include "../TerrainCommon.hlsl"
#include "ParticleCommon.hlsl"

cbuffer ParticleSimParams : register(b0) {
	float3 cameraPosition;      // z-up 当前帧
	float time;
	float3 prevCameraPosition;  // z-up 上一帧（camDelta 粒子跟随相机）
	float deltaTime;
};

Texture2D<float2> TerrainHeight : register(t0);
Texture2D<float4> Meteorograph : register(t1);
SamplerState MeteorographSampler : register(s0);
RWStructuredBuffer<ParticleSSBOStruct> ParticleSSBO : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	uint index = threadID.x;
	ParticleSSBOStruct p = ParticleSSBO[index];

	static const float fadeThreshold = 0.25;
	static const float regeneratedRange = 8.0;
	static const float maxHorizontalSpeed = 0.5;
	static const float frictionFactor = 0.005;

	// boundary（参考 ±10，z 从 0 起）。
	const float3 boundaryMin = float3(-10.0, -10.0, 0.0) + cameraPosition.xyz;
	const float3 boundaryMax = float3(10.0, 10.0, 10.0) + cameraPosition.xyz;

	float deltaElapseTime = deltaTime;
	// 粒子世界位置跟随相机（camDelta 平移，必要修复：否则移动相机粒子留在原地）。
	float3 positionOut = p.position + (cameraPosition - prevCameraPosition);

	// If particle leaving boundary, elapsing double time.
	if (any(positionOut < boundaryMin) || any(positionOut > boundaryMax)) {
		deltaElapseTime *= 4;
	}

	p.elapseTime = p.elapseTime + deltaElapseTime;

	if (p.elapseTime > p.maxLifeTime) {
		p.elapseTime = 0.0;
		// Generate higher than the ground and not in the upper air.
		positionOut = Hash3D(float3(index, index, index), randomSeed0);
		positionOut.xy = Snorm(positionOut.xy);
		float3 offset = { cameraPosition.xy, 0.0 };
		positionOut = mad(positionOut, regeneratedRange, offset);

		float2 terrainHeight = LoadTerrain(TerrainHeight, positionOut.xy);
		positionOut.z += terrainHeight.x;

		// 重生时刷新粒子色（参考 init 的 Hash 色）。
		p.color.rgb = Hash3D(float3(index, index, index), randomSeed0 * 0.5);
	} else {
		positionOut = p.position + p.velocity * deltaTime * 1.0;
	}
	p.position = positionOut;

	// Accumulate wind velocity.
	float3 velocityOut = p.velocity;
	velocityOut.xy *= (1.0 - frictionFactor);
	velocityOut.xy += SampleMeteorograph(Meteorograph, MeteorographSampler, positionOut.xy).xy * deltaTime;
	velocityOut.xy = normalize(velocityOut.xy) * min(length(velocityOut.xy), maxHorizontalSpeed);
	p.velocity = velocityOut;

	// Particle Opacity
	float fadeValue = min(min(p.maxLifeTime - p.elapseTime, p.elapseTime) / p.maxLifeTime, fadeThreshold);
	p.color.a = fadeValue * (1.0 / fadeThreshold);

	ParticleSSBO[index] = p;
}
