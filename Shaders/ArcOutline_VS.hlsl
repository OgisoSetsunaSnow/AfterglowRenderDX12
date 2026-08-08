// BattleMage 描边：照搬参考项目 Shaders/Toon/ArcOutline_VS.hlsl。
// 双因子外扩：faceFadedDir = (worldPos-camPos)*(1-color.b) + suspensionFadedDir = normal*color.a；clamp 0.01。
#include "CharacterCommon.hlsl"

UnlitVSOutput VSMain(VSInput input) {
	UnlitVSOutput output;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));
	float3 worldNormal = normalize(mul((float3x3)invTransModel, input.normal));

	// Expand world position by normal.
	float distanceFade = min(distance(cameraPosition, worldPosition.xyz) * outlineParams.y, 0.01);
	float fovFade = invProjection[0][0] * invScreenAspectRatio;
	float3 faceFadedDir = (worldPosition.xyz - cameraPosition) * (1.0 - input.color.b);
	float3 suspensionFadedDir = worldNormal * input.color.a;
	worldPosition.xyz += outlineParams.x * (distanceFade * fovFade) * (faceFadedDir + suspensionFadedDir);

	output.position = mul(viewProjection, worldPosition);
	output.texCoord0 = input.uv;
	return output;
}
