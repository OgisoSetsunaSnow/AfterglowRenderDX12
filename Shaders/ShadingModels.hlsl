// Ported 1:1 from reference project ShadingModels.hlsl (DefaultShading).
// D3D12 adaptation: dirLight direction/color passed as function args (reference used global push constants).
#ifndef SHADING_MODELS_HLSL
#define SHADING_MODELS_HLSL

#include "BxDF.hlsl"
#include "ShadingEnergyConservation.hlsl"
#include "ColorConversion.hlsl"

struct ShadingContext {
	float3 baseColor;
	float metallic;
	float specular;
	float roughness;
	float ambientOcclusion;
	float anisotropy;
	float3 normal;
	float3 tangent;
	float3 bitangent;
	float3 view;
	float3 subsurfaceColor;
	float opacity;
};

struct LightingResult {
	float3 diffuse;
	float3 specular;
	float3 transmission;
};

float3 SpecularGGX(in BxDFContext bxdfContext, float roughness, float anisotropy, float3 specularColor) {
	float alpha = roughness * roughness;
	float2 anisotropicAlpha = AnisotropicSquaredRoughness(alpha, anisotropy);

	float3 distribution = DistributionAnisotropicGGX(
		anisotropicAlpha.x,
		anisotropicAlpha.y,
		bxdfContext.noh,
		bxdfContext.xoh,
		bxdfContext.yoh
	);

	float3 visibility = VisibilityAnisotropicSmithJoint(
		anisotropicAlpha.x,
		anisotropicAlpha.y,
		bxdfContext.nov,
		bxdfContext.nol,
		bxdfContext.xov,
		bxdfContext.xol,
		bxdfContext.yov,
		bxdfContext.yol
	);

	float3 fresnel = FresnelSchlick(specularColor, bxdfContext.voh);

	return distribution * visibility * fresnel;
}

float3 SpecularGGX(in BxDFContext bxdfContext, float roughness, float3 specularColor) {
	float alpha2 = Pow4(roughness);

	float distribution = DistributionGGX(alpha2, bxdfContext.noh);
	float visibility = VisibilitySmithJointApprox(alpha2, bxdfContext.nov, bxdfContext.nol);
	float3 fresnel = FresnelSchlick(specularColor, bxdfContext.voh);

	return distribution * visibility * fresnel;
}

LightingResult EnvLighting(float3 reflectionVector, float3 baseColor, float3 specularColor, float nov, float roughness, float metallic,
	Texture2D ambientTex, SamplerState ambientSamp) {
	LightingResult lighting;

	float normalizePhi = (atan2(reflectionVector.y, reflectionVector.x)) * (0.5 * invPi) + 0.5;
	float normalizeTheta = acos(reflectionVector.z) * invPi;

	float textureWidth = 0;
	float textureHeight = 0;
	float textureNumMipMaps = 0;
	ambientTex.GetDimensions(0, textureWidth, textureHeight, textureNumMipMaps);

	float envMipLevel = clamp(sqrt(roughness) * 10, 0, textureNumMipMaps - 1);
	float3 envColor = ambientTex.SampleLevel(ambientSamp, float2(normalizePhi, normalizeTheta), envMipLevel).xyz;
	// 照搬参考项目 ShadingModels.EnvLighting：specular = envColor * EnvBRDFApprox。
	lighting.specular = envColor * EnvBRDFApprox(specularColor, roughness, nov);

	// TEMP Method（参考项目原样）。
	lighting.diffuse = max(1 - metallic, 0) * baseColor * 0.08f;

	return lighting;
}

#define __DEFALUT_LIT_BXDF_BODY(initBxDFExpression, specularGGXExpression) \
	LightingResult lighting; \
	lighting.transmission = 0; \
	\
	BxDFContext bxdfContext; \
	initBxDFExpression; \
	\
	bxdfContext.nol = saturate(bxdfContext.nol); \
	bxdfContext.nov = saturate(abs(bxdfContext.nov) + 1e-6); \
	\
	lighting.diffuse = DiffuseBurley( \
		shadingContext.baseColor, shadingContext.roughness, bxdfContext.nov, bxdfContext.nol, bxdfContext.voh \
	) * bxdfContext.nol; \
	\
	lighting.specular = specularGGXExpression * bxdfContext.nol; \
	\
	BxDFEnergyTermsF3 energyTerms = ComputeGGXSpecularEnergyTerms(shadingContext.roughness, bxdfContext.nov, specularColor); \
	lighting.diffuse *= ComputeEnergyPreservation(energyTerms); \
	lighting.specular *= ComputeEnergyConservation(energyTerms); \
	\
	return lighting;

LightingResult DefaultLitBxDF(in ShadingContext shadingContext, float3 lightDirection, float3 specularColor) {
	__DEFALUT_LIT_BXDF_BODY(
		InitBxDFContext(bxdfContext, shadingContext.normal, shadingContext.view, lightDirection),
		SpecularGGX(bxdfContext, shadingContext.roughness, specularColor)
	)
}

LightingResult DefaultLitBxDFAnisotropic(in ShadingContext shadingContext, float3 lightDirection, float3 specularColor) {
	__DEFALUT_LIT_BXDF_BODY(
		InitBxDFContext(
			bxdfContext,
			shadingContext.normal,
			shadingContext.tangent,
			shadingContext.bitangent,
			shadingContext.view,
			lightDirection
		),
		SpecularGGX(bxdfContext, shadingContext.roughness, shadingContext.anisotropy, specularColor)
	)
}

LightingResult DefaultShading(in ShadingContext shadingContext,
	float3 dirLightDirection, float4 dirLightColor, Texture2D ambientTex, SamplerState ambientSamp) {
	LightingResult lighting = (LightingResult)0;

	float3 specularColor = ComputeF0(shadingContext.specular, shadingContext.baseColor, shadingContext.metallic);

	float3 directionalLightScale = dirLightColor.xyz * dirLightColor.w;
	LightingResult directionalLighting = DefaultLitBxDF(shadingContext, dirLightDirection, specularColor);

	lighting.diffuse += directionalLighting.diffuse * directionalLightScale;
	lighting.specular += directionalLighting.specular * directionalLightScale;
	lighting.transmission += directionalLighting.transmission * directionalLightScale;

	float3 reflectionVector = reflect(-shadingContext.view, shadingContext.normal);
	LightingResult envLighting = EnvLighting(
		reflectionVector,
		shadingContext.baseColor,
		specularColor,
		dot(shadingContext.normal, shadingContext.view),
		shadingContext.roughness,
		shadingContext.metallic,
		ambientTex, ambientSamp
	);
	lighting.diffuse += envLighting.diffuse * shadingContext.ambientOcclusion;
	lighting.specular += envLighting.specular * shadingContext.ambientOcclusion;

	return lighting;
}

// Fast subsurface（参考项目 ShadingModels __SUBSURFACE_BXDF_BODY）。
// 仅实现 Anisotropic 版本（DefaultLit_FS 用）。
LightingResult SubsurfaceLitBxDFAnisotropic(in ShadingContext shadingContext, float3 lightDirection, float3 specularColor) {
	LightingResult lighting = DefaultLitBxDFAnisotropic(shadingContext, lightDirection, specularColor);

	// Effect when you see through the material.
	float inScatter = pow(saturate(dot(lightDirection, -shadingContext.view)), 12) * lerp(3, 0.1f, shadingContext.opacity);

	// Warp around lighting.
	const float warppedDiffuse = pow(saturate(dot(shadingContext.normal, lightDirection) * (1.0f / 1.5f) + (0.5f / 1.5f)), 1.5f) * (2.5f / 1.5f);
	const float normalContribution = lerp(1.0f, warppedDiffuse, shadingContext.opacity);
	const float backScatter = shadingContext.ambientOcclusion * normalContribution / (pi * 2);

	// Transmission
	const float3 extinctionCoefficients = TransmittanceToExtinction(shadingContext.subsurfaceColor, 0.515f);
	const float3 rawTransmittedColor = ExtinctionToTransmittance(extinctionCoefficients, 1.0f);
	const float3 transmittedColor = HSVToLinearRGB(float3(LinearRGBToHSV(rawTransmittedColor).xy, LinearRGBToHSV(shadingContext.subsurfaceColor).z));

	lighting.transmission = lerp(backScatter, 1, inScatter) * transmittedColor;

	return lighting;
}

// SubsurfaceShading（参考项目 SubsurfaceShadingAnisotropic，__SHADING_BODY + SubsurfaceBxDFAnisotropic）。
LightingResult SubsurfaceShadingAnisotropic(in ShadingContext shadingContext,
	float3 dirLightDirection, float4 dirLightColor, Texture2D ambientTex, SamplerState ambientSamp) {
	LightingResult lighting = (LightingResult)0;

	float3 specularColor = ComputeF0(shadingContext.specular, shadingContext.baseColor, shadingContext.metallic);

	float3 directionalLightScale = dirLightColor.xyz * dirLightColor.w;
	LightingResult directionalLighting = SubsurfaceLitBxDFAnisotropic(shadingContext, dirLightDirection, specularColor);

	lighting.diffuse += directionalLighting.diffuse * directionalLightScale;
	lighting.specular += directionalLighting.specular * directionalLightScale;
	lighting.transmission += directionalLighting.transmission * directionalLightScale;

	float3 reflectionVector = reflect(-shadingContext.view, shadingContext.normal);
	LightingResult envLighting = EnvLighting(
		reflectionVector,
		shadingContext.baseColor,
		specularColor,
		dot(shadingContext.normal, shadingContext.view),
		shadingContext.roughness,
		shadingContext.metallic,
		ambientTex, ambientSamp
	);
	lighting.diffuse += envLighting.diffuse * shadingContext.ambientOcclusion;
	lighting.specular += envLighting.specular * shadingContext.ambientOcclusion;

	return lighting;
}

// Anisotropic 版本（参考项目 DefaultShadingAnisotropic，用 DefaultLitBxDFAnisotropic）。
LightingResult DefaultShadingAnisotropic(in ShadingContext shadingContext,
	float3 dirLightDirection, float4 dirLightColor, Texture2D ambientTex, SamplerState ambientSamp) {
	LightingResult lighting = (LightingResult)0;

	float3 specularColor = ComputeF0(shadingContext.specular, shadingContext.baseColor, shadingContext.metallic);

	float3 directionalLightScale = dirLightColor.xyz * dirLightColor.w;
	LightingResult directionalLighting = DefaultLitBxDFAnisotropic(shadingContext, dirLightDirection, specularColor);

	lighting.diffuse += directionalLighting.diffuse * directionalLightScale;
	lighting.specular += directionalLighting.specular * directionalLightScale;
	lighting.transmission += directionalLighting.transmission * directionalLightScale;

	float3 reflectionVector = reflect(-shadingContext.view, shadingContext.normal);
	LightingResult envLighting = EnvLighting(
		reflectionVector,
		shadingContext.baseColor,
		specularColor,
		dot(shadingContext.normal, shadingContext.view),
		shadingContext.roughness,
		shadingContext.metallic,
		ambientTex, ambientSamp
	);
	lighting.diffuse += envLighting.diffuse * shadingContext.ambientOcclusion;
	lighting.specular += envLighting.specular * shadingContext.ambientOcclusion;

	return lighting;
}

#endif
