// Grass 每帧实例生成（照搬参考项目 Grass/GrassInstancing_CS.hlsl）。
// 相机前方可见区域 → 视锥剔除 + 坡度剔除 → InterlockedAdd 计数到 IndirectBuffer，
// 写 InstanceBuffer（model/invTransModel/wind）供间接绘制。
// 注：z-up 平面世界（与参考一致）；本工程 y-up 场景，model 含 rotate(-90X)。
#include "../Common.hlsl"
#include "../Random.hlsl"
#include "GrassCommon.hlsl"
#include "../TerrainCommon.hlsl"
#include "../MeteorographCommon.hlsl"

// 视锥剔除依赖 frustumPlane* 全局（cbuffer 成员），故 FrustumCulling 在 cbuffer 之后 include。
cbuffer GrassInstancingParams : register(b0) {
	float time;
	float deltaTime;
	float2 _pad0;
	float3 cameraPosition;
	float cameraNear;
	float3 cameraVector;
	float cameraFar;
	float4 frustumPlaneL, frustumPlaneR, frustumPlaneB, frustumPlaneT, frustumPlaneN, frustumPlaneF;
};
#include "FrustumCulling.hlsl"

Texture2D<float2> TerrainHeight : register(t0);
Texture2D<float4> GrassData : register(t1);
Texture2D<float4> Meteorograph : register(t2);
SamplerState MeteorographSampler : register(s0);

RWStructuredBuffer<InstanceBufferStruct> InstanceBufferOut : register(u0);
RWStructuredBuffer<IndirectBufferStruct> IndirectBufferOut : register(u1);

[numthreads(32, 32, 1)]
void main(
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint3 threadID : SV_DispatchThreadID
) {
	static const float grassCullingRadius = 1.0; // m
	static const float grassCullingOffsetZ = grassCullingRadius * 0.5;
	static const float grassSlopeThreshold = 0.4;

	// 注意：IndirectBuffer 的 indexCount/instanceCount=0/firstIndex/vertexOffset/firstInstance
	// 由 CPU 每帧写入（见 GrassRenderer，参考项目也是 CPU 重置）。此处仅 InterlockedAdd 递增 instanceCount。

	// 相机前方可见矩形偏移（参考：cameraVector 水平方向）。
	float3 directionalOffset = float3(-cameraVector.xy, 0.0) * (grassVisibleSideElement * 0.5);
	float3 worldPos = float3((int2(threadID.xy) - (grassVisibleSideElement * 0.5)) * grassDataInterval + cameraPosition + directionalOffset, 0.0);
	uint2 grassDataID = TileIDFromWorldPosition(worldPos.xy, grassInvDataInterval);
	worldPos.xy = WorldPositionFromTileID(grassDataID, grassInvDataSideElements);

	// Grass culling（地形高度）。
	float terrainHeight = LoadTerrain(TerrainHeight, worldPos.xy).x;
	worldPos.z = terrainHeight;

	if (IsSphereOutsideFrustum(worldPos + float3(0.0, 0.0, grassCullingOffsetZ), grassCullingRadius)) {
		return;
	}

	// Grass possibility
	float4 grassInfo = GrassData[grassDataID];
	float distanceToCamera = distance(cameraPosition, worldPos);
	float distanceFade = max(grassVisibleDistance - distanceToCamera, 0.0) * (1.0 / grassVisibleDistance);

	if (grassInfo.x < grassSlopeThreshold || distanceToCamera > grassVisibleDistance) {
		return;
	}

	uint instanceIndex;
	InterlockedAdd(IndirectBufferOut[0].instanceCount, 1, instanceIndex);

	// Write instance buffer
	InstanceBufferStruct instanceInfo;
	uint id = grassDataID.x * grassDataSideElements + grassDataID.y;

	// World position offset（抖动）
	worldPos.xy += Snorm(Hash2D(worldPos.xy, randomSeed0)) * 0.5;

	// Rotate around z-axis（随机朝向）
	float yaw = UniformBitHash(id) * (2 * pi);
	float sinYaw;
	float cosYaw;
	sincos(yaw, sinYaw, cosYaw);

	// Random scaling（距离衰减 + 草地缩放）
	float scaling = (Snorm(Hash(1.0 / id, randomSeed1)) * 0.2 + 1.0) * min(distanceFade * 2.0, 1.0) * (grassInfo.z);

	float4x4 instanceModel = {
		cosYaw * scaling, -sinYaw * scaling, 0.0, worldPos.x,
		sinYaw * scaling, cosYaw * scaling, 0.0, worldPos.y,
		0.0, 0.0, scaling, worldPos.z,
		0.0, 0.0, 0.0, 1.0
	};

	instanceInfo.model = instanceModel;
	// 保持草高度（防止物体变换改变高度）。
	instanceInfo.model[2][3] = terrainHeight;

	// 本工程场景无额外变换（草在相机前区域生成），invTransModel = instanceModel 的刚体逆。
	instanceInfo.invTransModel = transpose(InverseRigidTransform(instanceModel));

	// Clamped wind（气象风场）
	float2 wind = SampleMeteorograph(Meteorograph, MeteorographSampler, worldPos.xy).xy;
	instanceInfo.wind = ClampVectorWithLength(wind, 0.5 * scaling);

	InstanceBufferOut[instanceIndex] = instanceInfo;
}
