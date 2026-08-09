// ParticleExample 点精灵 FS（照搬参考 ParticleExample_FS.hlsl）：圆形 alpha + sceneColor 折射混合 + TestTexture Load。
#include "ParticleCommon.hlsl"

cbuffer ParticleRenderParams : register(b0) {
	float4x4 viewProjection;
	float2 screenResolution;
	float2 invScreenResolution;
	float cameraFov;
	float _pad0;
	float2 _pad1;
};

Texture2D sceneColorTexture : register(t0);
SamplerState sceneColorTextureSampler : register(s0);
Texture2D TestTextureIn : register(t1);

struct VSOutput {
	float4 position : SV_POSITION;
	float3 velocity : VELOCITY;
	float4 color : COLOR;
	nointerpolation float2 centerPosition : CENTERPOSITION;
	nointerpolation float transferPointSize : TRANSFERPOINTSIZE;
};

float4 PSMain(VSOutput input) : SV_TARGET {
	float4 color;
	color.rgb = input.color.rgb;
	float circleAlpha = 1 - distance(input.centerPosition.xy, input.position.xy) / input.transferPointSize * 2;
	circleAlpha = smoothstep(0, 1, circleAlpha);

	// Storage image test.
	color.rg = lerp(color.rg, TestTextureIn.Load(int3(0, 0, 0)).rg, 0.5);

	float2 screenUV = input.position.xy * invScreenResolution;
	float2 distribUV = ((0.1 + input.centerPosition.xy - input.position.xy) / input.transferPointSize) * (1.0 - circleAlpha) * 0.2;
	float4 sceneColor = sceneColorTexture.Sample(sceneColorTextureSampler,
		clamp(screenUV + distribUV, invScreenResolution, 1.0 - invScreenResolution));
	color.xyz = lerp(sceneColor.xyz, color.xyz, circleAlpha * 0.25); // 参考：粒子色 25%（折射混合）
	color.a = input.color.a * circleAlpha; // 参考：淡入淡出 alpha
	return color;
}
