// Yvonne Face 材质：照搬参考项目 Shaders/Toon/EndfieldFace_FS.hlsl。
// 5 纹理 albedo + sdf + ramp + mask + highlightMask；SDF 光照 + 唇高光。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ColorConversion.hlsl"
#include "EndfieldCommon.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D sdfTex : register(t1);
SamplerState sdfTexSampler : register(s1);
Texture2D rampTex : register(t2);
SamplerState rampTexSampler : register(s2);
Texture2D maskTex : register(t3);
SamplerState maskTexSampler : register(s3);
Texture2D highlightMaskTex : register(t4);
SamplerState highlightMaskTexSampler : register(s4);

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	static const float illuminanceCorrect = 0.5;

	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float4 mask = maskTex.Sample(maskTexSampler, input.texCoord0);
	float3 view = normalize(cameraPosition - input.worldPosition);
	float vertNov = dot(input.worldNormal, view);
	float vol = dot(-view, dirLightDirection.xyz);
	float vertNol = dot(input.worldNormal, dirLightDirection.xyz);

	// Project light dir into plane perpendicular to objectUp.
	float3 projectedLightDir = ProjectDirection(dirLightDirection.xyz, objectUp.xyz);
	float rol = dot(projectedLightDir.xyz, objectRight.xyz);
	float2 sdfCoord = input.texCoord0;
	if (rol > 0.0) {
		sdfCoord.x = 1.0 - sdfCoord.x;
	}
	float4 sdf = sdfTex.Sample(sdfTexSampler, sdfCoord);

	float fol = dot(dirLightDirection.xyz, objectForward.xyz);
	float normalizedlightAngle = atan2(rol, -fol) * (invPi);
	float lightThreadshold = abs(normalizedlightAngle);

	// Smooth method:
	float faceLightAngle = Unorm(((sdf.x + sdf.y) * 0.5) - abs(normalizedlightAngle));
	float faceRadiance = smoothstep(0.5, 0.6, faceLightAngle);
	faceRadiance = lerp(faceRadiance, max(dot(input.worldNormal, dirLightDirection.xyz), 0.0), mask.y);

	// view attenuation
	float viewFactor = lerp(1.0, max(vertNov, 0.0), materialParams.w * mask.x);
	faceRadiance *= viewFactor;
	float fadedSubsurfaceMFP = materialParams.z * (0.5 + viewFactor * 0.5);

	// Color ramp
	static const float rampOffset = 0.2;
	float2 rampCoord = float2(clamp((1.0 - rampOffset) - faceRadiance * (0.5 - (rampOffset * 0.5)), 0.1, 0.9), 0.5);
	float4 rampColor = rampTex.Sample(rampTexSampler, rampCoord);

	float3 finalColor = baseColor.rgb * (faceRadiance + (rampColor.rgb * rampColor.a) * min(fadedSubsurfaceMFP, baseColor.xyz));

	// Nose color
	finalColor = lerp(finalColor * 0.5, finalColor, baseColor.a);

	// Face rim lighting
	float3 rimLighting = EndfieldRimLighting(vertNov, vol, vertNol, 1.0, mask.w, materialParams.x);
	float rimLightMask = smoothstep(0.0, 0.01, max(Snorm(input.texCoord0.x) * rol, 0.0));
	rimLightMask *= materialParams.y;

	// Lip highlight
	float4 highlightMask = highlightMaskTex.Sample(highlightMaskTexSampler, float2(input.texCoord0.x - rol * 0.025, input.texCoord0.y));
	float lipHighlihgt = highlightMask.r * faceRadiance;

	finalColor += rimLighting * rimLightMask;
	finalColor *= dirLightColor.xyz * dirLightColor.w * illuminanceCorrect;
	finalColor += lipHighlihgt;

	return float4(finalColor, baseColor.a);
}
