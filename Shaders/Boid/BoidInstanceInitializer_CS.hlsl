// Boid 实例初始化（一次性）：照搬参考项目 Boid/BoidInstanceInitializer_CS.hlsl。
// 写 InstanceBuffer（u0）：instanceModel 平移（球面随机散布）+ 单位旋转。
#include "BoidCommon.hlsl"
#include "../Random.hlsl"

RWStructuredBuffer<InstanceBufferStruct> InstanceBufferOut : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	uint index = threadID.x;

	// Initialize instancing buffer
	// Translation: Spherical shape
	// Sphere center higher than the ground.
	static const float3 center = { 0, 0, 20 };
	float3 direction = Snorm(Hash3D(float3(index, index, index), randomSeed0));
	float radius = Hash(float(index), randomSeed0) * 1.0;
	float3 translation = direction * radius + center;
	// Transfrom matrix store by row, but it is a Column-major matrix in actual.
	InstanceBufferOut[index].instanceModel = 0.0;
	InstanceBufferOut[index].instanceModel[0].w = translation.x;
	InstanceBufferOut[index].instanceModel[1].w = translation.y;
	InstanceBufferOut[index].instanceModel[2].w = translation.z;
	InstanceBufferOut[index].instanceModel[3].w = 1.0;

	// Rotation: Initialize by random direction
	// From quaternion to rotate matrix
	float4 quaternion = normalize(Snorm(float4(
		Hash3D(direction, randomSeed0),
		Hash(radius, randomSeed0)
	)));
	// Quaternion to matrix
	InstanceBufferOut[index].instanceModel[0][0] = 1.0;
	InstanceBufferOut[index].instanceModel[1][1] = 1.0;
	InstanceBufferOut[index].instanceModel[2][2] = 1.0;
}
