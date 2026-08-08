// Boid 属性初始化（一次性）：照搬参考项目 Boid/BoidAttributeInitializer_CS.hlsl。
// 写 BoidBuffer（u0）：acceleration/influenceRadius/maxAcceleration/turnSpeed/velocity/curiosity/maxVelocity/perception。
#include "BoidCommon.hlsl"
#include "../Random.hlsl"

RWStructuredBuffer<BoidBufferStruct> BoidBufferOut : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	uint index = threadID.x;

	// Initialize boid buffer
	BoidBufferOut[index].acceleration = Snorm(Hash3D(float3(index, index, index), randomSeed0 - 104.351)) * 0.5;
	BoidBufferOut[index].influenceRadius = (Hash(index, randomSeed0) + 2.0) * 1.0;
	float3 maxAcceleration = (Hash3D(1.0 / float3(index, index, index), randomSeed0 + 34.124) + 8.0);
	BoidBufferOut[index].maxAcceleration = maxAcceleration;
	BoidBufferOut[index].turnSpeed = (UniformBitHash(index) + 2.0) * 0.0001;
	BoidBufferOut[index].velocity = float3(0.0, 0.0, 0.0);
	BoidBufferOut[index].curiosity = saturate((Hash(index, randomSeed1) + 0.2) * 0.25);
	BoidBufferOut[index].maxVelocity = (Hash3D(maxAcceleration, randomSeed1) + 8.0) * 1.0;
	BoidBufferOut[index].perception = (Hash(BoidBufferOut[index].curiosity, randomSeed0) + 0.2) * 20;
}
