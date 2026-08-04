// 阶段 8-9：法线（逆转置变换）+ 方向光 + Toon 着色。布局与 PerObjectCB/FrameCB 对齐。
cbuffer PerObject : register(b0) {
	float4x4 viewProj;      // proj * view（列主序，与 glm 一致）
	float4x4 world;         // 世界矩阵
	float4x4 invTransModel; // world 的逆转置（法线变换用）
	float4 baseColor;
	float outlineWidth;     // 描边宽度（描边 shader 用，此处保持布局一致）
	float3 _padPerObject;
};

cbuffer Frame : register(b1) {
	float4 lightDir;              // 世界空间：指向光源的单位方向
	float4 lightColor;            // 光源颜色（w = 强度，同参考项目 dirLightColor.a）
	float4 ambientColor;          // 环境光
	float4 cameraPosition;        // 世界空间相机位置
	float invProjection00;        // 1/proj[0][0]，描边 FOV 补偿
	float invScreenAspectRatio;   // 高/宽
	float outlineDistanceFadeFactor; // 描边距离衰减
	float _padFrame;
};

Texture2D baseTexture : register(t0);
SamplerState baseSampler : register(s0);

struct VSIn {
	float3 pos : POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
	float4 color : COLOR;
};

struct PSIn {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
	float3 worldNormal : NORMAL; // 世界空间法线（未归一化，PS 内归一）
	float4 color : COLOR;
};

PSIn VSMain(VSIn input) {
	PSIn output;
	output.pos = mul(viewProj, mul(world, float4(input.pos, 1.0)));
	output.uv = input.uv;
	// 法线用逆转置矩阵变换（参考项目 invTransModel），非均匀缩放也正确。
	output.worldNormal = mul((float3x3)invTransModel, input.normal);
	output.color = input.color;
	return output;
}

float4 PSMain(PSIn input) : SV_TARGET {
	const float3 n = normalize(input.worldNormal);
	const float3 l = normalize(lightDir.xyz);
	const float ndl = saturate(dot(n, l));

	// Toon 分段：三档色阶。
	float3 toon;
	if (ndl > 0.85f) {
		toon = float3(1.0, 1.0, 1.0);    // 高光段
	} else if (ndl > 0.45f) {
		toon = float3(0.7, 0.7, 0.7);    // 中间段
	} else {
		toon = float3(0.35, 0.35, 0.35); // 阴影段
	}

	const float3 albedo = baseTexture.Sample(baseSampler, input.uv).rgb;
	// 光强乘 w（同参考项目 dirLightColor.xyz * dirLightColor.a）。
	const float3 lit = albedo * (ambientColor.rgb + lightColor.rgb * lightColor.a * toon);
	return float4(lit * input.color.rgb * baseColor.rgb, 1.0);
}
