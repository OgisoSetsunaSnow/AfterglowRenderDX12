// ParticleExample 粒子结构（照搬参考 ParticleExample.mat elementLayout：position/maxLifeTime/velocity/elapseTime/color）。
#ifndef PARTICLE_COMMON_HLSL
#define PARTICLE_COMMON_HLSL

// HLSL StructuredBuffer 对齐：float3@0、float@12、float3@16、float@28、float4@32 → 48B。
struct ParticleSSBOStruct {
	float3 position;     // 0
	float maxLifeTime;   // 12
	float3 velocity;     // 16
	float elapseTime;    // 28
	float4 color;        // 32
};

#endif
