// 阶段 10b：Bloom Vertical Blur + Combined（参考项目 VerticalBlurCombination_FS）。
// 输入 horizontalBlurTexture，输出 bloomCombinedTexture（垂直高斯，不乘 bloomIntensity，已在 horizontal 乘过）。
Texture2D horizontalBlurTexture : register(t0);
SamplerState linearSampler : register(s0);

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
	const int widthBound = 65536;
	const int heightBound = 65536;

	float3 color = 0.0;
	[unroll] for (int idx = -7; idx <= 7; ++idx) {
		const int y = clamp(pixel.y + idx, 0, heightBound - 1);
		color += horizontalBlurTexture.Load(int3(pixel.x, y, 0)).rgb * gaussianSigma3[abs(idx)];
	}
	// 参考项目：VerticalBlurCombination 不再乘 bloomIntensity（已在 horizontal 乘过）
	return float4(color, 1.0);
}