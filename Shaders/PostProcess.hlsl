// 阶段 10b：全屏后处理 pass（参考项目 PostProcess_VS/FS 移植）。
// t0 sceneColor + t1 bloomCombinedTexture 合成。

Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
SamplerState sceneSampler : register(s0);

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
	// 参考项目 PostProcess_FS：`output.color.xyz += bloomColor;`
	const float3 bloomColor = bloomTexture.Sample(sceneSampler, i.uv).rgb;
	return float4(sceneColor + bloomColor, 1.0);
}