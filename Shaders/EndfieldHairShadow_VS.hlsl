// Yvonne HairShadow VS：照搬参考项目 EndfieldHairShadow_VS.hlsl（object space bias Z 平移 + depth bias）。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"

VSOutput VSMain(VSInput input) {
	static const float objectSpaceBiasY = 0.012;
	static const float lightBiasIntensity = 0.0025;
	static const float depthBias = 0.005;

	VSOutput output;
	input.pos.y += objectSpaceBiasY;
	input.pos.x *= 1.0 + objectSpaceBiasY * 8.0;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));

	float3 forwardDirection = mul(model, float4(0.0, 1.0, 0.0, 0.0)).xyz;
	float3 rightDirection = mul(model, float4(1.0, 0.0, 0.0, 0.0)).xyz;

	float3 projectedLightVector = ProjectVector(dirLightDirection.xyz, forwardDirection);
	float biasFactor = abs(dot(dirLightDirection.xyz, rightDirection));

	worldPosition.xyz -= projectedLightVector * lightBiasIntensity * biasFactor;

	output.position = mul(viewProjection, worldPosition);
	output.position.z -= depthBias / output.position.w;
	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul((float3x3)invTransModel, input.normal));
	output.color = input.color;
	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
