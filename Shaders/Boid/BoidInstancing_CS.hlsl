// Boid 每帧模拟（照搬参考项目 Boid/BoidInstancing_CS.hlsl，O(N²) 群集行为）。
// 参考 accessMode=ReadWrite → D3D12 用 RWStructuredBuffer 读写同一 UAV（避免 SRV+UAV 同 pass 冲突）。
// 输入/输出：
//   TerrainHeight(t0 SRV 读, 地形回避)
//   InstanceBuffer(u0 RW：读当前位置/写新位置)
//   BoidBuffer(u1 RW：读属性/写新速度加速度)
#include "BoidStructs.hlsl"
#include "../Common.hlsl"      // Snorm/Square/InverseRigidTransform
#include "../Random.hlsl"
#include "../TerrainCommon.hlsl"
#include "../Constants.hlsl"

cbuffer BoidUniform : register(b0) {
	float time;
	float deltaTime;
	float2 _pad0;
	float4x4 model;         // 场景变换（scale 0.2 + rotate -90X → y-up）
	float4x4 invTransModel;
	float objectID;
	float3 _pad1;
};

Texture2D<float2> TerrainHeight : register(t0);
RWStructuredBuffer<InstanceBufferStruct> InstanceBuffer : register(u0);
RWStructuredBuffer<BoidBufferStruct> BoidBuffer : register(u1);

struct BoidDistanceInfo {
	float3 position;
	float distance;
	uint index;
};

struct BoidAccumulationInfo {
	float3 position;
	uint numBoids;
	float3 acceleration;
};

[numthreads(256, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	static const uint boidCount = 1024;
	// Temporary: If boid lost its target, go back to here.
	static const float3 homePos = float3(sin(time * 0.2) * 100.0, cos(time * 0.2) * 100.0, 0.5);

	uint index = threadID.x;
	BoidBufferStruct boid = BoidBuffer[index];
	InstanceBufferStruct instanceInfo = InstanceBuffer[index];
	float3 position = float3(
		instanceInfo.instanceModel[0].w,
		instanceInfo.instanceModel[1].w,
		instanceInfo.instanceModel[2].w
	);
	float3 acceleration = boid.acceleration;
	float rcpPerception = rcp(boid.perception);
	// The exponent smaller, the flock size larger.
	float alignedAccelerationWeight = pow(rcpPerception, 0.04 * sqrt(rcp(deltaTime)));

	BoidDistanceInfo closestBoidInfo = { float3(0.0, 0.0, 0.0), 1e+16f, index };
	BoidAccumulationInfo boidAccumulationInfo = { float3(0.0, 0.0, 0.0), 0, float3(0.0, 0.0, 0.0) };

	for (uint otherBoidIndex = 0; otherBoidIndex < boidCount; ++otherBoidIndex) {
		if (otherBoidIndex == index) {
			continue;
		}
		InstanceBufferStruct otherBoidInstanceInfo = InstanceBuffer[otherBoidIndex];
		BoidDistanceInfo otherBoidInfo;
		otherBoidInfo.position = float3(
			otherBoidInstanceInfo.instanceModel[0].w,
			otherBoidInstanceInfo.instanceModel[1].w,
			otherBoidInstanceInfo.instanceModel[2].w
		);
		otherBoidInfo.distance = distance(position, otherBoidInfo.position);
		otherBoidInfo.index = otherBoidIndex;

		// Use Perception to accumulate position, for a large gathering magnitude.
		if (otherBoidInfo.distance <= boid.perception) {
			boidAccumulationInfo.position += lerp(position, otherBoidInfo.position, rcpPerception);
			++boidAccumulationInfo.numBoids;
			boidAccumulationInfo.acceleration += lerp(acceleration, BoidBuffer[otherBoidIndex].acceleration, alignedAccelerationWeight);
		}

		if (otherBoidInfo.distance < closestBoidInfo.distance) {
			closestBoidInfo = otherBoidInfo;
		}
	}

	// If have not any other boid in perception, back to the origin.
	float rcpNumBoids = rcp(boidAccumulationInfo.numBoids);
	float3 toCenter = boidAccumulationInfo.numBoids > 0 ? boidAccumulationInfo.position * rcpNumBoids - position : homePos - position;
	float3 toCenterDir = normalize(toCenter);
	float3 maxAccelerationToCenter = boid.maxAcceleration * toCenterDir;

	BoidBufferStruct closestBoid = BoidBuffer[closestBoidInfo.index];
	float3 toClosestBoid = closestBoidInfo.position - position;
	float3 toClosestBoidDir = normalize(toClosestBoid);
	float3 maxAccelerationToClosestBoid = boid.maxAcceleration * toClosestBoidDir;

	float closestInfluence = (closestBoid.influenceRadius + boid.influenceRadius) - closestBoidInfo.distance;
	float closestInfluenceWeight = Square(max(closestInfluence, 0.0) / closestBoidInfo.distance);

	// P2: Matching closest boids velocity
	float3 alignedAcceleration = (boidAccumulationInfo.numBoids > 0 ? boidAccumulationInfo.acceleration * rcpNumBoids : acceleration);
	acceleration = lerp(alignedAcceleration, acceleration, boid.curiosity);

	// P0, P1...: The priority of the action, smaller first.
	static const float toCenterFactor = 1.0;
	float3 steeringAcceleration = normalize(toClosestBoidDir + toCenterDir) * boid.maxAcceleration;
	acceleration += closestInfluenceWeight == 0.0 ? maxAccelerationToCenter * (toCenterFactor * deltaTime) : steeringAcceleration * boid.turnSpeed;
	acceleration -= maxAccelerationToClosestBoid * closestInfluenceWeight;

	// TODO: Pull up from terrain, use terrain buffer intead. try better steering way.
	// 本工程 y-up：model 含 rotate(-90X) 是 y-up 世界，不能用其平移采样地形。
	// 直接用模拟坐标 position（z-up，与 TerrainHeight 纹理 z-up 平面坐标系一致）。
	float2 terrainHeight = LoadTerrain(TerrainHeight, position.xy);
	static const float terrainAvoidance = 5.0;
	acceleration.z += max(max(terrainHeight.x, terrainHeight.y) + terrainAvoidance - position.z, 0.0) * 0.05 * boid.maxAcceleration.z;

	// Update velocity
	float3 chaseAcceleration = boid.maxAcceleration + (closestBoidInfo.distance / boid.perception) * boid.maxAcceleration * 2.0;
	acceleration = clamp(acceleration, -chaseAcceleration, chaseAcceleration);

	float3 velocity = clamp(boid.velocity + acceleration * deltaTime, -boid.maxVelocity, boid.maxVelocity);

	BoidBuffer[index].velocity = velocity;

	// Acceleration attenuation
	BoidBuffer[index].acceleration = acceleration;

	// Apply to transform
	position += velocity * deltaTime;

	InstanceBuffer[index].instanceModel[0].w = position.x;
	InstanceBuffer[index].instanceModel[1].w = position.y;
	InstanceBuffer[index].instanceModel[2].w = position.z;

	// Process Orientation
	float3 velocityDir = normalize(velocity);
	float yaw = atan2(velocityDir.y, velocityDir.x);
	float pitch = acos(velocityDir.z) - (0.5 * pi);

	float sinYaw, cosYaw, sinPitch, cosPitch;
	sincos(yaw, sinYaw, cosYaw);
	sincos(pitch, sinPitch, cosPitch);

	InstanceBuffer[index].instanceModel[0][0] = cosYaw * cosPitch;
	InstanceBuffer[index].instanceModel[0][1] = -sinYaw;
	InstanceBuffer[index].instanceModel[0][2] = cosYaw * sinPitch;
	InstanceBuffer[index].instanceModel[1][0] = sinYaw * cosPitch;
	InstanceBuffer[index].instanceModel[1][1] = cosYaw;
	InstanceBuffer[index].instanceModel[1][2] = sinYaw * sinPitch;
	InstanceBuffer[index].instanceModel[2][0] = -sinPitch;
	InstanceBuffer[index].instanceModel[2][1] = 0.0;
	InstanceBuffer[index].instanceModel[2][2] = cosPitch;

	// Update Matrices
	InstanceBuffer[index].model = mul(model, InstanceBuffer[index].instanceModel);
	InstanceBuffer[index].invTransModel = mul(
		transpose(InverseRigidTransform(InstanceBuffer[index].instanceModel)),
		invTransModel
	);
}
