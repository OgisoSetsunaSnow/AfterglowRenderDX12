// ShellFur instanced FS, ported 1:1 from reference ShellFur/ShellFur_FS.hlsl.
// 用 FractalWorleyNoise 做毛发生长噪声 + 按 instanceID 裁剪（clip），FractalNormal.zw 做毛法线。
#include "../Common.hlsl"
#include "../Random.hlsl"

cbuffer FurUniform : register(b0) {
	float4x4 model;
	float4x4 invTransModel;
	float4x4 viewProjection;
	float objectID;
	float instanceCount;
	float shellInterval;
	float _pad;
};

cbuffer FurGlobalUniform : register(b1) {
	float3 cameraPosition;
	float _gpad0;
	float3 dirLightDirection;
	float _gpad1;
	float4 dirLightColor;   // rgb color, w = intensity
};

Texture2D albedoTex : register(t0);
SamplerState albedoTexSampler : register(s0);
Texture2D normalTex : register(t1);
SamplerState normalTexSampler : register(s1);
Texture2D<float> FractalWorleyNoise : register(t2);
SamplerState FractalWorleyNoiseSampler : register(s2);
Texture2D FractalNormal : register(t3);   // .rg simplex normal, .ba worley normal
SamplerState FractalNormalSampler : register(s3);

struct VSOutput {
	float4 position : SV_POSITION;
	float3 worldPosition : POSITION;
	float3 worldNormal : NORMAL;
	float3 worldTangent : TANGENT;
	float3 worldBitangent : BITANGENT;
	uint objectID : OBJECT_ID;
	uint instanceID : INSTANCE_ID;
	float4 color : COLOR;
	float2 texCoord0 : TEXCOORD;
};

float4 PSMain(VSOutput input) : SV_TARGET {
	static const float furThickness = 1.5;

	float2 furCoord = (input.texCoord0 - float2(0.0, input.instanceID * 0.001)) * float2(16.0, 16.0);
	float noise = FractalWorleyNoise.SampleLevel(FractalWorleyNoiseSampler, furCoord, 0.0);
	// @note: clip 会禁用 early-z（参考同款 TODO）。
	if (max(furThickness - noise, 0.0) < (input.instanceID + 0.5) * rcp(instanceCount)) {
		clip(-1);
	}

	float3x3 tbn = {
		input.worldTangent,
		input.worldBitangent,
		input.worldNormal
	};

	float4 albedo = albedoTex.Sample(albedoTexSampler, input.texCoord0);
	float3 normal = LoadNormalTexture(normalTex, normalTexSampler, input.texCoord0);
	float3 furNormal = ReconstructNormal(FractalNormal.SampleLevel(FractalNormalSampler, furCoord, 0).zw);
	normal = mul(BlendAngleCorrectedNormals(normal, furNormal), tbn);

	float3 view = normalize(cameraPosition - input.worldPosition);
	float vertNov = dot(normal, view);
	float vol = dot(-view, dirLightDirection.xyz);
	float radiance = max(dot(normal, dirLightDirection.xyz), 0.0);

	float3 finalColor = lerp(albedo.rgb * 0.2, albedo.rgb, radiance);
	finalColor *= Unorm(1.0 - noise);
	finalColor += Pow4(1.0 - vertNov) * albedo.rgb * Unorm(vol);

	static const float luminanceCorrect = 0.5;
	finalColor *= dirLightColor.xyz * dirLightColor.w * luminanceCorrect;

	return float4(finalColor, 1.0);
}
