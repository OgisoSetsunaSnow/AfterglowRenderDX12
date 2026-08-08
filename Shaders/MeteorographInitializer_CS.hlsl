// Meteorograph 初始化（一次性）：照搬参考项目 Meteorograph/MeteorographInitializer_CS.hlsl。
// 输出 R32G32B32A32_FLOAT（参考 textureMode 9 = RGBA32Float）：.rg 风, .b 湿度, .a 温度(Celsius)。
#include "MeteorographCommon.hlsl"
#include "Random.hlsl"

RWTexture2D<float4> MeteorographOut : register(u0);

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	float2 worldPos = WorldPositionFromTileID(threadID.xy, meteorographInvSideElements);
	float2 wind = Snorm2(float2(
		PerlinNoise(float2(threadID.xy) * meteorographInvSideElements, 10.0, randomSeed0),
		PerlinNoise(float2(threadID.xy) * meteorographInvSideElements, 10.0, randomSeed1)
	));
	float humidity = PerlinNoise(float2(threadID.xy) * meteorographInvSideElements, 20.0, randomSeed2);
	float temperature = SurfaceTemperature(worldPos.y, 0.0);

	MeteorographOut[threadID.xy] = float4(wind, humidity, temperature);
}
