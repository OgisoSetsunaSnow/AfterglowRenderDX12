// Grass instanced vertex shader, ported 1:1 from reference Grass/GrassInstancing_VS.hlsl.
// z-up 语义：InstanceBuffer 的 model 是 z-up 世界矩阵；viewProjection 由 C++ 传 z-up 版
// （= y-up viewProj * rotate(-90X)），保证输出正确且 FS 的 worldPosition 保持 z-up 采样地形。
#include "../Common.hlsl" // Square/Unorm
#include "../Random.hlsl"
#include "../Constants.hlsl"
#include "GrassCommon.hlsl"

cbuffer GrassUniform : register(b0) {
	float time;
	float deltaTime;
	float2 _pad0;
	float4x4 viewProjection;
	float objectID;
	float3 _pad1;
};

StructuredBuffer<InstanceBufferStruct> InstanceBufferIn : register(t0);

struct VSInput {
	float3 pos : POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
	float4 color : COLOR;
	float3 tangent : TANGENT;
};

struct VSOutput {
	float4 position : SV_POSITION;
	float3 worldPosition : POSITION;
	float3 worldNormal : NORMAL;
	float4 color : COLOR;
	uint objectID : OBJECT_ID;
};

VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID) {
	static const float grassStiffness = 1.2;

	VSOutput output;

	InstanceBufferStruct instanceInfo = InstanceBufferIn[instanceID];

	float4 worldPosition = mul(instanceInfo.model, float4(input.pos, 1.0));
	output.worldPosition = worldPosition.xyz;

	// Wind effect（草尖摆动）
	float instancePosID = instanceInfo.model[0][3] + instanceInfo.model[1][3];
	float randomSpeedFactor = 2.0 * (1.0 + Hash(instancePosID, randomSeed0));

	float2 tipFrequency = Square(1.0 - abs(sin(time * 1.0 + randomSpeedFactor - rcp(worldPosition.xy) * 0.001 * instanceInfo.wind)));
	float2 amplitude = Unorm(sin(time * 2.5 + (tipFrequency * 8.0) + worldPosition.xy * 0.5)) * 0.2 + tipFrequency * 0.8 + 0.2;
	float3 vertexOffset = 0.0;
	vertexOffset.xy = instanceInfo.wind * amplitude * pow(max(input.pos.z, 0.0), grassStiffness);
	vertexOffset.z = -length(vertexOffset.xy);
	vertexOffset *= Square(input.color.x);

	worldPosition.xyz += vertexOffset;

	output.position = mul(viewProjection, worldPosition);
	output.worldNormal = normalize(mul(instanceInfo.invTransModel, float4(input.normal, 0.0)).xyz);
	output.color = input.color;
	output.color.y = length(vertexOffset);
	output.objectID = (uint)objectID;
	return output;
}
