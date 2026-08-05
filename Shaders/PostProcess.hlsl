// 阶段 10b-10d：全屏后处理 pass。
// t0 sceneColor + t1 bloomCombinedTexture + t2 depth + ACES tonemap + 距离雾。
// 注：参考项目 Chromatics ACES（ACES2 + LUT 烘焙）为未完成实现（HLSL 引用未声明 SSBO），
// 无法复现。此处采用行业标准 Narkowicz Fitted ACES tone curve（去掉色彩空间矩阵，
// 因场景为 sRGB linear），达成 ACES tonemap 效果。

cbuffer PostParams : register(b0) {
	float4 cameraPosition;
	float4 fogColor;
	float nearZ;
	float farZ;
	float fogDensity;
	float _pad;
};

Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
Texture2D depthTexture : register(t2);
SamplerState sceneSampler : register(s0);

// ---- ACES tonemap（Narkowicz Fitted RRT+ODT curve）----
float3 ACESToneMap(float3 color) {
	// RRT + ODT 拟合曲线（Krzyztof Narkowicz "fitted"）。
	color = (color * (color + 0.0245786) - 0.000090537)
		/ (color * (0.983729 * color + 0.4329510) + 0.238081);
	return saturate(color);
}

struct VSOut {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
};

// 全屏三角形：NDC y=+1 顶部、uv.y=0 纹理顶部 → 翻转 pos.y 避免画面颠倒。
VSOut VSMain(uint vid : SV_VertexID) {
	VSOut o;
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
	const float3 sceneColor = sceneTexture.Sample(sceneSampler, i.uv).rgb;
	const float3 bloomColor = bloomTexture.Sample(sceneSampler, i.uv).rgb;

	// ACES tonemap。
	const float3 tonemapped = ACESToneMap(sceneColor + bloomColor);

	// 阶段 10d：深度距离雾。
	const float depthNdc = depthTexture.Sample(sceneSampler, i.uv).r; // 0..1 非线性
	const float ndcZ = depthNdc * 2.0 - 1.0;
	// 线性深度（标准逆向 Z 透视公式）。
	const float linearDepth = (2.0 * nearZ * farZ) / (farZ + nearZ - ndcZ * (farZ - nearZ));
	// 天空盒距离极远（depth=1, linearDepth≈far），不应被雾覆盖。
	// 设计选择：linearDepth >= far*0.85 视为天空，雾因子=0（天空始终可见）。
	const float farThreshold = farZ * 0.85;
	const float fog = (linearDepth < farThreshold)
		? saturate(1.0 - exp(-linearDepth * fogDensity))
		: 0.0;

	const float3 finalColor = lerp(fogColor.rgb, tonemapped, 1.0 - fog);
	return float4(finalColor, 1.0);
}