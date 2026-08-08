// Yvonne Eyelash：照搬参考项目 EndfieldEyelash_FS.hlsl（albedo + ramp + 深度淡化 alpha）。
#include "CharacterCommon.hlsl"
#include "EndfieldCommon.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D rampTex : register(t1);
SamplerState rampTexSampler : register(s1);

struct FSOut {
	float4 color : SV_TARGET;
	float depth : SV_DEPTH;
};

FSOut PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) {
	FSOut output;
	static const float illuminanceCorrect = 0.5;

	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);

	float radiance = max(dot(input.worldNormal, dirLightDirection.xyz), 0.0);

	// Color ramp
	float2 rampCoord = float2(clamp(0.8 - radiance * 0.5, 0.1, 0.9), 0.5);
	float4 rampColor = rampTex.Sample(rampTexSampler, rampCoord);

	float3 finalColor = baseColor.rgb * (radiance + (rampColor.rgb * rampColor.a) * min(materialParams.z, baseColor.xyz));

	finalColor *= dirLightColor.xyz * dirLightColor.w * illuminanceCorrect;

	output.color.xyz = finalColor;
	output.color.a = 1.0; // 参考项目 depthFade 需 scene depth；简化不淡化。
	output.depth = input.position.z;
	return output;
}
