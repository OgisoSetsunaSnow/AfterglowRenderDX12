// 阶段 9：背面外扩描边。对齐参考项目 ArcOutline_VS / EndfieldOutline_VS：
// 世界空间沿法线外扩，距离衰减 clamp + FOV 补偿，剔除正面只画背面。
// 描边颜色来自 PerObjectCB.outlineColor（参考项目 EndfieldOutline.outlineColor，per-material）。
cbuffer PerObject : register(b0) {
	float4x4 viewProj;
	float4x4 world;
	float4x4 invTransModel;
	float4 baseColor;
	float outlineWidth;  // 世界空间外扩宽度
	float3 outlineColor; // 描边颜色（参考项目 EndfieldOutline.outlineColor）
};

cbuffer Frame : register(b1) {
	float4 lightDir;
	float4 lightColor;
	float4 ambientColor;
	float4 cameraPosition;
	float invProjection00;
	float invScreenAspectRatio;
	float outlineDistanceFadeFactor;
	float _padFrame;
};

struct VSIn {
	float3 pos : POSITION;
	float3 normal : NORMAL;
};

struct PSIn {
	float4 pos : SV_POSITION;
};

PSIn VSMain(VSIn input) {
	PSIn output;
	float4 worldPos = mul(world, float4(input.pos, 1.0));
	const float3 worldNormal = normalize(mul((float3x3)invTransModel, input.normal));

	// 距离衰减：远处轮廓变细（参考项目 outlineDistanceFadeFactor）。
	// clamp 上限 0.002：distance > 2m 后描边屏幕宽度随距离减小（否则远距离描边恒定变糊）。
	const float distanceFade = min(
		distance(cameraPosition.xyz, worldPos.xyz) * outlineDistanceFadeFactor, 0.002);
	// FOV 补偿（参考项目 invProjection[0][0] * invScreenAspectRatio）。
	const float fovFade = invProjection00 * invScreenAspectRatio;

	worldPos.xyz += outlineWidth * (distanceFade * fovFade) * worldNormal;
	output.pos = mul(viewProj, worldPos);
	return output;
}

float4 PSMain(PSIn input) : SV_TARGET {
	// 参考项目 EndfieldOutline_FS：output.color = outlineColor
	return float4(outlineColor, 1.0);
}
