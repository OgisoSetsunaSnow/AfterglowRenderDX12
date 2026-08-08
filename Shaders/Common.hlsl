// Common math/vector helpers, ported 1:1 from reference project Common.hlsl (DX12 flavor).
#ifndef COMMON_HLSL
#define COMMON_HLSL

#include "Constants.hlsl"

float Snorm(float v) { return v * 2.0 - 1.0; }
float2 Snorm(float2 v) { return v * 2.0 - 1.0; }
float3 Snorm(float3 v) { return v * 2.0 - 1.0; }
float4 Snorm(float4 v) { return v * 2.0 - 1.0; }

float Unorm(float v) { return v * 0.5 + 0.5; }
float2 Unorm(float2 v) { return v * 0.5 + 0.5; }
float3 Unorm(float3 v) { return v * 0.5 + 0.5; }
float4 Unorm(float4 v) { return v * 0.5 + 0.5; }

float EncodeNormal(float n) { return Unorm(n); }
float3 EncodeNormal(float3 n) { return Unorm(n); }

float DecodeNormal(float n) { return Snorm(n); }
float3 DecodeNormal(float3 n) { return Snorm(n); }

float3 ScaleNormal(float3 n, float intensity) { return normalize(float3(n.xy * intensity, n.z)); }

float3 ReconstructNormal(float2 normalXY) {
	return float3(normalXY, sqrt(max(1.0 - dot(normalXY, normalXY), 0.0)));
}

float3 LoadNormalTexture(Texture2D normalTex, SamplerState texSampler, float2 uv) {
	return Snorm(normalTex.Sample(texSampler, uv).xyz);
}

float3 LoadNormalTextureFromXY(Texture2D normalTex, SamplerState texSampler, float2 uv) {
	return ReconstructNormal(Snorm(normalTex.Sample(texSampler, uv).xy));
}

float3 BlendAngleCorrectedNormals(float3 baseNormal, float3 additionalNormal) {
	float3 n0 = float3(baseNormal.xy, baseNormal.z + 1.0);
	float3 n1 = float3(-additionalNormal.xy, additionalNormal.z);
	return normalize(dot(n0, n1) * n0 - n0.z * n1);
}

float Square(float x) { return x * x; }
float2 Square(float2 x) { return x * x; }
float3 Square(float3 x) { return x * x; }
float4 Square(float4 x) { return x * x; }

float Pow3(float x) { return x * x * x; }
float3 Pow3(float3 x) { return x * x * x; }

float Pow4(float x) { float xx = x * x; return xx * xx; }
float2 Pow4(float2 x) { return x * x * x * x; }
float3 Pow4(float3 x) { return x * x * x * x; }

float Pow5(float x) { float xx = x * x; return xx * xx * x; }
float3 Pow5(float3 x) { return x * x * x * x * x; }

// 参考项目 Common.hlsl：限制向量长度（风向/水流风速用）。
float2 ClampVectorWithLength(float2 v, float maxLen) {
	float sqrLenV = dot(v, v);
	float invLenV = rsqrt(max(sqrLenV, 1e-6));
	return v * min(invLenV * maxLen, 1.0);
}
float3 ClampVectorWithLength(float3 v, float maxLen) {
	float sqrLenV = dot(v, v);
	float invLenV = rsqrt(max(sqrLenV, 1e-6));
	return v * min(invLenV * maxLen, 1.0);
}
float2 ClampVectorWithLength(float2 v, float minLen, float maxLen) {
	float sqrLenV = dot(v, v);
	float invLenV = rsqrt(max(sqrLenV, 1e-6));
	return v * max(min(invLenV * maxLen, 1.0), invLenV * minLen);
}

// @return: vector which is projected on the plane.
float3 ProjectVector(float3 vec, float3 planeNormal) {
	planeNormal = normalize(planeNormal);
	return vec - dot(vec, planeNormal) * planeNormal;
}

// @return: direction(normalized) which is projected on the plane.
float3 ProjectDirection(float3 vec, float3 planeNormal) {
	return normalize(vec - dot(vec, planeNormal) * planeNormal);
}

// ---- ACES/Chromatics 工具（照搬参考项目 Common.hlsl，供 ShadingModels/Chromatics 用）----
float SafePow(float base, float exponent) {
	return (base < 0.0 && exponent != floor(exponent)) ? 0.0 : pow(base, exponent);
}
float2 SafePow(float2 base, float2 exponent) {
	return float2(SafePow(base[0], exponent[0]), SafePow(base[1], exponent[1]));
}
float3 SafePow(float3 base, float3 exponent) {
	return float3(SafePow(base[0], exponent[0]), SafePow(base[1], exponent[1]), SafePow(base[2], exponent[2]));
}
float4 SafePow(float4 base, float4 exponent) {
	return float4(SafePow(base[0], exponent[0]), SafePow(base[1], exponent[1]), SafePow(base[2], exponent[2]), SafePow(base[3], exponent[3]));
}

// @return: x with y's sign.
template<typename Type>
Type CopySign(Type x, Type y) {
	return sign(y) * abs(x);
}

template<typename Type>
Type WrapTo360(Type x) {
	float y = fmod(x, 360.0);
	if (y < 0.0) {
		y += 360.0;
	}
	return y;
}

template<typename Type>
Type MidPoint(Type a, Type b) {
	return (a + b) * 0.5;
}

float3x3 Float4x3ToFloat3x3(float4x3 mat) {
	return float3x3(mat[0].xyz, mat[1].xyz, mat[2].xyz);
}

#endif
