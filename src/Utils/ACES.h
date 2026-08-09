// ACES2 OutputTransform 参数与查找表生成（照搬参考项目 ACESCommon.h/ACESUtilities.h/ACESOutputDevice.h，
// 供 Chromatics_CS 烘焙 32^3 3D LUT 使用）。唯一改动：gcem constexpr 数学 → std:: 运行时；异常用 std::runtime_error。
#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace aces {
	using float2 = glm::vec2;
	using float3 = glm::vec3;
	using float4 = glm::vec4;
	using float2x2 = glm::mat2x2;
	using float3x3 = glm::mat3x3;
	using float3x4 = glm::mat3x4;
	using float4x4 = glm::mat4x4;
	using int2 = glm::ivec2;
	using int3 = glm::ivec3;
	using int4 = glm::ivec4;

	constexpr float pi = 3.14159265359f;

	// ---- 色度坐标（照搬 ACESCommon.h）----
	using Primary = float2;
	constexpr Primary whiteCIE1931_D65 = { 0.3127f, 0.3290f };
	constexpr Primary whiteACES_D60 = { 0.32168f, 0.33767f };
	constexpr Primary whiteDCI_Calibration = { 0.314f, 0.351f };
	constexpr Primary whiteCAM16 = { 0.333f, 0.333f };

	struct Chromaticities {
		Primary red;
		Primary green;
		Primary blue;
		Primary white;
		float3x3 makeRGBToXYZMat() const;
	};

	constexpr Chromaticities chromaticitiesACES2065_1{ { 0.73470f, 0.26530f }, { 0.00000f, 1.00000f }, { 0.00010f, -0.07700f }, whiteACES_D60 };
	constexpr Chromaticities chromaticitiesACEScg{ { 0.713f, 0.293f }, { 0.165f, 0.830f }, { 0.128f, 0.044f }, whiteACES_D60 };
	constexpr Chromaticities chromaticitiesCAM16{ { 0.8336f, 0.1735f }, { 2.3854f, -1.4659f }, { 0.087f, -0.125f }, whiteCAM16 };
	constexpr Chromaticities chromaticitiesSRGB{ { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, whiteCIE1931_D65 };
	constexpr Chromaticities chromaticitiesRec2020{ { 0.708f, 0.292f }, { 0.170f, 0.797f }, { 0.131f, 0.046f }, whiteCIE1931_D65 };
	constexpr Chromaticities chromaticitiesP3_DCI{ { 0.680f, 0.320f }, { 0.265f, 0.690f }, { 0.150f, 0.060f }, whiteDCI_Calibration };
	constexpr Chromaticities chromaticitiesP3_D65{ { 0.680f, 0.320f }, { 0.265f, 0.690f }, { 0.150f, 0.060f }, whiteCIE1931_D65 };

	// ---- 输出设备（照搬 ACESOutputDevice.h）----
	constexpr float outputDevicePeakLuminance = 100.0f; // nits
	constexpr Chromaticities OutputDeviceChromaticities() { return chromaticitiesSRGB; }

	// ---- 表常量（照搬 ACESUtilities.h）----
	constexpr float hueLimit = 360.0f;
	constexpr uint32_t tableSize = 360;
	constexpr uint32_t paddedTableSize = tableSize + 2;
	constexpr uint32_t cuspCornerCount = 6;
	constexpr uint32_t paddedCuspCornerCount = cuspCornerCount + 2;
	constexpr uint32_t maxSortedCornerCount = cuspCornerCount * 2;
	constexpr uint32_t upperHullGammaTestCount = 5;

	using HueTable = std::array<float, paddedTableSize>;
	using ReachMTable = std::array<float, paddedTableSize>;
	using UpperHullGammaTable = std::array<float, paddedTableSize>;
	using GamutCuspsTable = std::array<float4, paddedTableSize>;
	using CuspCorners = std::array<float3, paddedCuspCornerCount>;
	using SortedCornerHues = std::array<float, maxSortedCornerCount>;

	// ---- CAM/压缩常量（gcem → std:: 运行时）----
	constexpr float cam_ref_luminance = 100.0f;
	constexpr float cam_L_A = 100.0f;
	constexpr float cam_Y_b = 20.0f;
	constexpr float3 cam_surround = { 0.9f, 0.59f, 0.9f };
	constexpr float cam_J_scale = 100.0f;
	constexpr float cam_nl_Y_reference = 100.0f;
	constexpr float cam_nl_offset = 0.2713f * cam_nl_Y_reference;
	constexpr float cam_nl_scale = 4.0f * cam_nl_Y_reference;
	inline const float cam_model_gamma = cam_surround.y * (1.48f + std::sqrt(cam_Y_b / cam_ref_luminance));

	// JMhParams 用（参考原为 static constexpr）
	inline const float k = 1.0f / (5.0f * cam_L_A + 1.0f);
	inline const float k4 = k * k * k * k;
	inline const float F_L = 0.2f * k4 * (5.0f * cam_L_A) + 0.1f * std::pow(1.0f - k4, 2.0f) * std::pow(5.0f * cam_L_A, 1.0f / 3.0f);
	inline const float F_L_n = F_L / cam_ref_luminance;
	inline const float jmh_cz = cam_model_gamma;
	inline const float jmh_inv_cz = 1.0f / jmh_cz;

	// Chroma compression
	constexpr float chroma_compress = 2.4f;
	constexpr float chroma_compress_fact = 3.3f;
	constexpr float chroma_expand = 1.3f;
	constexpr float chroma_expand_fact = 0.69f;
	constexpr float chroma_expand_thr = 0.5f;

	// Gamut compression
	constexpr float smooth_cusps = 0.12f;
	constexpr float smooth_m = 0.27f;
	constexpr float cusp_mid_blend = 1.3f;
	constexpr float focus_gain_blend = 0.3f;
	constexpr float focus_adjust_gain = 0.55f;
	constexpr float focus_distance = 1.35f;
	constexpr float focus_distance_scaling = 1.75f;
	constexpr float compression_threshold = 0.75f;

	// ---- 结构（照搬 ACESUtilities.h）----
	struct JMhParams {
		explicit JMhParams(const Chromaticities& chromaticities = OutputDeviceChromaticities());
		float3x3 rgbToCAM16c;
		float3x3 cam16cToRGB;
		float3x3 coneResponseToAab;
		float3x3 aabToConeResponse;
		// 参考为 static constexpr 成员；本工程用命名空间 inline const（gcem→std 运行时）。
		float F_L_n = aces::F_L_n;
		float cz = aces::jmh_cz;
		float inv_cz = aces::jmh_inv_cz;
		float A_w_J;
		float inv_A_w_J;
	};

	struct TonescaleParams {
		explicit TonescaleParams(float inPeakLuminance = outputDevicePeakLuminance);
		float peakLuminance;
		float n_r;
		float g;
		float t_1;
		float c_t;
		float s_2;
		float u_2;
		float m_2;
		float forwardLimit;
		float inverseLimit;
		float logPeak;
	};

	struct OutputTransformParams {
		explicit OutputTransformParams(float inPeakLuminance = outputDevicePeakLuminance,
			const Chromaticities& limitChromaticities = OutputDeviceChromaticities());
		float peakLuminance;
		float limitJMax;
		float invModelGamma;
		float saturation;
		float saturationThreshold;
		float chromaCompression;
		float chromaCompressionScale;
		float midJ;
		float focusDistiance;
		float invLowerHullGamma;
		int2 hueLinearitySearchRange;
		JMhParams inputParams;
		JMhParams reachParams;
		JMhParams limitParams;
		TonescaleParams tonescaleParams;
		std::unique_ptr<ReachMTable> reachMTable;
		std::unique_ptr<GamutCuspsTable> gamutCuspsTable;
		std::unique_ptr<UpperHullGammaTable> upperHullGammaTable;
	};

	// ---- 基础函数（照搬 ACESUtilities.h 声明）----
	float3x3 ScaleMatrixDiagonal(const float3x3& mat, const float3& scale);
	float WrapTo360(float x);
	float BaseHueForPosition(uint32_t index);
	uint32_t HuePositionInUniformTable(float hue, uint32_t size);
	float PostAdaptationConeResponseCompressionForwardImpl(float Rc);
	float PostAdaptationConeResponseCompressionForward(float v);
	float PostAdaptationConeResponseCompressionInverseImpl(float Ra);
	float PostAdaptationConeResponseCompressionInverse(float v);
	float JToAchromaticN(float J, float inv_cz);
	float AchromaticNToJ(float A, float cz);
	float YToJ(float Y, const JMhParams& params);
	float3 AabToJMh(const float3& colorAab, const JMhParams& jmhParams);
	float3 JMhToAab(const float3& colorJMh, const JMhParams& jmhParams);
	float3 RGBToAab(const float3& colorRGB, const JMhParams& jmhParams);
	float3 AabToRGB(const float3& colorAab, const JMhParams& jmhParams);
	float3 RGBToJMh(const float3& colorRGB, const JMhParams& jmhParams);
	float3 JMhToRGB(const float3& colorJMh, const JMhParams& jmhParams);

	// ---- 表生成（照搬 ACESUtilities.h 声明）----
	float3 GenerateUnitCubeCuspCorners(uint32_t cornerIndex);
	void BuildLimitingCuspCorners(CuspCorners& limitingRGBCorners, CuspCorners& limitingJMhCorners,
		const JMhParams& limitParams, float peakLuminance);
	CuspCorners FindReachCorners(const JMhParams& reachParams, float limitJMax, float toneForwardLimit);
	SortedCornerHues ExtractSortedCubeHues(const CuspCorners& reachJMhCorners, const CuspCorners& limitJMhCorners);
	float2 FindDisplayCuspForHue(float hue, const CuspCorners& rgbCorners, const CuspCorners& jmhCorners,
		const JMhParams& jmhParams);
	void BuildHueSampleInterval(HueTable& hueTable, int32_t samples, float lower, float upper, int32_t baseIndex);
	void BuildHueTable(HueTable& hueTable, const SortedCornerHues& sortedHues);
	void BuildCuspsTable(GamutCuspsTable& cuspsTable, const HueTable& hueTable, const CuspCorners& rgbCorners,
		const CuspCorners& jmhCorners, const JMhParams& jmhParams);
	float ComputeFocusJ(float cuspJ, float midJ, float limitJMax);
	float GetFocusGain(float colorJ, float analyticalThreshold, float limitJMax, float focusDistance);
	float SolveIntersectJ(float colorJ, float colorM, float focusJ, float maxJ, float slopeGain);
	float ComputeCompressionVectorSlope(float intersectJ, float focusJ, float limitJMax, float slopeGain);
	float SmoothMinimumScaled(float a, float b, float scaleReference);
	float EstimateLineAndBoundaryIntersectionM(float axisIntersectJ, float slope, float invGamma, float maxJ, float maxM,
		float intersectionReferenceJ);
	float FindGamutBoundaryIntersection(float2 cuspJM, float maxJ, float invGammaTop, float invGammaBottom,
		float intersectSourceJ, float slope, float intersectCuspJ);
	bool EvaluateGammaFit(float2 cuspJM, float invTopGamma, float peakLuminance, float limitJMax, float invLowerHullGamma,
		const std::array<float3, upperHullGammaTestCount>& testJMhs, const std::array<float, upperHullGammaTestCount>& intersectSourceJs,
		const std::array<float, upperHullGammaTestCount>& slopes, const std::array<float, upperHullGammaTestCount>& intersectCuspJs,
		const JMhParams& limitParams);
	void GenerateGammaTestData(const float2 cuspJM, const float hue, const float limitJMax, const float midJ,
		const float focusDistance, std::array<float3, upperHullGammaTestCount>& testJMhs,
		std::array<float, upperHullGammaTestCount>& intersectSourceJs, std::array<float, upperHullGammaTestCount>& slopes,
		std::array<float, upperHullGammaTestCount>& intersectCuspJs);
	std::unique_ptr<ReachMTable> MakeReachMTable(const JMhParams& reachParams, float limitJMax);
	std::unique_ptr<GamutCuspsTable> MakeUniformHueGamutTable(const JMhParams& reachParams, const JMhParams& limitParams,
		float peakLuminance, float limitJMax, float toneForwardLimit);
	std::unique_ptr<UpperHullGammaTable> MakeUpperHullGammaTable(const GamutCuspsTable& gamutCuspsTable,
		const JMhParams& limitParams, float peakLuminance, float limitJMax, float midJ, float focusDistance,
		float invLowerHullGamma);
	int2 DetermineHueLinearitySearchRange(const GamutCuspsTable& gamutCuspsTable);

	// ---- 输出：SSBO 结构（照搬参考 OutputTransformParamsSSBO，std140 与 HLSL struct 一致）----
	struct alignas(16) OutputTransformParamsSSBO {
		float peakLuminance;
		float limitJMax;
		float invModelGamma;
		float saturation;
		float saturationThreshold;
		float chromaCompression;
		float chromaCompressionScale;
		float midJ;
		float focusDistiance;
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
		// 矩阵：float[12] raw 数组（3 列 × 4 行 = 12 floats，48B，4B 对齐）——避免 HLSL float4 数组歧义
		float inputJMh_rgbToCam16c[12];
		float inputJMh_coneResponseToAab[12];
		float limitJMh_cam16cToRGB[12];
		float limitJMh_aabToConeResponse[12];
	};

	struct GeneratedTables {
		OutputTransformParamsSSBO params;
		std::vector<float4> reachM;        // 362（.x 有效）
		std::vector<float4> upperHullGamma; // 362（.x 有效）
		std::vector<float4> gamutCusps;   // 362
	};
	GeneratedTables GenerateOutputTransformTables();
}
