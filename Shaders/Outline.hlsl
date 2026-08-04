// 阶段 9：背面外扩描边。思路参考参考项目 ArcOutline_VS：
// 世界空间沿法线外扩，距离衰减 clamp(0.01) + FOV 补偿，剔除正面只画背面。
cbuffer PerObject : register(b0) {
	float4x4 viewProj;
	float4x4 world;
	float4x4 invTransModel;
	float4 baseColor;
	float outlineWidth; // 世界空间外扩宽度（含 distanceFade*fovFade 归一）
	float3 _padPerObject;
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

	// 距离衰减：远处轮廓淡出（参考项目 outlineDistanceFadeFactor）。
	const float distanceFade = min(
		distance(cameraPosition.xyz, worldPos.xyz) * outlineDistanceFadeFactor, 0.01);
	// FOV 补偿（参考项目 invProjection[0][0] * invScreenAspectRatio）。
	const float fovFade = invProjection00 * invScreenAspectRatio;

	worldPos.xyz += outlineWidth * (distanceFade * fovFade) * worldNormal;
	output.pos = mul(viewProj, worldPos);
	return output;
}

float4 PSMain(PSIn input) : SV_TARGET {
	return float4(0.06, 0.06, 0.08, 1.0); // 深色描边
}
