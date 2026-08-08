// Yvonne Cloth/Tail/ThinFilm 材质：照搬参考项目 Shaders/Toon/EndfieldPBR_FS.hlsl。
// albedo + normal + property(+mask+ramp 薄膜干涉) + rim + DefaultShading PBR。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ColorConversion.hlsl"
#include "ShadingModels.hlsl"
#include "EndfieldCommon.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D normalTex : register(t1);
SamplerState normalTexSampler : register(s1);
Texture2D propertyTex : register(t2);
SamplerState propertyTexSampler : register(s2);
Texture2D maskTex : register(t3);
SamplerState maskTexSampler : register(s3);
Texture2D rampTex : register(t4);
SamplerState rampTexSampler : register(s4);

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float4 property = propertyTex.Sample(propertyTexSampler, input.texCoord0);

	float3x3 tbn = {
		input.worldTangent,
		input.worldBitangent,
		input.worldNormal
	};
	tbn = isFrontFace ? tbn : -tbn;
	float3 normal = mul(LoadNormalTextureFromXY(normalTex, normalTexSampler, input.texCoord0), tbn);
	float3 view = normalize(cameraPosition - input.worldPosition);

	// Thin film interference
	float4 rampColor = 0.0;
	float4 mask = maskTex.Sample(maskTexSampler, input.texCoord0);
	if (mask.x > 0.0) {
		float rampCoord = clamp(dot(view, normal), 0.0, 1.0);
		rampColor = rampTex.Sample(rampTexSampler, float2(rampCoord, 0.5));
		baseColor.xyz = rampColor.xyz;
	}

	// Rim light
	float3 vertNormal = isFrontFace ? input.worldNormal : -input.worldNormal;
	float3 rimLighting = EndfieldRimLighting(vertNormal, view, property.w, property.z, materialParams.x);

	ShadingContext shadingContext = (ShadingContext)0;
	shadingContext.baseColor = baseColor.xyz * property.z;
	shadingContext.metallic = property.x + rampColor;
	shadingContext.specular = 0.5;
	shadingContext.roughness = 1.0 - property.w;
	shadingContext.ambientOcclusion = property.z;
	shadingContext.normal = normal;
	shadingContext.view = view;

	LightingResult lightingResult = DefaultShading(shadingContext,
		normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, ambientTextureSampler);
	float3 finalColor = lightingResult.diffuse + lightingResult.specular + lightingResult.transmission;
	finalColor += rimLighting;

	return float4(finalColor, baseColor.a);
}
