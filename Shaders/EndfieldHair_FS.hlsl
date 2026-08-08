// Yvonne Hair 材质：照搬参考项目 Shaders/Toon/EndfieldHair_FS.hlsl。
// 5 纹理 albedo + hn + property + st + hairRamp；双法线 + 各向异性 GGX。
#include "CharacterCommon.hlsl"
#include "Common.hlsl"
#include "ColorConversion.hlsl"
#include "ShadingModels.hlsl"
#include "EndfieldCommon.hlsl"

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D hnTex : register(t1);
SamplerState hnTexSampler : register(s1);
Texture2D propertyTex : register(t2);
SamplerState propertyTexSampler : register(s2);
Texture2D stTex : register(t3);
SamplerState stTexSampler : register(s3);
Texture2D hairRampTex : register(t4);
SamplerState hairRampTexSampler : register(s4);

float4 PSMain(VSOutput input, bool isFrontFace : SV_ISFRONTFACE) : SV_TARGET {
	float4 baseColor = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float4 property = propertyTex.Sample(propertyTexSampler, input.texCoord0);

	float3x3 tbn = {
		input.worldTangent,
		input.worldBitangent,
		input.worldNormal
	};
	tbn = isFrontFace ? tbn : -tbn;
	float4 hn = hnTex.Sample(hnTexSampler, input.texCoord0);
	float3 normal = mul(ReconstructNormal(DecodeNormal(hn.xy)), tbn);
	float3 normalH = mul(ReconstructNormal(DecodeNormal(hn.zw)), tbn);

	float4 st = stTex.Sample(stTexSampler, input.texCoord0);

	float3 view = normalize(cameraPosition - input.worldPosition);

	// Gram-Schmidt Orthogonization
	float3 tangent = normalize(tbn[0] - dot(tbn[0], normal) * normal);
	float3 bitangent = cross(property.x >= 0.5 ? normal : normalH, tangent);

	// Rim light
	float3 rimLighting = EndfieldRimLighting(input.worldNormal, view, property.w, property.z, materialParams.x);

	// Hight light color ramp
	float2 hairRampCoord = saturate(float2(abs(dot(normal, dirLightDirection.xyz)), 0.5));
	float4 hairRamp = hairRampTex.Sample(hairRampTexSampler, hairRampCoord);

	ShadingContext shadingContext = (ShadingContext)0;
	shadingContext.baseColor = baseColor.xyz * property.z;
	shadingContext.metallic = 0.2;
	shadingContext.specular = 2.0 * property.y;
	shadingContext.roughness = 0.8;
	shadingContext.ambientOcclusion = property.z;
	shadingContext.anisotropy = 0.85;
	shadingContext.normal = normal;
	shadingContext.tangent = tangent;
	shadingContext.bitangent = bitangent;
	shadingContext.view = view;

	LightingResult lightingResult = DefaultShadingAnisotropic(shadingContext,
		normalize(dirLightDirection.xyz), dirLightColor, ambientTexture, ambientTextureSampler);

	static const float specularSolidity = 0.1;
	const float minSpecularClamp = min(specularSolidity, 0.5);
	const float maxSpecularClamp = max(1.0 - specularSolidity, 0.501);

	float3 finalColor =
		lightingResult.diffuse
		+ smoothstep(minSpecularClamp, maxSpecularClamp, lightingResult.specular) * (hairRamp * property.w) * st.x
		+ lightingResult.transmission;
	finalColor += rimLighting;

	return float4(finalColor, baseColor.a);
}
