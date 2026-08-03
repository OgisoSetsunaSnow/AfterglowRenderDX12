// 阶段 5：带材质参数（baseColor）的纹理采样着色。
cbuffer PerObject : register(b0) {
	float4x4 mvp;      // 列主序（与 glm 布局一致）
	float4 baseColor;  // 材质基础色
};

Texture2D baseTexture : register(t0);
SamplerState baseSampler : register(s0);

struct VSIn {
	float3 pos : POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

struct PSIn {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

PSIn VSMain(VSIn input) {
	PSIn output;
	output.pos = mul(mvp, float4(input.pos, 1.0));
	output.uv = input.uv;
	output.color = input.color;
	return output;
}

float4 PSMain(PSIn input) : SV_TARGET {
	return baseTexture.Sample(baseSampler, input.uv) * input.color * baseColor;
}
