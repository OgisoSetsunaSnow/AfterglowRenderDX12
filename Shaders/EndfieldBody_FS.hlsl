// Yvonne Body/Neck 材质：照搬参考项目 Shaders/Toon/EndfieldBody_FS.hlsl。
// 3 纹理 albedo + normal + ramp；法线贴图 TBN + color ramp + rim light + subsurface。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ColorConversion.hlsl"
#include "ShadingModels.hlsl"
#include "EndfieldCommon.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D normalTex : register(t1);
SamplerState normalTexSampler : register(s1);
Texture2D rampTex : register(t2);
SamplerState rampTexSampler : register(s2);

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	static const float illuminanceCorrect = 0.5;

	// 参考项目 EndfieldBody_FS 的 TBN 处理：mesh tangent 有问题用 color.x 翻转 bitangent；背面取反。
	float3x3 tbn = {
		input.worldTangent,
		input.color.x > 0.5 ? input.worldBitangent : -input.worldBitangent,
		input.worldNormal
	};
	tbn = isFrontFace ? tbn : -tbn;

	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float3 normal = mul(LoadNormalTextureFromXY(normalTex, normalTexSampler, input.texCoord0), tbn);
	float3 view = normalize(cameraPosition - input.worldPosition);
	float vertNov = dot(input.worldNormal, view);
	float vol = dot(-view, dirLightDirection.xyz);
	float vertNol = dot(input.worldNormal, dirLightDirection.xyz);

	float radiance = max(dot(normal, dirLightDirection.xyz), 0.0);

	// View Attenuation
	float viewFactor = lerp(1.0, max(vertNov, 0.0), materialParams.w);
	radiance *= viewFactor;
	float fadedSubsurfaceMFP = materialParams.z * (0.5 + viewFactor * 0.5);

	// Color ramp
	static const float rampOffset = 0.2; // the rampOffset greater, the color more saturation.
	float2 rampCoord = float2(clamp((1.0 - rampOffset) - radiance * (0.5 - (rampOffset * 0.5)), 0.1, 0.9), 0.5);
	float4 rampColor = rampTex.Sample(rampTexSampler, rampCoord);

	float3 finalColor = baseColor.rgb * (radiance + (rampColor.rgb * rampColor.a) * min(fadedSubsurfaceMFP, baseColor.xyz));

	// Rim light
	float3 rimLighting = EndfieldRimLighting(vertNov, vol, vertNol, 1.0, 1.0, materialParams.x);
	rimLighting *= materialParams.y;

	finalColor += rimLighting;
	finalColor *= dirLightColor.xyz * dirLightColor.w * illuminanceCorrect;

	return float4(finalColor, baseColor.a);
}
