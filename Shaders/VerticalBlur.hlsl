// Bloom Vertical Blur + 组合上一级（参考项目 VerticalBlurCombination_FS）。
// 输入 horizontalBlurTexture(t0, 垂直高斯)，bloomCombinedTexture(t1, 上一级组合，useCombinedTexture 时相加)。
// 不乘 bloomIntensity（已在 horizontal 乘过）。
Texture2D horizontalBlurTexture : register(t0);
SamplerState linearSampler : register(s0);
Texture2D bloomCombinedTexture : register(t1);
SamplerState bloomCombinedTextureSampler : register(s1);

cbuffer BloomParams : register(b0) {
	float4 bloomIntensity;      // 未用（已乘过）
	float useCombinedTexture;   // 1=叠加上一级组合（最小级为 0）
	float3 _pad;
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

VSOut VSMain(uint vid : SV_VertexID) {
	VSOut o;
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, 0.0, 1.0);
	o.uv = uv;
	return o;
}

static const float gaussianSigma3[8] = {
	0.1329808, 0.1257944, 0.1064827, 0.0806569,
	0.0546700, 0.0331590, 0.0179970, 0.0087406
};

float4 PSMain(VSOut i) : SV_TARGET {
	const int2 pixel = int2(i.pos.xy);
	// 参考 VerticalBlurCombination_FS: maxResolution = resolutionScale * screenResolution（即 RT 实际尺寸）。
	uint texWidth, texHeight;
	horizontalBlurTexture.GetDimensions(texWidth, texHeight);
	const int heightBound = int(texHeight);

	float3 color = 0.0;
	[unroll] for (int idx = -7; idx <= 7; ++idx) {
		const int y = clamp(pixel.y + idx, 0, heightBound - 1);
		color += horizontalBlurTexture.Load(int3(pixel.x, y, 0)).rgb * gaussianSigma3[abs(idx)];
	}
	// 参考项目：叠加上一级（更小分辨率）组合，放大采样。
	if (useCombinedTexture > 0.5) {
		color += bloomCombinedTexture.Sample(bloomCombinedTextureSampler, i.uv).rgb;
	}
	return float4(color, 1.0);
}
