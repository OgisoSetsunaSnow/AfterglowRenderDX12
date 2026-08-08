// Boid 数据结构定义（照搬参考 BoidInstancing.mat elementLayout 生成的 HLSL 结构）。
// 纯结构头：被 CS（模拟）和 VS（渲染）共享，避免 cbuffer 布局冲突。
#ifndef BOID_STRUCTS_HLSL
#define BOID_STRUCTS_HLSL

// InstanceBuffer：1024 实例（elementLayout: instanceModel:9 model:9 invTransModel:9）。
struct InstanceBufferStruct {
	float4x4 instanceModel; // 实例自身模型（z-up 模拟坐标）
	float4x4 model;         // mul(model, instanceModel)（场景×实例 → y-up 显示）
	float4x4 invTransModel; // 法线变换
};

// BoidBuffer：1024 实例（elementLayout: acceleration:5 influenceRadius:3 maxAcceleration:5 turnSpeed:3 velocity:5 curiosity:3 maxVelocity:5 perception:3）。
// float3 16B 对齐（HLSL StructuredBuffer 规则）；C++ 端需 alignas(16) 匹配。
struct BoidBufferStruct {
	float3 acceleration;
	float influenceRadius;
	float3 maxAcceleration;
	float turnSpeed;
	float3 velocity;
	float curiosity;
	float3 maxVelocity;
	float perception;
};

#endif
