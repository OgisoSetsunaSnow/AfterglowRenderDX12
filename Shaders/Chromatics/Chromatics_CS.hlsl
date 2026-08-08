// Chromatics：一次性烘焙 32^3 3D LUT（照搬参考 Chromatics/Chromatics_CS.hlsl）。
// 参考项目 ACES 表来自 externalSSBOs（C++ initMode=InternalFunction 生成）；
// D3D12 用 StructuredBuffer 声明 + ComputePass 绑定（t0-t3 表，u0 3D LUT UAV）。
// 注意：ACESOutParams 等资源声明必须在 include ACESOutputTransform.hlsl 之前。

// 常量 + LogToLinearColor（照搬 Chromatics/ChromaticsCommon.hlsl 所需部分，避免 include 其 LUTMapping 用到 PostProcess 的 ColorLUTIn）。
static const uint lutSideLength = 32;
static const float lutInvSideLength = 1.0 / lutSideLength;
static const float linearColorRange = 14.0;
static const float linearColorGrey = 0.18;
static const float colorExposureGrey = 444.0;

// 从 GammaCorrectionCommon.ush（Unreal Engine）。
float3 LogToLinearColor(float3 logColor) {
	// The maximum linear color value is about 43.708.
	// 1.0(log) -> 43.708 (linear)
	return exp2((logColor - colorExposureGrey / 1023.0) * linearColorRange) * linearColorGrey;
}

// ---- ACES 表（参考项目由 C++ aces::OutputTransformContext 生成，见 src/ACES.h）----
struct OutputTransformParamsSSBO {
	float peakLuminance;
	float limitJMax;
	float invModelGamma;
	float saturation;
	float saturationThreshold;
	float chromaCompression;
	float chromaCompressionScale;
	float midJ;
	float focusDistance; // 参考 elementLayout 字段名（C++ 为 focusDistiance，offset 对齐）
	float invLowerHullGamma;
	int2 hueLinearitySearchRange;
	// Tonescale params
	float tonescale_n_r;
	float tonescale_g;
	float tonescale_t_1;
	float tonescale_c_t;
	float tonescale_s_2;
	float tonescale_u_2;
	float tonescale_m_2;
	float tonescale_forwardLimit;
	float tonescale_inverseLimit;
	// JMh params
	float jmh_F_L_n;
	float jmh_cz;
	float jmh_inv_cz;
	float inputJMh_A_w_j;
	float inputJMh_inv_A_w_j;
	// 矩阵：float[12] raw 数组（3 列 × 4 行，列主序）——与 C++ 端完全一致，避免 HLSL float4 数组歧义
	float inputJMh_rgbToCam16c[12];
	float inputJMh_coneResponseToAab[12];
	float limitJMh_cam16cToRGB[12];
	float limitJMh_aabToConeResponse[12];
};

StructuredBuffer<OutputTransformParamsSSBO> ACESOutParams : register(t0);
StructuredBuffer<float4> ACESUpperHullGammaTable : register(t1);
StructuredBuffer<float4> ACESReachMTable : register(t2);
StructuredBuffer<float4> ACESGamutCuspsTable : register(t3);
RWTexture3D<float4> ColorLUTOut : register(u0);

// 从 raw 12-float 数组（3 列 × 4 行，列主序）构造 float3x3（转置语义）。
float3x3 Float4x3ToFloat3x3_Raw(float m[12]) {
	return float3x3(float3(m[0], m[4], m[8]), float3(m[1], m[5], m[9]), float3(m[2], m[6], m[10]));
}

#include "ACESOutputTransform.hlsl"

[numthreads(8, 8, 8)]
void main(uint3 threadID : SV_DispatchThreadID) {
	float4 neutral = float4(lutInvSideLength * threadID, 1.0);

	// Decode log value
	// Working color space: D60-ACEScg (AP1)
	float3 linearColor = LogToLinearColor(neutral.rgb);

	// Bake ACES transform into a LUT
	float3 outputColor = OutputTransformFoward(linearColor);

	ColorLUTOut[threadID].xyz = outputColor;
}
