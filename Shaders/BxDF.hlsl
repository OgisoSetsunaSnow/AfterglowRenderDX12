// Ported 1:1 from reference project BxDF.hlsl (From BRDF.ush, Unreal Engine).
#ifndef BXDF_HLSL
#define BXDF_HLSL

#include "Common.hlsl"
#include "Constants.hlsl"

struct BxDFContext {
	float nov;
	float nol;
	float vol;
	float noh;
	float voh;
	float xov;
	float xol;
	float xoh;
	float yov;
	float yol;
	float yoh;
};

static const float bxdfTolerance = 1.0e-10;

void InitBxDFContext(inout BxDFContext context, float3 normal, float3 view, float3 light) {
	context.nol = dot(normal, light);
	context.nov = dot(normal, view);
	context.vol = clamp(dot(view, light), -1, 1);
	float invLenH = rsqrt(2 + 2 * context.vol);
	context.noh = saturate((context.nol + context.nov) * invLenH);
	context.voh = saturate(invLenH + invLenH * context.vol);

	context.xov = 0.0f;
	context.xol = 0.0f;
	context.xoh = 0.0f;
	context.yov = 0.0f;
	context.yol = 0.0f;
	context.yoh = 0.0f;
}

void InitBxDFContext(inout BxDFContext context, float3 normal, float3 tangent, float3 bitangent, float3 view, float3 light) {
	context.nol = dot(normal, light);
	context.nov = dot(normal, view);
	context.vol = clamp(dot(view, light), -1, 1);
	float invLenH = rsqrt(2 + 2 * context.vol);
	context.noh = saturate((context.nol + context.nov) * invLenH);
	context.voh = saturate(invLenH + invLenH * context.vol);

	context.xov = dot(tangent, view);
	context.xol = dot(tangent, light);
	context.xoh = (context.xov + context.xol) * invLenH;
	context.yov = dot(bitangent, view);
	context.yol = dot(bitangent, light);
	context.yoh = (context.yov + context.yol) * invLenH;

	float3 hTangent = normalize(float3(context.xoh, context.yoh, context.noh));
	context.xoh = hTangent.x;
	context.yoh = hTangent.y;
	context.noh = hTangent.z;
}

float3 DiffuseLambert(float3 diffuseColor) {
	return diffuseColor * invPi;
}

// [Burley 2012, "Physically-Based Shading at Disney"]
float3 DiffuseBurley(float3 diffuseColor, float roughness, float nov, float nol, float voh) {
	float fd90 = 0.5 + 2 * voh * voh * roughness;
	float fdv = 1 + (fd90 - 1) * Pow5(1 - nov);
	float fdl = 1 + (fd90 - 1) * Pow5(1 - nol);
	return diffuseColor * (invPi * fdv * fdl);
}

float DistributionBlinn(float a2, float noh) {
	float n = 2 / a2 - 2;
	return (n + 2) / (2 * pi) * pow(noh, n);
}

float DistributionBeckmann(float a2, float noh) {
	float noh2 = noh * noh;
	return exp((noh2 - 1) / (a2 * noh2)) / (pi * a2 * noh2 * noh2);
}

float DistributionGGX(float a2, float noh) {
	float d = (noh * a2 - noh) * noh + 1;
	return a2 / (pi * d * d + bxdfTolerance);
}

float DistributionAnisotropicGGX(float ax, float ay, float noh, float xoh, float yoh) {
	float a2 = ax * ay;
	float3 v = float3(ay * xoh, ax * yoh, a2 * noh);
	float s = max(dot(v, v), bxdfTolerance);
	return invPi * a2 * Square(a2 / s);
}

float VisibilitySchlick(float a2, float nov, float nol) {
	float k = sqrt(a2) * 0.5;
	float schlickV = nov * (1 - k) + k;
	float schlickL = nol * (1 - k) + k;
	return 0.25 / (schlickV * schlickL);
}

float VisibilitySmith(float a2, float nov, float nol) {
	float smithV = nov + sqrt(nov * (nov - nov * a2) + a2);
	float smithL = nol + sqrt(nol * (nol - nol * a2) + a2);
	return rcp(smithV * smithL);
}

float VisibilitySmithJointApprox(float a2, float nov, float nol) {
	float a = sqrt(a2);
	float smithV = nol * (nov * (1 - a) + a);
	float smithL = nov * (nol * (1 - a) + a);
	return 0.5 * rcp(smithV + smithL + bxdfTolerance);
}

float VisibilitySmithJoint(float a2, float nov, float nol) {
	float smithV = nol * sqrt(nov * (nov - nov * a2) + a2);
	float smithL = nov * sqrt(nol * (nol - nol * a2) + a2);
	return 0.5 * rcp(smithV + smithL + bxdfTolerance);
}

float VisibilityAnisotropicSmithJoint(float ax, float ay, float nov, float nol, float xov, float xol, float yov, float yol) {
	float smithV = nol * length(float3(ax * xov, ay * yov, nov));
	float smithL = nov * length(float3(ax * xol, ay * yol, nol));
	return 0.5 * rcp(smithV + smithL + bxdfTolerance);
}

float3 FresnelSchlick(float3 specularColor, float voh) {
	float fc = Pow5(1 - voh);
	return saturate(50.0 * specularColor.g) * fc + (1 - fc) * specularColor;
}

float3 FresnelSchlick(float f0, float3 f90, float voh) {
	float fc = Pow5(1 - voh);
	return f90 * fc + (1 - fc) * f0;
}

float2 EnvBRDFApproxLazarov(float roughness, float nov) {
	const float4 c0 = { -1, -0.0275, -0.572, 0.022 };
	const float4 c1 = { 1, 0.0425, 1.04, -0.04 };
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * nov)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return ab;
}

float3 EnvBRDFApprox(float3 specularColor, float roughness, float nov) {
	float2 ab = EnvBRDFApproxLazarov(roughness, nov);
	float f90 = saturate(50.0 * specularColor.g);
	return specularColor * ab.x + f90 * ab.y;
}

float3 EnvBRDFApprox(float3 f0, float3 f90, float roughness, float nov) {
	float2 ab = EnvBRDFApproxLazarov(roughness, nov);
	return f0 * ab.x + f90 * ab.y;
}

float2 AnisotropicSquaredRoughness(float alpha, float anisotropy) {
	float2 anisotropicRoughness;
	anisotropicRoughness.x = max(alpha * (1.0 + anisotropy), 0.001f);
	anisotropicRoughness.y = max(alpha * (1.0 - anisotropy), 0.001f);
	return anisotropicRoughness;
}

float2 AnisotropicRoughness(float roughness, float anisotropy) {
	float2 anisotropicRoughness = saturate(roughness);
	anisotropy = clamp(anisotropy, -1.0, 1.0);
	anisotropicRoughness.x = max(roughness * sqrt(1.0 + anisotropy), 0.001f);
	anisotropicRoughness.y = max(roughness * sqrt(1.0 - anisotropy), 0.001f);
	return anisotropicRoughness;
}

// ---- Participating media (SSS transmission), ported from reference BxDF.hlsl ----
static const float participatingMediaMinMFPMeter = 1e-12f;
static const float participatingMediaMinTransmittance = 1e-12f;

float3 TransmittanceToExtinction(in float3 transmittanceColor, in float thicknessMeters) {
	// TransmisstanceColor = exp(-extinction * thickness)
	// Extinction = -log(transitionColor) / thickness
	return -log(clamp(transmittanceColor, participatingMediaMinTransmittance, 1.0f)) / max(participatingMediaMinMFPMeter, thicknessMeters);
}

float3 ExtinctionToTransmittance(in float3 extinction, in float thicknessMeters) {
	return exp(-extinction * thicknessMeters);
}

#endif
