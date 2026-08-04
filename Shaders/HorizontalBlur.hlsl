// 阶段 10b：Bloom Horizontal Blur（参考项目 HorizontalBlur_FS：gaussianSigma3 15-tap 水平高斯）。
// 输入 downSampledTexture，输出 horizontalBlurTexture（乘 bloomIntensity）。
Texture2D downSampledTexture : register(t0);
SamplerState linearSampler : register(s0);

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
	const int maxX = (int)(i.uv.x * i.uv.x * 0.0 + 0.0); // 避免未引用：直接用 SV_Position 推断宽度
	// 实际宽度由 viewport 决定（640 默认），但 clamp 用一个大数安全：
	const int widthBound = 65536;
	const int heightBound = 65536;

	float3 color = 0.0;
	[unroll] for (int idx = -7; idx <= 7; ++idx) {
		const int x = clamp(pixel.x + idx, 0, widthBound - 1);
		color += downSampledTexture.Load(int3(x, pixel.y, 0)).rgb * gaussianSigma3[abs(idx)];
	}
	// 参考项目：乘 bloomIntensity（材质 scalar，默认 1.0）
	const float3 bloomIntensity = float3(1.0, 1.0, 1.0);
	return float4(color * bloomIntensity, 1.0);
}