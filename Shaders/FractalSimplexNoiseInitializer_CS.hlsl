// FractalSimplexNoise 初始化（一次性）：照搬参考项目 FractalNoise/FractalSimplexNoiseInitializer_CS.hlsl。
// 输出 256x256 R32Float（textureMode 6），供 Water_FS 泡沫/速度噪声采样。
#include "Random.hlsl"

RWTexture2D<float> FractalSimplexNoiseOut : register(u0);

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	static const uint texSideElements = 256;
	float2 uv = threadID.xy * (1.0 / texSideElements);

	float value = 0.0;
	static const uint fractalCount = 8;
	static const float invFractualCount = 1.0 / float(fractalCount);
	static const float baseScaling = 2.0;
	[Unroll] for (uint index = 0; index < fractalCount; ++index) {
		float weight = (2.0 * (fractalCount - index) / (fractalCount)) * invFractualCount;
		value += SimplexNoise(uv, pow(2.0, index + baseScaling), randomSeed0) * weight;
	}
	FractalSimplexNoiseOut[threadID.xy] = pow(value * 1.0, 2.0);
}
