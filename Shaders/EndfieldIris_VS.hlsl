// Yvonne Iris VS：照搬参考项目 EndfieldIris_VS.hlsl（color.x = tangent.x 区分左右眼）。
#include "CharacterCommon.hlsl"

VSOutput VSMain(VSInput input) {
	VSOutput output;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));
	output.position = mul(viewProjection, worldPosition);
	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul((float3x3)invTransModel, input.normal));
	output.worldTangent = normalize(mul((float3x3)invTransModel, input.tangent));
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);

	// color.x stores tangent.x object space use to distinguish left iris and right iris.
	output.color = input.color;
	output.color.x = input.tangent.x;

	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
