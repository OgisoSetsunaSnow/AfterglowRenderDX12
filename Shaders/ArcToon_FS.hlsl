// BattleMage 材质：照搬参考项目 Shaders/Toon/ArcToon_FS.hlsl。
// 3 纹理 albedoTex + sssTex + ilmTex；动漫硬色阶 + GGX + EnvLighting(IBL) + ilm 通道控制。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ColorConversion.hlsl"
#include "ShadingModels.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D sssTex : register(t1);
SamplerState sssTexSampler : register(s1);
Texture2D ilmTex : register(t2);
SamplerState ilmTexSampler : register(s2);

struct FSOutput {
	float4 color : SV_TARGET;
};

FSOutput PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) {
	FSOutput output;
	static const float luminanceCorrect = 1.0;

	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float4 sssColor = sssTex.Sample(sssTexSampler, input.texCoord0);
	float4 ilm = ilmTex.Sample(ilmTexSampler, input.texCoord0);

	float3 normal = input.worldNormal;

	float3 view = normalize(cameraPosition.xyz - input.worldPosition);

	BxDFContext bxdfContext = (BxDFContext)0;
	InitBxDFContext(bxdfContext, normal, view, dirLightDirection.xyz);
	bxdfContext.nol = saturate(bxdfContext.nol);
	bxdfContext.nov = saturate(abs(bxdfContext.nov) + 1e-6);
	float diffuse = bxdfContext.nol * input.color.x;
	diffuse = smoothstep(0.49, 0.50, diffuse);

	float specular = 0.5;
	float metallic = baseColor.a;
	float roughness = 0.5;

	float3 specularColor = ComputeF0(specular, baseColor.xyz, metallic);

	float3 reflectionVector = reflect(-view, normal);
	LightingResult envLighting = EnvLighting(
		reflectionVector, baseColor.xyz, specularColor, bxdfContext.nov, 1.0, metallic,
		ambientTexture, ambientTextureSampler
	);

	float3 ggx = SpecularGGX(bxdfContext, roughness, specularColor);
	ggx = smoothstep(0.04, 0.05, ggx * ilm.z);
	ggx += envLighting.specular;

	float3 finalColor = lerp(sssColor, baseColor, diffuse).xyz;
	finalColor += ggx;
	finalColor += envLighting.diffuse;

	// Internal lines
	finalColor *= ilm.w;
	// Light effect
	finalColor *= dirLightColor.xyz * dirLightColor.w;
	// Color correct
	finalColor = finalColor * luminanceCorrect * ilm.y;

	output.color.xyz = finalColor;
	return output;
}
