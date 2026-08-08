// Ported 1:1 from reference project ShadingEnergyConservation.hlsl (functions used by DefaultShading).
#ifndef SHADING_ENERGY_CONSERVATION_HLSL
#define SHADING_ENERGY_CONSERVATION_HLSL

#include "Common.hlsl"
#include "ShadingCommon.hlsl"

struct BxDFEnergyTermsF3 {
	float3 w; // Overall weight to scale the lobe BxDF to ensure energy conservation.
	float3 e; // Directional albedo of the lobe for energy preservation and lobe picking.
};

float GetEnergyLuminance(float inEnergy) {
	return inEnergy;
}

float GetEnergyLuminance(float3 inEnergy) {
	return dot(inEnergy, float3(0.299, 0.587, 0.114));
}

float2 GGXEnergyLookup(float roughness, float nov) {
	const float energy = 1.0 - saturate(pow(roughness, nov / roughness) * ((roughness * nov + 0.0266916) / (0.466495 + nov)));
	const float energyFresnel = Pow5(1.0 - nov) * pow(2.36651 * pow(nov, max(4.7703 * roughness, 1.0e-12)) + 0.0387332, roughness);
	return float2(energy, energyFresnel);
}

BxDFEnergyTermsF3 ComputeFresnelEnergyTerms(float2 energy, float3 f0, float3 f90) {
	BxDFEnergyTermsF3 result;
	result.w = 1.0 + f0 * ((1.0 - energy.x) / energy.x);
	result.e = result.w * (energy.x * f0 + energy.y * (f90 - f0));
	return result;
}

BxDFEnergyTermsF3 ComputeGGXSpecularEnergyTerms(float roughness, float nov, float3 f0, float3 f90) {
	return ComputeFresnelEnergyTerms(GGXEnergyLookup(roughness, nov), f0, f90);
}

BxDFEnergyTermsF3 ComputeGGXSpecularEnergyTerms(float roughness, float nov, float3 f0) {
	const float f90 = F0RGBToMircoOcclusion(f0);
	return ComputeGGXSpecularEnergyTerms(roughness, nov, f0, f90);
}

// Energy absorbed by upper layer (specular) -> attenuation onto diffuse.
float3 ComputeEnergyPreservation(BxDFEnergyTermsF3 energyTerms) {
	return 1.0 - GetEnergyLuminance(energyTerms.e);
}

// Multiple-scattering energy conservation weight.
float3 ComputeEnergyConservation(BxDFEnergyTermsF3 energyTerms) {
	return energyTerms.w;
}

#endif
