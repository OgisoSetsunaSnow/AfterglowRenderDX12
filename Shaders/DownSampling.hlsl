// 阶段 10b：Bloom DownSampling（参考项目 AfterglowDownSamplingPass，2x2 box 平均）。
// 输入 sceneTexture（HD sceneColor），输出 1/2 分辨率降采样 RT。
Texture2D sceneTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

VSOut VSMain(uint vid : SV_VertexID) {
	VSOut o;
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
	// 2x2 box average：当前像素对应 sceneColor 的 2x2 区域。
	const int2 base = int2(i.pos.xy) * 2;
	const float3 c00 = sceneTexture.Load(int3(base + int2(0, 0), 0)).rgb;
	const float3 c10 = sceneTexture.Load(int3(base + int2(1, 0), 0)).rgb;
	const float3 c01 = sceneTexture.Load(int3(base + int2(0, 1), 0)).rgb;
	const float3 c11 = sceneTexture.Load(int3(base + int2(1, 1), 0)).rgb;
	return float4((c00 + c10 + c01 + c11) * 0.25, 1.0);
}