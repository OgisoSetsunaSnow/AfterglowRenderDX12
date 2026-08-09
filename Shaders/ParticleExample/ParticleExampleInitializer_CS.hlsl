// ParticleExample 初始化（一次性），照搬参考 ParticleExampleInitializer_CS.hlsl。
// 关键改动：elapseTime 初始 = maxLifeTime + 1 → 第一帧 sim 立即重生到相机周围
// （参考把粒子放世界原点 [0,1]^3，依赖 sim 重生；但本工程相机不在原点 → 前 10-20s 粒子在相机身后
//  → w<0 投影翻转/退化 → 屏幕一条线。强制首帧重生避免此问题）。
// 另修 index=0 除零：1.0/index → 1.0/(index+1)（参考原版 index=0 时 life=NaN 永不重生）。
#include "../Common.hlsl"
#include "../Random.hlsl"
#include "ParticleCommon.hlsl"

RWStructuredBuffer<ParticleSSBOStruct> ParticleSSBO : register(u0);
RWTexture2D<float4> TestTextureOut : register(u1); // 128×32（index/32, index%32）

[numthreads(256, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	uint index = threadID.x;
	float invIndex = 1.0 / (index + 1.0); // 修 index=0 除零（参考 1.0/index → NaN）
	float colorSeed = randomSeed0 * 0.5;
	ParticleSSBO[index].position = Hash3D(float3(index, index, index), randomSeed0) * 1.0;
	ParticleSSBO[index].position.z += 1.0;
	ParticleSSBO[index].maxLifeTime = (Hash(invIndex, randomSeed1) + 1.0) * 10.0;
	ParticleSSBO[index].velocity = Snorm(Hash3D(ParticleSSBO[index].position * 100.0, randomSeed0)) * 0.25;
	// elapseTime 随机（0~maxLife）：粒子错开重生 → alpha 错开 → 始终有可见粒子。
	// 注意：不能所有粒子 elapseTime 相同（同步重生会一起淡入淡出 → 周期性全透明）。
	ParticleSSBO[index].elapseTime = ParticleSSBO[index].maxLifeTime * Hash(invIndex * 7.0, randomSeed1);
	ParticleSSBO[index].color = float4(Hash3D(float3(index, index, index), colorSeed), 1.0);

	TestTextureOut[uint2(index / 32, index % 32)].rg = abs(ParticleSSBO[index].velocity.xy) * 2.0;
}
