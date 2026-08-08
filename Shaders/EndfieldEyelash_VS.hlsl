// Yvonne Eyelash VS：照搬参考项目 EndfieldEyelash_VS.hlsl（深度偏移）。
#include "CharacterCommon.hlsl"
#include "EndfieldCommon.hlsl"

VSOutput VSMain(VSInput input) {
	VSOutput output;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));
	output.position = mul(viewProjection, worldPosition);

	// Depth offset.
	output.position.z -= eyelashDepthOffset / output.position.w;

	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul((float3x3)invTransModel, input.normal));
	output.worldTangent = normalize(mul((float3x3)invTransModel, input.tangent));
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);
	output.color = input.color;
	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
