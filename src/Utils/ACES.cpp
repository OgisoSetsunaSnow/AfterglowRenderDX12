// ACES2 OutputTransform 表生成实现（照搬参考项目 ACESCommon.cpp + ACESUtilities.cpp）。
#include "ACES.h"

#include <algorithm>
#include <cmath>

namespace aces {

float3x3 Chromaticities::makeRGBToXYZMat() const {
	constexpr float y = 1.0f;
	// X and Z values of RGB value (1, 1, 1), or "white"
	float x = white.x * y / white.y;
	float z = (1.0f - white.x - white.y) * y / white.y;

	// Scale factors for matrix rows
	float d = red.x * (blue.y - green.y) +
		blue.x * (green.y - red.y) +
		green.x * (red.y - blue.y);

	float sr = (x * (blue.y - green.y) -
		green.x * (y * (blue.y - 1) + blue.y * (x + z)) +
		blue.x * (y * (green.y - 1) + green.y * (x + z))) / d;

	float sg = (x * (red.y - blue.y) +
		red.x * (y * (blue.y - 1) + blue.y * (x + z)) -
		blue.x * (y * (red.y - 1) + red.y * (x + z))) / d;

	float sb = (x * (green.y - red.y) -
		red.x * (y * (green.y - 1) + green.y * (x + z)) +
		green.x * (y * (red.y - 1) + red.y * (x + z))) / d;

	// Assemble the matrix
	float3x3 m{};
	m[0][0] = sr * red[0];
	m[0][1] = sr * red[1];
	m[0][2] = sr * (1.0f - red[0] - red[1]);

	m[1][0] = sg * green[0];
	m[1][1] = sg * green[1];
	m[1][2] = sg * (1.0f - green[0] - green[1]);

	m[2][0] = sb * blue[0];
	m[2][1] = sb * blue[1];
	m[2][2] = sb * (1.0f - blue[0] - blue[1]);

	return m;
}

float3x3 ScaleMatrixDiagonal(const float3x3& mat, const float3& scale) {
	float3x3 result = mat;
	result[0][0] = mat[0][0] * scale[0];
	result[1][1] = mat[1][1] * scale[1];
	result[2][2] = mat[2][2] * scale[2];
	return result;
}

float WrapTo360(float x) {
	float y = fmod(x, 360.0f);
	if (y < 0.0f) {
		y += 360.0f;
	}
	return y;
}

float BaseHueForPosition(uint32_t index) {
	return static_cast<float>(index) * (hueLimit / tableSize);
}

uint32_t HuePositionInUniformTable(float hue, uint32_t size) {
	const float wrappedHue = WrapTo360(hue);
	return static_cast<uint32_t>(wrappedHue / hueLimit * static_cast<float>(size));
}

float PostAdaptationConeResponseCompressionForwardImpl(float Rc) {
	const float F_L_Y = std::pow(Rc, 0.42f);
	return F_L_Y / (cam_nl_offset + F_L_Y);
}

float PostAdaptationConeResponseCompressionForward(float v) {
	const float Ra = PostAdaptationConeResponseCompressionForwardImpl(std::abs(v));
	return std::copysign(Ra, v);
}

float PostAdaptationConeResponseCompressionInverseImpl(float Ra) {
	const float Ra_lim = std::min(Ra, 0.99f);
	const float F_L_Y = (cam_nl_offset * Ra_lim) / (1.0f - Ra_lim);
	return std::pow(F_L_Y, 1.0f / 0.42f);
}

float PostAdaptationConeResponseCompressionInverse(float v) {
	const float Rc = PostAdaptationConeResponseCompressionInverseImpl(std::abs(v));
	return std::copysign(Rc, v);
}

float JToAchromaticN(float J, float inv_cz) {
	return std::pow(J * (1.0f / cam_J_scale), inv_cz);
}

float AchromaticNToJ(float A, float cz) {
	return cam_J_scale * std::pow(A, cz);
}

float YToJ(float Y, const JMhParams& params) {
	float Ra = PostAdaptationConeResponseCompressionForwardImpl(std::abs(Y) * params.F_L_n);
	float J = AchromaticNToJ(Ra * params.inv_A_w_J, params.cz);
	return std::copysign(J, Y);
}

float3 AabToJMh(const float3& colorAab, const JMhParams& jmhParams) {
	if (colorAab.x <= 0.0f) {
		return float3{ 0.0f, 0.0f, 0.0f };
	}
	float J = AchromaticNToJ(colorAab.x, jmhParams.cz);
	float M = std::sqrt(colorAab.y * colorAab.y + colorAab.z * colorAab.z);
	float h_rad = std::atan2(colorAab.z, colorAab.y);
	float h = WrapTo360(glm::degrees(h_rad));
	return float3{ J, M, h };
}

float3 JMhToAab(const float3& colorJMh, const JMhParams& jmhParams) {
	float h_rad = glm::radians(colorJMh.z);
	float cos_hr = std::cos(h_rad);
	float sin_hr = std::sin(h_rad);

	float A = JToAchromaticN(colorJMh.x, jmhParams.inv_cz);
	float a = colorJMh.y * cos_hr;
	float b = colorJMh.y * sin_hr;

	return float3(A, a, b);
}

float3 RGBToAab(const float3& colorRGB, const JMhParams& jmhParams) {
	float3 rgb_m = jmhParams.rgbToCAM16c * colorRGB;
	float3 rgb_a = {
		PostAdaptationConeResponseCompressionForward(rgb_m.x),
		PostAdaptationConeResponseCompressionForward(rgb_m.y),
		PostAdaptationConeResponseCompressionForward(rgb_m.z)
	};
	return jmhParams.coneResponseToAab * rgb_a;
}

float3 AabToRGB(const float3& colorAab, const JMhParams& jmhParams) {
	float3 rgb_a = jmhParams.aabToConeResponse * colorAab;
	float3 rgb_m = float3(
		PostAdaptationConeResponseCompressionInverse(rgb_a.x),
		PostAdaptationConeResponseCompressionInverse(rgb_a.y),
		PostAdaptationConeResponseCompressionInverse(rgb_a.z)
	);
	return jmhParams.cam16cToRGB * rgb_m;
}

float3 RGBToJMh(const float3& colorRGB, const JMhParams& jmhParams) {
	float3 colorAab = RGBToAab(colorRGB, jmhParams);
	return AabToJMh(colorAab, jmhParams);
}

float3 JMhToRGB(const float3& colorJMh, const JMhParams& jmhParams) {
	float3 colorAab = JMhToAab(colorJMh, jmhParams);
	return AabToRGB(colorAab, jmhParams);
}

float3 GenerateUnitCubeCuspCorners(uint32_t cornerIndex) {
	// Generation order R, Y, G, C, B, M to ensure hues rotate in correct order
	return float3{
		((cornerIndex + 1) % cuspCornerCount) < 3 ? 1.0f : 0.0f,
		((cornerIndex + 5) % cuspCornerCount) < 3 ? 1.0f : 0.0f,
		((cornerIndex + 3) % cuspCornerCount) < 3 ? 1.0f : 0.0f
	};
}

void BuildLimitingCuspCorners(CuspCorners& limitingRGBCorners, CuspCorners& limitingJMhCorners,
	const JMhParams& limitParams, float peakLuminance) {
	std::array<float3, cuspCornerCount> tempRGBCorners{};
	std::array<float3, cuspCornerCount> tempJMhCorners{};

	int32_t min_index = 0;
	for (int32_t i = 0; i < cuspCornerCount; ++i) {
		tempRGBCorners[i] = GenerateUnitCubeCuspCorners(i) * (peakLuminance / cam_ref_luminance);
		tempJMhCorners[i] = RGBToJMh(tempRGBCorners[i], limitParams);
		if (tempJMhCorners[i].z < tempJMhCorners[min_index].z) {
			min_index = 1; // 照搬参考（参考原文如此，虽疑似 bug 但严格一致）
		}
	}
	for (size_t i = 0; i < cuspCornerCount; ++i) {
		limitingRGBCorners[i + 1] = tempRGBCorners[(i + min_index) % cuspCornerCount];
		limitingJMhCorners[i + 1] = tempJMhCorners[(i + min_index) % cuspCornerCount];
	}
	constexpr int32_t lastCornerIndex = paddedCuspCornerCount - 1;
	limitingRGBCorners[0] = limitingRGBCorners[cuspCornerCount];
	limitingRGBCorners[lastCornerIndex] = limitingRGBCorners[1];
	limitingJMhCorners[0] = limitingJMhCorners[cuspCornerCount];
	limitingJMhCorners[lastCornerIndex] = limitingJMhCorners[1];

	limitingJMhCorners[0].z -= hueLimit;
	limitingJMhCorners[lastCornerIndex].z += hueLimit;
}

CuspCorners FindReachCorners(const JMhParams& reachParams, float limitJMax, float toneForwardLimit) {
	constexpr float reachCuspTolerance = 1e-3f;

	std::array<float3, cuspCornerCount> tempCorners{};
	CuspCorners jmhCorners{};

	float limitA = JToAchromaticN(limitJMax, reachParams.inv_cz);

	int32_t minIndex = 0;
	for (int32_t i = 0; i < cuspCornerCount; ++i) {
		float3 rgbVector = GenerateUnitCubeCuspCorners(i);

		float lower = 0.0f;
		float upper = toneForwardLimit;

		while ((upper - lower) > reachCuspTolerance) {
			float test = (lower + upper) / 2.0f;
			float3 testCorner = rgbVector * test;
			float A = RGBToAab(testCorner, reachParams).x;
			if (A < limitA) {
				lower = test;
			} else {
				upper = test;
			}
		}

		tempCorners[i] = RGBToJMh(rgbVector * upper, reachParams);

		if (tempCorners[i].z < tempCorners[minIndex].z) {
			minIndex = i;
		}
	}

	for (size_t i = 0; i < cuspCornerCount; ++i) {
		jmhCorners[i + 1] = tempCorners[(i + minIndex) % cuspCornerCount];
	}
	constexpr int32_t lastCornerIndex = paddedCuspCornerCount - 1;
	jmhCorners[0] = jmhCorners[cuspCornerCount];
	jmhCorners[lastCornerIndex] = jmhCorners[1];

	jmhCorners[0].z -= hueLimit;
	jmhCorners[lastCornerIndex].z += hueLimit;

	return jmhCorners;
}

SortedCornerHues ExtractSortedCubeHues(const CuspCorners& reachJMhCorners, const CuspCorners& limitJMhCorners) {
	SortedCornerHues sortedCornerHues{};

	int32_t index = 0;
	int32_t reachIndex = 1;
	int32_t limitIndex = 1;
	while ((reachIndex < cuspCornerCount + 1) || (limitIndex < cuspCornerCount + 1)) {
		float reachHue = reachJMhCorners[reachIndex].z;
		float limitHue = limitJMhCorners[limitIndex].z;
		if (reachHue == limitHue) {
			sortedCornerHues[index] = reachHue;
			++reachIndex;
			++limitIndex;
		} else {
			if (reachHue < limitHue) {
				sortedCornerHues[index] = reachHue;
				++reachIndex;
			} else {
				sortedCornerHues[index] = limitHue;
				++limitIndex;
			}
		}
		++index;
	}
	return sortedCornerHues;
}

float2 FindDisplayCuspForHue(float hue, const CuspCorners& rgbCorners, const CuspCorners& jmhCorners,
	const JMhParams& jmhParams) {
	int32_t upperCorner = 1;
	for (int32_t i = upperCorner; i < paddedCuspCornerCount; ++i) {
		if (jmhCorners[i].z > hue) {
			upperCorner = i;
			break;
		}
	}
	int32_t lowerCorner = upperCorner - 1;

	if (jmhCorners[lowerCorner].z == hue) {
		float3 jmhCorner = jmhCorners[lowerCorner];
		return { jmhCorner.x, jmhCorner.y };
	}

	float3 cuspLower = rgbCorners[lowerCorner];
	float3 cuspUpper = rgbCorners[upperCorner];

	float sampleT = 0.0f;
	float lowerT = 0.0f;
	float upperT = 1.0f;

	float3 sample{};
	float3 colorJMh{};

	constexpr float displayCuspTolerance = 1e-7f;
	while ((upperT - lowerT) > displayCuspTolerance) {
		sampleT = (lowerT + upperT) * 0.5f;
		sample = cuspLower + (cuspUpper - cuspLower) * sampleT;
		colorJMh = RGBToJMh(sample, jmhParams);
		if (colorJMh.z < jmhCorners[lowerCorner].z) {
			upperT = sampleT;
		} else if (colorJMh.z >= jmhCorners[upperCorner].z) {
			lowerT = sampleT;
		} else if (colorJMh.z > hue) {
			upperT = sampleT;
		} else {
			lowerT = sampleT;
		}
	}

	sampleT = (lowerT + upperT) * 0.5f;
	sample = cuspLower + (cuspUpper - cuspLower) * sampleT;
	colorJMh = RGBToJMh(sample, jmhParams);

	return { colorJMh.x, colorJMh.y };
}

void BuildHueSampleInterval(HueTable& hueTable, int32_t samples, float lower, float upper, int32_t baseIndex) {
	float delta = (upper - lower) / static_cast<float>(samples);
	for (int32_t i = 0; i < samples; ++i) {
		hueTable[i + baseIndex] = lower + static_cast<float>(i) * delta;
	}
}

void BuildHueTable(HueTable& hueTable, const SortedCornerHues& sortedHues) {
	float idealSpacing = static_cast<float>(tableSize) / hueLimit;
	std::array<int32_t, maxSortedCornerCount + 2> samplesCount{};
	int32_t lastIndex = -1;
	int32_t minIndex = sortedHues[0] == 0.0f ? 0 : 1;

	for (size_t hueIndex = 0; hueIndex < maxSortedCornerCount; ++hueIndex) {
		int32_t nominalIndex = std::clamp(
			static_cast<int32_t>(std::round(sortedHues[hueIndex] * idealSpacing)),
			minIndex,
			static_cast<int32_t>(tableSize) - 1
		);

		if (lastIndex == nominalIndex) {
			if (hueIndex > 1 && samplesCount[hueIndex - 2] != (samplesCount[hueIndex - 1] - 1)) {
				samplesCount[hueIndex - 1] = samplesCount[hueIndex - 1] - 1;
			} else {
				nominalIndex = nominalIndex + 1;
			}
		}
		samplesCount[hueIndex] = std::min(nominalIndex, static_cast<int32_t>(tableSize) - 1);
		minIndex = nominalIndex;
		lastIndex = minIndex;
	}

	int32_t totalSamples = 0;

	BuildHueSampleInterval(hueTable, samplesCount[0], 0.0f, sortedHues[0], totalSamples + 1);
	totalSamples = totalSamples + samplesCount[0];

	for (size_t i = 1; i < maxSortedCornerCount; ++i) {
		int32_t samples = samplesCount[i] - samplesCount[i - 1];
		BuildHueSampleInterval(hueTable, samples, sortedHues[i - 1], sortedHues[i], totalSamples + 1);
		totalSamples = totalSamples + samples;
	}
	BuildHueSampleInterval(hueTable, static_cast<int32_t>(tableSize) - totalSamples,
		sortedHues[maxSortedCornerCount - 1], hueLimit, totalSamples + 1);

	hueTable[0] = hueTable[tableSize] - hueLimit;
	hueTable[static_cast<size_t>(paddedTableSize) - 1] = hueTable[1] + hueLimit;
}

void BuildCuspsTable(GamutCuspsTable& cuspsTable, const HueTable& hueTable, const CuspCorners& rgbCorners,
	const CuspCorners& jmhCorners, const JMhParams& jmhParams) {
	for (int32_t i = 1; i < paddedTableSize; ++i) {
		float hue = hueTable[i];
		float2 colorJM = FindDisplayCuspForHue(hue, rgbCorners, jmhCorners, jmhParams);
		cuspsTable[i] = { colorJM.x, colorJM.y * (1.0f + smooth_m * smooth_cusps), hue, 0.0f };
	}

	cuspsTable[0] = cuspsTable[tableSize];
	cuspsTable[0].z = hueTable[0];

	constexpr int32_t lastIndex = paddedTableSize - 1;
	cuspsTable[lastIndex] = cuspsTable[1];
	cuspsTable[lastIndex].z = hueTable[lastIndex];
}

float ComputeFocusJ(float cuspJ, float midJ, float limitJMax) {
	return cuspJ + (midJ - cuspJ) * std::min(1.0f, cusp_mid_blend - (cuspJ / limitJMax));
}

float GetFocusGain(float colorJ, float analyticalThreshold, float limitJMax, float focusDistance) {
	float gain = limitJMax * focusDistance;

	if (colorJ > analyticalThreshold) {
		float gainAdjustment = std::log10((limitJMax - analyticalThreshold) / std::max(0.0001f, limitJMax - colorJ));
		gainAdjustment = gainAdjustment * gainAdjustment + 1.0f;
		gain *= gainAdjustment;
	}
	return gain;
}

float SolveIntersectJ(float colorJ, float colorM, float focusJ, float maxJ, float slopeGain) {
	const float scaledM = colorM / slopeGain;
	const float a = scaledM / focusJ;

	if (colorJ < focusJ) {
		const float b = 1.0f - scaledM;
		const float c = -colorJ;
		const float det = b * b - 4.0f * a * c;
		const float root = std::sqrt(det);
		return -2.0f * c / (b + root);
	} else {
		const float b = -(1.0f + scaledM + maxJ * a);
		const float c = maxJ * scaledM + colorJ;
		const float det = b * b - 4.0f * a * c;
		const float root = std::sqrt(det);
		return -2.0f * c / (b - root);
	}
}

float ComputeCompressionVectorSlope(float intersectJ, float focusJ, float limitJMax, float slopeGain) {
	float directionScalar = 0.0f;
	if (intersectJ < focusJ) {
		directionScalar = intersectJ;
	} else {
		directionScalar = limitJMax - intersectJ;
	}
	return directionScalar * (intersectJ - focusJ) / (focusJ * slopeGain);
}

float SmoothMinimumScaled(float a, float b, float scaleReference) {
	const float scaledS = smooth_cusps * scaleReference;
	const float h = std::max(scaledS - std::abs(a - b), 0.0f) / scaledS;
	return std::min(a, b) - h * h * h * scaledS * (1.0f / 6.0f);
}

float EstimateLineAndBoundaryIntersectionM(float axisIntersectJ, float slope, float invGamma, float maxJ, float maxM,
	float intersectionReferenceJ) {
	const float normalisedJ = axisIntersectJ / intersectionReferenceJ;
	const float shiftedIntersection = intersectionReferenceJ * std::pow(normalisedJ, invGamma);

	return shiftedIntersection * maxM / (maxJ - slope * maxM);
}

float FindGamutBoundaryIntersection(float2 cuspJM, float maxJ, float invGammaTop, float invGammaBottom,
	float intersectSourceJ, float slope, float intersectCuspJ) {
	const float boundaryLowerM = EstimateLineAndBoundaryIntersectionM(
		intersectSourceJ, slope, invGammaBottom, cuspJM.x, cuspJM.y, intersectCuspJ
	);

	const float intersectCuspFJ = maxJ - intersectCuspJ;
	const float intersectSourceFJ = maxJ - intersectSourceJ;
	const float fJMCuspJ = maxJ - cuspJM.x;
	const float boundaryUpperM = EstimateLineAndBoundaryIntersectionM(
		intersectSourceFJ, -slope, invGammaTop, fJMCuspJ, cuspJM.y, intersectCuspFJ
	);

	float boundaryM = SmoothMinimumScaled(boundaryLowerM, boundaryUpperM, cuspJM.y);
	return boundaryM;
}

bool EvaluateGammaFit(float2 cuspJM, float invTopGamma, float peakLuminance, float limitJMax, float invLowerHullGamma,
	const std::array<float3, upperHullGammaTestCount>& testJMhs, const std::array<float, upperHullGammaTestCount>& intersectSourceJs,
	const std::array<float, upperHullGammaTestCount>& slopes, const std::array<float, upperHullGammaTestCount>& intersectCuspJs,
	const JMhParams& limitParams) {
	float luminanceLimit = peakLuminance / cam_ref_luminance;

	for (int32_t testIndex = 0; testIndex < upperHullGammaTestCount; ++testIndex) {
		float approxLimitM = FindGamutBoundaryIntersection(
			cuspJM, limitJMax, invTopGamma, invLowerHullGamma,
			intersectSourceJs[testIndex], slopes[testIndex], intersectCuspJs[testIndex]
		);
		float approxLimitJ = intersectSourceJs[testIndex] + slopes[testIndex] * approxLimitM;
		float3 approximateJMh = { approxLimitJ, approxLimitM, testJMhs[testIndex].z };

		float3 newLimitRGB = JMhToRGB(approximateJMh, limitParams);

		bool outsideHill = (newLimitRGB.x > luminanceLimit) || (newLimitRGB.y > luminanceLimit) || (newLimitRGB.z > luminanceLimit);
		if (!outsideHill) {
			return false;
		}
	}
	return true;
}

void GenerateGammaTestData(const float2 cuspJM, const float hue, const float limitJMax, const float midJ,
	const float focusDistance, std::array<float3, upperHullGammaTestCount>& testJMhs,
	std::array<float, upperHullGammaTestCount>& intersectSourceJs, std::array<float, upperHullGammaTestCount>& slopes,
	std::array<float, upperHullGammaTestCount>& intersectCuspJs) {
	float analyticalThreshold = cuspJM.x + (limitJMax - cuspJM.x) * focus_gain_blend;
	float focusJ = ComputeFocusJ(cuspJM.x, midJ, limitJMax);

	constexpr std::array<float, upperHullGammaTestCount> testPositions = { 0.01f, 0.1f, 0.5f, 0.8f, 0.99f };

	for (int32_t testIndex = 0; testIndex < upperHullGammaTestCount; ++testIndex) {
		float testJ = cuspJM.x + (limitJMax - cuspJM.x) * testPositions[testIndex];
		float slopeGain = GetFocusGain(testJ, analyticalThreshold, limitJMax, focusDistance);
		float intersectJ = SolveIntersectJ(testJ, cuspJM.y, focusJ, limitJMax, slopeGain);
		float slope = ComputeCompressionVectorSlope(intersectJ, focusJ, limitJMax, slopeGain);
		float cuspJ = SolveIntersectJ(cuspJM.x, cuspJM.y, focusJ, limitJMax, slopeGain);

		testJMhs[testIndex] = { testJ, cuspJM.y, hue };
		intersectSourceJs[testIndex] = intersectJ;
		slopes[testIndex] = slope;
		intersectCuspJs[testIndex] = cuspJ;
	}
}

std::unique_ptr<ReachMTable> MakeReachMTable(const JMhParams& reachParams, float limitJMax) {
	const float searchRange = 50.0f;
	const float searchMaximum = 1300.0f;

	auto tablePtr = std::make_unique<ReachMTable>();
	ReachMTable& table = *tablePtr;
	for (uint32_t tableIndex = 0; tableIndex < tableSize; ++tableIndex) {
		float hue = BaseHueForPosition(tableIndex);
		float low = 0.0f;
		float high = low + searchRange;
		bool outside = false;

		while ((outside != true) && (high < searchMaximum)) {
			float3 searchJMh = { limitJMax, high, hue };
			float3 newLimitRGB = JMhToRGB(searchJMh, reachParams);
			outside = newLimitRGB.x < 0.0f || newLimitRGB.y < 0.0f || newLimitRGB.z < 0.0f;
			if (!outside) {
				low = high;
				high = high + searchRange;
			}
		}

		while (high - low > 1e-2f) {
			float sampleM = (high + low) / 2.0f;
			float3 searchJMh = { limitJMax, sampleM, hue };
			float3 newLimitRGB = JMhToRGB(searchJMh, reachParams);
			outside = newLimitRGB.x < 0.0f || newLimitRGB.y < 0.0f || newLimitRGB.z < 0.0f;
			if (outside) {
				high = sampleM;
			} else {
				low = sampleM;
			}
		}

		table[tableIndex + 1] = high;
	}

	table[0] = table[tableSize];
	table[paddedTableSize - 1] = table[1];

	return tablePtr;
}

std::unique_ptr<GamutCuspsTable> MakeUniformHueGamutTable(const JMhParams& reachParams, const JMhParams& limitParams,
	float peakLuminance, float limitJMax, float toneForwardLimit) {
	CuspCorners reachJMhCorners = FindReachCorners(reachParams, limitJMax, toneForwardLimit);
	CuspCorners limitingRGBCorners;
	CuspCorners limitingJMhCorners;

	BuildLimitingCuspCorners(limitingRGBCorners, limitingJMhCorners, limitParams, peakLuminance);
	SortedCornerHues sortedHues = ExtractSortedCubeHues(reachJMhCorners, limitingJMhCorners);

	auto hueTablePtr = std::make_unique<HueTable>();
	BuildHueTable(*hueTablePtr, sortedHues);

	auto cuspsTablePtr = std::make_unique<GamutCuspsTable>();
	BuildCuspsTable(*cuspsTablePtr, *hueTablePtr, limitingRGBCorners, limitingJMhCorners, limitParams);

	return cuspsTablePtr;
}

std::unique_ptr<UpperHullGammaTable> MakeUpperHullGammaTable(const GamutCuspsTable& gamutCuspsTable,
	const JMhParams& limitParams, float peakLuminance, float limitJMax, float midJ, float focusDistance,
	float invLowerHullGamma) {
	auto tablePtr = std::make_unique<UpperHullGammaTable>();
	auto& table = *tablePtr;

	constexpr float gammaAccuracy = 1e-5f;

	for (int32_t i = 1; i < tableSize + 1; ++i) {
		float4 cuspJMhx = gamutCuspsTable[i];
		float hue = cuspJMhx.z;
		float2 cuspJM = { cuspJMhx.x, cuspJMhx.y };

		std::array<float3, upperHullGammaTestCount> testJMhs{};
		std::array<float, upperHullGammaTestCount> intersectSourceJs{};
		std::array<float, upperHullGammaTestCount> slopes{};
		std::array<float, upperHullGammaTestCount> intersectCuspJs{};

		GenerateGammaTestData(cuspJM, hue, limitJMax, midJ, focusDistance, testJMhs, intersectSourceJs, slopes, intersectCuspJs);

		constexpr float searchRange = 0.4f;
		float low = 0.0f;
		float high = low + searchRange;
		while (high < 5.0f) {
			bool gammaFound = EvaluateGammaFit(cuspJM, 1.0f / high, peakLuminance, limitJMax, invLowerHullGamma,
				testJMhs, intersectSourceJs, slopes, intersectCuspJs, limitParams);
			if (!gammaFound) {
				low = high;
				high += searchRange;
			} else {
				break;
			}
		}

		float testGamma = -1.0f;
		while ((high - low) > gammaAccuracy) {
			testGamma = (high + low) * 0.5f;
			bool gammaFound = EvaluateGammaFit(cuspJM, 1.0f / testGamma, peakLuminance, limitJMax, invLowerHullGamma,
				testJMhs, intersectSourceJs, slopes, intersectCuspJs, limitParams);
			if (gammaFound) {
				high = testGamma;
			} else {
				low = testGamma;
			}
		}

		table[i] = 1.0f / high;
	}

	table[0] = table[tableSize];
	table[paddedTableSize - 1] = table[1];

	return tablePtr;
}

int2 DetermineHueLinearitySearchRange(const GamutCuspsTable& gamutCuspsTable) {
	const int lowerPadding = 0;
	const int upperPadding = 1;

	int2 hueLinearitySearchRange = { lowerPadding, upperPadding };

	for (int i = 1; i < tableSize + 1; ++i) {
		const int pos = static_cast<int>(HuePositionInUniformTable(gamutCuspsTable[i].z, paddedTableSize));
		const int delta = i - pos;
		hueLinearitySearchRange = {
			std::min(hueLinearitySearchRange.x, delta + lowerPadding),
			std::max(hueLinearitySearchRange.y, delta + upperPadding)
		};
	}
	return hueLinearitySearchRange;
}

JMhParams::JMhParams(const Chromaticities& chromaticities) {
	constexpr float3x3 baseConeResponseToAab = {
		2.0f, 1.0f, 1.0f / 9.0f,
		1.0f, -12.0f / 11.0f, 1.0f / 9.0f,
		1.0f / 20.0f, 1.0f / 11.0f, -2.0f / 9.0f
	};

	const float3x3 cam16ToXYZMat = chromaticitiesCAM16.makeRGBToXYZMat();
	const float3x3 xyzToCAM16Mat = glm::inverse(cam16ToXYZMat);
	const float3x3 rgbToXYZ = chromaticities.makeRGBToXYZMat();

	const float3 whiteXYZ = rgbToXYZ * float3(cam_ref_luminance, cam_ref_luminance, cam_ref_luminance);

	float3 whiteCAM16 = xyzToCAM16Mat * whiteXYZ;

	const float3 D_RGB = {
		F_L_n * whiteXYZ.y / whiteCAM16.x,
		F_L_n * whiteXYZ.y / whiteCAM16.y,
		F_L_n * whiteXYZ.y / whiteCAM16.z
	};

	const float3 RGB_wc = {
		D_RGB.x * whiteCAM16.x,
		D_RGB.y * whiteCAM16.y,
		D_RGB.z * whiteCAM16.z
	};

	const float3 RGB_Aw = {
		PostAdaptationConeResponseCompressionForward(RGB_wc.x),
		PostAdaptationConeResponseCompressionForward(RGB_wc.y),
		PostAdaptationConeResponseCompressionForward(RGB_wc.z)
	};

	coneResponseToAab = baseConeResponseToAab * (glm::mat3x3(1.0f) * cam_nl_scale);
	float A_w = coneResponseToAab[0][0] * RGB_Aw.x + coneResponseToAab[1][0] * RGB_Aw.y + coneResponseToAab[2][0] * RGB_Aw.z;

	A_w_J = PostAdaptationConeResponseCompressionForwardImpl(F_L);
	inv_A_w_J = 1.0f / A_w_J;

	// Prescale the CAM16 LMS responses to directly provide for chromatic adaptation
	float3x3 M1 = xyzToCAM16Mat * rgbToXYZ;
	float3x3 M2 = glm::mat3x3(1.0f) * cam_ref_luminance;
	float3x3 rgbToCAM16 = M2 * M1;
	rgbToCAM16c = ScaleMatrixDiagonal(glm::mat3x3(1.0f), D_RGB) * rgbToCAM16;
	cam16cToRGB = glm::inverse(rgbToCAM16c);

	coneResponseToAab = {
		coneResponseToAab[0][0] / A_w, coneResponseToAab[0][1] * 43.0f * cam_surround.z, coneResponseToAab[0][2] * 43.0f * cam_surround.z,
		coneResponseToAab[1][0] / A_w, coneResponseToAab[1][1] * 43.0f * cam_surround.z, coneResponseToAab[1][2] * 43.0f * cam_surround.z,
		coneResponseToAab[2][0] / A_w, coneResponseToAab[2][1] * 43.0f * cam_surround.z, coneResponseToAab[2][2] * 43.0f * cam_surround.z
	};

	aabToConeResponse = glm::inverse(coneResponseToAab);
}

TonescaleParams::TonescaleParams(float inPeakLuminance) {
	peakLuminance = inPeakLuminance;

	n_r = 100.0f;
	g = 1.15f;
	constexpr float c = 0.18f;
	constexpr float c_d = 10.013f;
	constexpr float w_g = 0.14f;
	t_1 = 0.04f;
	constexpr float r_hit_min = 128.0f;
	constexpr float r_hit_max = 896.0f;

	const float r_hit = r_hit_min + (r_hit_max - r_hit_min) * (std::log(peakLuminance / n_r) / std::log(10000.0f / 100.0f));
	const float m_0 = (peakLuminance / n_r);
	const float m_1 = 0.5f * (m_0 + std::sqrt(m_0 * (m_0 + 4.0f * t_1)));
	const float u = std::pow((r_hit / m_1) / ((r_hit / m_1) + 1.0f), g);
	const float m = m_1 / u;
	const float w_i = std::log(peakLuminance / 100.0f) / std::log(2.0f);
	c_t = c_d / n_r * (1.0f + w_i * w_g);

	const float g_ip = 0.5f * (c_t + std::sqrt(c_t * (c_t + 4.0f * t_1)));
	const float g_ipp2 = -(m_1 * std::pow((g_ip / m), (1.0f / g))) / (std::pow(g_ip / m, 1.0f / g) - 1.0f);
	const float w_2 = c / g_ipp2;
	s_2 = w_2 * m_1;
	u_2 = std::pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
	m_2 = m_1 / u_2;

	forwardLimit = 8.0f * r_hit;
	inverseLimit = peakLuminance / (u_2 * n_r);
	logPeak = std::log10(m_0);
}

OutputTransformParams::OutputTransformParams(float inPeakLuminance, const Chromaticities& limitChromaticities)
	: inputParams(chromaticitiesACEScg),
	  reachParams(chromaticitiesACEScg),
	  limitParams(limitChromaticities),
	  tonescaleParams(inPeakLuminance) {
	peakLuminance = inPeakLuminance;

	limitJMax = YToJ(peakLuminance, inputParams);
	invModelGamma = 1.0f / cam_model_gamma;

	saturation = std::max(0.2f, chroma_expand - (chroma_expand * chroma_expand_fact) * tonescaleParams.logPeak);
	saturationThreshold = chroma_expand_thr / peakLuminance;
	chromaCompression = chroma_compress + (chroma_compress * chroma_compress_fact) * tonescaleParams.logPeak;
	chromaCompressionScale = std::pow(0.03379f * peakLuminance, 0.30596f) - 0.45135f;

	midJ = YToJ(tonescaleParams.c_t * cam_ref_luminance, inputParams);
	focusDistiance = focus_distance + focus_distance * focus_distance_scaling * tonescaleParams.logPeak;
	const float lowerHullGamma = 1.14f + 0.07f * tonescaleParams.logPeak;
	invLowerHullGamma = 1.0f / lowerHullGamma;

	reachMTable = MakeReachMTable(reachParams, limitJMax);
	gamutCuspsTable = MakeUniformHueGamutTable(reachParams, limitParams, peakLuminance, limitJMax,
		tonescaleParams.forwardLimit);
	upperHullGammaTable = MakeUpperHullGammaTable(*gamutCuspsTable, limitParams, peakLuminance, limitJMax, midJ,
		focusDistiance, invLowerHullGamma);

	hueLinearitySearchRange = DetermineHueLinearitySearchRange(*gamutCuspsTable);
}

// glm::mat3x3 -> mat3x4（扩充第 4 行 0，匹配 HLSL float4x3 列主序存储）
static float3x4 Mat3ToMat4(const float3x3& m) {
	return float3x4(m);
}

GeneratedTables GenerateOutputTransformTables() {
	OutputTransformParams params; // 默认 peak=100, sRGB

	GeneratedTables out;
	out.reachM.assign(paddedTableSize, float4(0.0f));
	out.upperHullGamma.assign(paddedTableSize, float4(0.0f));
	out.gamutCusps.assign(paddedTableSize, float4(0.0f));

	// 照搬参考 takeSSBO（OutputTransformParamsSSBO 填充）
	auto& s = out.params;
	s.peakLuminance = params.peakLuminance;
	s.limitJMax = params.limitJMax;
	s.invModelGamma = params.invModelGamma;
	s.saturation = params.saturation;
	s.saturationThreshold = params.saturationThreshold;
	s.chromaCompression = params.chromaCompression;
	s.chromaCompressionScale = params.chromaCompressionScale;
	s.midJ = params.midJ;
	s.focusDistiance = params.focusDistiance;
	s.invLowerHullGamma = params.invLowerHullGamma;
	s.hueLinearitySearchRange = params.hueLinearitySearchRange;

	s.tonescale_n_r = params.tonescaleParams.n_r;
	s.tonescale_g = params.tonescaleParams.g;
	s.tonescale_t_1 = params.tonescaleParams.t_1;
	s.tonescale_c_t = params.tonescaleParams.c_t;
	s.tonescale_s_2 = params.tonescaleParams.s_2;
	s.tonescale_u_2 = params.tonescaleParams.u_2;
	s.tonescale_m_2 = params.tonescaleParams.m_2;
	s.tonescale_forwardLimit = params.tonescaleParams.forwardLimit;
	s.tonescale_inverseLimit = params.tonescaleParams.inverseLimit;

	s.jmh_F_L_n = F_L_n;
	s.jmh_cz = jmh_cz;
	s.jmh_inv_cz = jmh_inv_cz;
	s.inputJMh_A_w_j = params.inputParams.A_w_J;
	s.inputJMh_inv_A_w_j = params.inputParams.inv_A_w_J;
	const auto setMat = [&](float* dst, const float3x3& mat) {
		float3x4 m4 = Mat3ToMat4(mat);
		// 列主序 12 floats：col0[0..3], col1[0..3], col2[0..3]
		dst[0] = m4[0][0]; dst[1] = m4[0][1]; dst[2] = m4[0][2]; dst[3] = m4[0][3];
		dst[4] = m4[1][0]; dst[5] = m4[1][1]; dst[6] = m4[1][2]; dst[7] = m4[1][3];
		dst[8] = m4[2][0]; dst[9] = m4[2][1]; dst[10] = m4[2][2]; dst[11] = m4[2][3];
	};
	setMat(s.inputJMh_rgbToCam16c, params.inputParams.rgbToCAM16c);
	setMat(s.inputJMh_coneResponseToAab, params.inputParams.coneResponseToAab);
	setMat(s.limitJMh_cam16cToRGB, params.limitParams.cam16cToRGB);
	setMat(s.limitJMh_aabToConeResponse, params.limitParams.aabToConeResponse);

	for (uint32_t i = 0; i < paddedTableSize; ++i) {
		out.reachM[i] = float4((*params.reachMTable)[i], 0.0f, 0.0f, 0.0f);
		out.upperHullGamma[i] = float4((*params.upperHullGammaTable)[i], 0.0f, 0.0f, 0.0f);
		out.gamutCusps[i] = (*params.gamutCuspsTable)[i];
	}

	return out;
}

} // namespace aces
