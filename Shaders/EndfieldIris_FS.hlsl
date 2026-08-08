// Yvonne Iris：简化版，参考参考项目 EndfieldIris_FS.hlsl（iridescent 角膜 + 虹膜）。
// 完整 matcap/角膜法线计算需要 matcap 纹理 + 角膜法线；当前简化用 albedo + 法线贴图 + irisDepth。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ShadingModels.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	static const float illuminanceCorrect = 0.5;

	// 简化：tangent-camera 偏移（参考项目 irisUVOffset + 角膜法线 + dirLight 漫反射）。
	float3x3 tbn = { input.worldTangent, input.worldBitangent, input.worldNormal };
	float3 tbnN = mul(tbn, normalize(input.worldPosition - cameraPosition));
	float irisUVOffset = (1.0 - distance(input.texCoord0, 0.5) * 2.0) * materialParams3.y;
	float2 irisViewOffset = float2(tbnN.x, -tbnN.y);
	irisViewOffset.y = input.color.x > 0 ? irisViewOffset.y : -irisViewOffset.y;
	float2 irisUV = input.texCoord0 + irisViewOffset * irisUVOffset;

	float4 baseColor = albedoTex.Sample(albedoTexSampler, irisUV);

	// 简化的 eye shading：直接 albedo * dirLight + ambient，没有完整 IBL。
	float3 normal = float3(0, 0, 1);
	float3 view = normalize(cameraPosition - input.worldPosition);
	ShadingContext ctx = (ShadingContext)0;
	ctx.baseColor = baseColor.xyz;
	ctx.metallic = 0.5;
	ctx.specular = 0.0;
	ctx.roughness = 1.0;
	ctx.ambientOcclusion = 1.0;
	ctx.normal = normal;
	ctx.view = view;
	LightingResult lit = DefaultShading(ctx, normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, ambientTextureSampler);

	float3 finalColor = lit.diffuse + lit.specular;
	finalColor *= dirLightColor.xyz * dirLightColor.w * illuminanceCorrect;

	return float4(finalColor, 1.0);
}
