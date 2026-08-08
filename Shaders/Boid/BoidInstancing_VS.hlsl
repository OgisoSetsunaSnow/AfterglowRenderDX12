// Boid instanced vertex shader, ported 1:1 from reference Boid/BoidInstancing_VS.hlsl.
// InstanceBuffer 绑定 VS 专属 t0（D3D12 允许同 register 不同 visibility）：
//   PS 的 t0 是 albedo（DefaultLit_FS），VS 的 t0 是 InstanceBuffer，互不干扰。
#include "BoidStructs.hlsl"
#include "../CharacterCommon.hlsl"

StructuredBuffer<InstanceBufferStruct> InstanceBufferIn : register(t0);

VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID) {
	VSOutput output;
	InstanceBufferStruct instanceInfo = InstanceBufferIn[instanceID];

	float4 worldPosition = mul(instanceInfo.model, float4(input.pos, 1.0));
	output.worldPosition = worldPosition.xyz;

	output.position = mul(viewProjection, worldPosition);

	output.worldNormal = normalize(mul((float3x3)instanceInfo.invTransModel, input.normal));
	output.worldTangent = normalize(mul((float3x3)instanceInfo.invTransModel, input.tangent));
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);

	output.color = input.color;
	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
