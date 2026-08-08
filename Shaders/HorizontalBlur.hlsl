// Bloom Horizontal Blur（参考项目 HorizontalBlur_FS：gaussianSigma3 15-tap 水平高斯，乘 bloomIntensity）。
Texture2D downSampledTexture : register(t0);
SamplerState linearSampler : register(s0);

cbuffer BloomParams : register(b0) {
	float4 bloomIntensity;      // rgb 强度（每级不同，参考 defaultBloomIntensities）
	float useCombinedTexture;   // 未用（仅 Vertical 用），占位
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

// 参考项目 Shaders/Kernels.hlsl gaussianSigma3（15-tap，0..7）
static const float gaussianSigma3[8] = {
	0.1329808, 0.1257944, 0.1064827, 0.0806569,
	0.0546700, 0.0331590, 0.0179970, 0.0087406
};

float4 PSMain(VSOut i) : SV_TARGET {
	const int2 pixel = int2(i.pos.xy);
	// 参考 HorizontalBlur_FS: maxResolution = resolutionScale * screenResolution（即 RT 实际尺寸）。
	uint texWidth, texHeight;
	downSampledTexture.GetDimensions(texWidth, texHeight);
	const int widthBound = int(texWidth);

	float3 color = 0.0;
	[unroll] for (int idx = -7; idx <= 7; ++idx) {
		const int x = clamp(pixel.x + idx, 0, widthBound - 1);
		color += downSampledTexture.Load(int3(x, pixel.y, 0)).rgb * gaussianSigma3[abs(idx)];
	}
	// 参考项目：乘 bloomIntensity（材质 scalar）
	return float4(color * bloomIntensity.xyz, 1.0);
}
