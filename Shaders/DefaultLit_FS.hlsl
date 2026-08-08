// Ankha / Standard 材质：照搬参考项目 Shaders/DefaultLit_FS.hlsl。
// albedo + normal 法线贴图 + SubsurfaceShadingAnisotropic（PBR + 次表面）。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ColorConversion.hlsl"
#include "ShadingModels.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D normalTex : register(t1);
SamplerState normalTexSampler : register(s1);

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	float3x3 tbn = {
		input.worldTangent,
		input.worldBitangent,
		input.worldNormal
	};
	tbn = isFrontFace ? tbn : -tbn;

	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float3 normal = mul(DecodeNormal(normalTex.Sample(normalTexSampler, input.texCoord0).xyz), tbn);

	// Gram-Schmidt Orthogonization
	float3 tangent = normalize(tbn[0] - dot(tbn[0], normal) * normal);
	float3 bitangent = cross(normal, tangent);

	float3 view = normalize(cameraPosition - input.worldPosition);

	ShadingContext shadingContext = (ShadingContext)0;
	shadingContext.baseColor = baseColor.xyz;
	shadingContext.metallic = materialParams2.x;
	shadingContext.specular = materialParams2.y;
	shadingContext.roughness = materialParams2.z;
	shadingContext.ambientOcclusion = 1.0;
	shadingContext.anisotropy = materialParams2.w;
	shadingContext.normal = normal;
	shadingContext.tangent = tangent;
	shadingContext.bitangent = bitangent;
	shadingContext.view = view;
	shadingContext.subsurfaceColor = baseColor.xyz;
	shadingContext.opacity = materialParams3.x;

	// 参考项目 DefaultLit_FS 用 SubsurfaceShadingAnisotropic。
	LightingResult lightingResult = SubsurfaceShadingAnisotropic(shadingContext,
		normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, ambientTextureSampler);
	float3 finalColor = lightingResult.diffuse + lightingResult.specular + lightingResult.transmission;

	return float4(finalColor, baseColor.a);
}
