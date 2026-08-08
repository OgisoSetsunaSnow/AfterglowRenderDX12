// ShellFur instanced VS, ported 1:1 from reference ShellFur/ShellFur_VS.hlsl.
// 每实例 = 一层毛壳（shell）：顶点沿模型空间法线偏移 (instanceID+1)*shellInterval。
// model 矩阵含 scale（Ankha 0.02），shell 间距在世界空间 = shellInterval*scale。
#include "../Common.hlsl"

cbuffer FurUniform : register(b0) {
	float4x4 model;
	float4x4 invTransModel;
	float4x4 viewProjection;
	float objectID;
	float instanceCount;   // 参考 ShellFur.mat scalars instanceCount=8
	float shellInterval;   // 参考 shellInterval=0.03
	float _pad;
};

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
	float3 worldTangent : TANGENT;
	float3 worldBitangent : BITANGENT;
	uint objectID : OBJECT_ID;
	uint instanceID : INSTANCE_ID;
	float4 color : COLOR;
	float2 texCoord0 : TEXCOORD;
};

VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID) {
	VSOutput output;

	float3 position = input.pos;
	position += input.normal * (instanceID + 1) * shellInterval;

	float4 worldPosition = mul(model, float4(position, 1.0));
	output.position = mul(viewProjection, worldPosition);
	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul(invTransModel, float4(input.normal, 0.0)).xyz);
	output.worldTangent = normalize(mul(invTransModel, float4(input.tangent, 0.0)).xyz);
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);
	output.color = input.color;
	output.objectID = (uint)objectID;
	output.instanceID = instanceID;
	output.texCoord0 = input.uv;
	return output;
}
