// Character vertex shader, ported 1:1 from reference DefaultLit_VS.hlsl.
// Outputs worldPosition/worldNormal/worldTangent/worldBitangent/color/texCoord0.
#include "CharacterCommon.hlsl"

VSOutput VSMain(VSInput input) {
	VSOutput output;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));
	output.position = mul(viewProjection, worldPosition);
	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul((float3x3)invTransModel, input.normal));
	output.worldTangent = normalize(mul((float3x3)invTransModel, input.tangent));
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);
	output.color = input.color;
	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
