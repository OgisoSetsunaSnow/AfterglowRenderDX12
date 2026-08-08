// Boid 公共定义：结构（BoidStructs）+ 每帧 cbuffer + 工具。
// 本工程 y-up 场景：Boid 模拟在 z-up 语义（照搬参考），渲染时 model 含 rotate(-90X) 转 y-up。
#ifndef BOID_COMMON_HLSL
#define BOID_COMMON_HLSL

#include "BoidStructs.hlsl"
#include "../Common.hlsl"      // Snorm/Square/InverseRigidTransform
#include "../Random.hlsl"
#include "../TerrainCommon.hlsl"
#include "../Constants.hlsl"

// ---- b0: BoidUniform（CS 模拟用：time/deltaTime + 场景 model/invTransModel/objectID）----
cbuffer BoidUniform : register(b0) {
	float time;
	float deltaTime;
	float2 _pad0;
	float4x4 model;         // 场景变换（scale 0.2 + rotate -90X → y-up）
	float4x4 invTransModel;
	float objectID;
	float3 _pad1;
};

#endif
