// 当前实现：CharacterRenderer 统一使用 CharacterVS（透传顶点色）；EndfieldBody 的 normalized position
// bitangent 翻转依赖 input.color.x，模型顶点色为白时近似（color.x=1，bitangent 不翻转）。
// 完整 EndfieldBody_VS（计算 normalized position 到 color.xyz）保留作为参考实现。
#include "CharacterCommon.hlsl"

VSOutput VSMain(VSInput input) {
	VSOutput output;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));
	output.position = mul(viewProjection, worldPosition);
	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul((float3x3)invTransModel, input.normal));
	output.worldTangent = normalize(mul((float3x3)invTransModel, input.tangent));
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);

	// color stores normalized object space position.
	float3 normalizePosition = (input.pos - minAABB) / max(maxAABB - minAABB, 1e-6);
	output.color.xyz = normalizePosition;
	output.color.w = input.color.w;

	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
