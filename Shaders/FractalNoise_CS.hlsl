// FractalNormal 生成（一次性）：照搬参考项目 FractalNoise/FractalNoise_CS.hlsl。
// 输入：FractalSimplexNoise(t0) + FractalWorleyNoise(t1)（相邻 texel 差分求法线）。
// 输出：FractalNormal(u0) 256x256 RGBA8Snorm（textureMode 3）：.rg simplex 法线, .ba worley 法线。
#include "Common.hlsl"
#include "Random.hlsl"

Texture2D<float> FractalSimplexNoiseIn : register(t0);
Texture2D<float> FractalWorleyNoiseIn : register(t1);
RWTexture2D<float4> FractalNormalOut : register(u0);

// 锯齿环绕索引（参考 FractalNoise_CS zigzagIndex）。
template<typename Type>
Type zigzagIndex(Type index, Type count) {
	Type maxIndex = count - 1;
	return abs(fmod(index + maxIndex, maxIndex * 2) - maxIndex);
}

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	// .rg: Simplex noise normal
	// .ba: Worley noise normal
	// @note: Generate normal map here due to the initializer is not ensure the memory barrier.
	// so artifacts were occur in high performance parallel GPUs.
	uint2 texRes = uint2(256, 256);

	float4 adjacentValue = float4(
		FractalSimplexNoiseIn[zigzagIndex(threadID.xy + uint2(1, 0), texRes)].x,
		FractalSimplexNoiseIn[zigzagIndex(threadID.xy + uint2(0, 1), texRes)].x,
		FractalWorleyNoiseIn[zigzagIndex(threadID.xy + uint2(1, 0), texRes)].x,
		FractalWorleyNoiseIn[zigzagIndex(threadID.xy + uint2(0, 1), texRes)].x
	);
	float4 deltaValue = (float4(FractalSimplexNoiseIn[threadID.xy].xx, FractalWorleyNoiseIn[threadID.xy].xx) - adjacentValue);
	FractalNormalOut[threadID.xy] = float4(normalize(float3(deltaValue.xy, 1.0)).xy, normalize(float3(deltaValue.zw, 1.0)).xy);
}
