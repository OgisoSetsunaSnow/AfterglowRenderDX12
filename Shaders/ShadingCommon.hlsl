// Ported 1:1 from reference project ShadingCommon.hlsl.
#ifndef SHADING_COMMON_HLSL
#define SHADING_COMMON_HLSL

float F0RGBToF0(float3 f0) {
	return max(f0.r, max(f0.g, f0.b));
}

float F0ToMircoOcclusion(float f0) {
	return saturate(50.0 * f0);
}

float F0RGBToMircoOcclusion(float3 f0) {
	return F0ToMircoOcclusion(F0RGBToF0(f0));
}

float DielectricSpecularToF0(float specular) {
	return 0.08f * specular;
}

float3 ComputeF0(float specular, float3 baseColor, float metallic) {
	return lerp(DielectricSpecularToF0(specular).xxx, baseColor, metallic);
}

#endif
