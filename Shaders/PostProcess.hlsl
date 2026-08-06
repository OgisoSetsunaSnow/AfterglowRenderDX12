// Post-process pass: tonemap + distance fog + bloom
// + 10d-2: chromatic aberration / atmospheric dust / vignette / skylight shaft.
// Functions ported from reference project PostEffects.hlsl / Random.hlsl / Depth.hlsl.
// Note: reference Chromatics ACES is incomplete, use Narkowicz Fitted ACES instead.

cbuffer PostParams : register(b0) {
	float4 cameraPosition;
	float4 fogColor;
	float4x4 view;            // view matrix (skylight shaft: dirLight -> CS)
	float4 dirLightDirection; // world dir light (skylight shaft)
	float nearZ;
	float farZ;
	float fogDensity;
	float time;               // animation time (dust / shaft hash)
	uint effects;             // bit0=chromatic aberration bit1=dust bit2=vignette
	uint _pad;
	float2 invScreenResolution; // 1/w, 1/h (shaft clamp)
	float2 _pad2;
};

Texture2D sceneTexture : register(t0);
Texture2D bloomTexture : register(t1);
Texture2D depthTexture : register(t2);
SamplerState sceneSampler : register(s0);

// ---- helpers (from reference Random.hlsl / ColorConversion.hlsl) ----
static const float randomSeed0 = 1312.21551f;

float2 Snorm(float2 x) { return x * 2.0 - 1.0; }
float3 Snorm(float3 x) { return x * 2.0 - 1.0; }

float Grayscale(float3 color) {
	return color.r * 0.299 + color.g * 0.587 + color.b * 0.114;
}

float3 Desaturation(float3 color, float value) {
	return lerp(color, Grayscale(color), value);
}

float3 Hash3D(float3 xyz, float seed) {
	return frac(sin(float3(
		dot(xyz, float3(83.7247, 71.7823, 24.274)),
		dot(xyz, float3(64.4634, 49.4349, 82.263)),
		dot(xyz, float3(94.262, 20.9245, 34.8256))
	)) * 52567.0925 + seed);
}

float2 Hash2D(float2 xy, float seed) {
	return frac(sin(float2(
		dot(xy, float2(46.531, 91.4653)),
		dot(xy, float2(64.4634, 49.4349))
	)) * 13127.643 + seed);
}

// ---- 10d-2 chromatic aberration (reference SceneColorChromaticAberration) ----
// intensity 0.01: edge G/B offset ~6px (reference 0.0025 too faint for our scene).
float3 SceneColorChromaticAberration(float2 uv, float3 sceneColor, float intensity = 0.01) {
	float2 uvOffset = (uv - float2(0.5, 0.5)) * intensity;
	float sceneColorG = sceneTexture.Sample(sceneSampler, uv - uvOffset).g;
	float sceneColorB = sceneTexture.Sample(sceneSampler, uv - uvOffset * 2).b;
	return float3(sceneColor.r, sceneColorG, sceneColorB);
}

// ---- 10d-2 atmospheric dust (reference AtmosphericDust) ----
// Colorless fine noise, far stronger, full screen.
//   baseNoise = Snorm(Desaturation(Hash3D))  noise [-1,1]
//   any(abs(noise)>0.4) ? 0 : noise          keep ~22% pixels
//   * min(depth*0.001, 0.1)                  depth fade
// Our factor 0.001 -> 0.03 (5m scene ~15% visible; reference terrain is huge).
float3 AtmosphericDust(float2 uv, float sceneDepth, float intensity = 0.4) {
	float timeSeed = frac(floor(time * 20.0) * 0.05);
	float3 baseNoise = Snorm(Desaturation(Hash3D(float3(uv, timeSeed), randomSeed0), 0.5));
	baseNoise = any(abs(baseNoise) > intensity) ? 0 : baseNoise;
	return baseNoise * min((sceneDepth * 0.03), 0.2);
}

// ---- 10d-2 vignette (reference Vignette) ----
float Vignette(float2 uv, float intensity = 0.75, float hardness = 0.5) {
	hardness *= 0.5;
	return 1.0 - smoothstep(min(hardness, 0.5), max(1.0 - hardness, 0.501), saturate(distance(0.5, uv) * intensity));
}

// ---- 10d-2 skylight shaft (reference SkylightShaft) ----
// Radial blur of bloom along light direction, only where depth is far (sky).
// Reference uses reversed depth (<0.0001 = far); ours uses standard depth (1 = far) -> >0.9.
float3 SkylightShaft(float3 bloomColor, float2 uv, float sceneDepth, float intensity = 0.25, uint layerCount = 8) {
	const float invLayerCount = rcp(layerCount);
	const float layerInterval = 0.32 * invLayerCount;
	const float hashScale = 0.16 * invLayerCount;
	float3 radialBlurColor = 0.0;
	const float3 dirLightDirectionCS = mul(view, dirLightDirection).xyz;
	const float2 radialBlurOffset = float2(layerInterval, -layerInterval) * dirLightDirectionCS.xy * (1.0 - dirLightDirectionCS.z)
		+ Snorm(uv) * min(dirLightDirectionCS.z, 0.0) * 0.02;
	const float2 screenLightDirection = normalize(dirLightDirectionCS.xy + 1e-6);
	for (uint index = 1; index < layerCount + 1; ++index) {
		float2 radialBlurUV = uv + index * radialBlurOffset;
		const float2 radialHash = hashScale * Snorm(Hash2D(uv + index, frac(time) * randomSeed0)) * screenLightDirection;
		radialBlurUV += radialHash;
		radialBlurUV = clamp(radialBlurUV, invScreenResolution, 1.0 - invScreenResolution);
		const float isFar = step(0.9, depthTexture.Sample(sceneSampler, radialBlurUV).r);
		radialBlurColor += bloomColor * isFar;
	}
	radialBlurColor *= invLayerCount;
	return radialBlurColor * intensity * min(sceneDepth * 0.5, 1.0);
}

// ---- ACES tonemap (Narkowicz Fitted RRT+ODT) ----
float3 ACESToneMap(float3 color) {
	color = (color * (color + 0.0245786) - 0.000090537)
		/ (color * (0.983729 * color + 0.4329510) + 0.238081);
	return saturate(color);
}

struct VSOut {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
};

// Full-screen triangle. NDC y=+1 top, uv.y=0 texture top -> flip pos.y.
VSOut VSMain(uint vid : SV_VertexID) {
	VSOut o;
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
	float3 sceneColor = sceneTexture.Sample(sceneSampler, i.uv).rgb;
	const float3 bloomColor = bloomTexture.Sample(sceneSampler, i.uv).rgb;

	// chromatic aberration (pre-tonemap, reference order). key 1.
	if ((effects & 1u) != 0u) {
		sceneColor = SceneColorChromaticAberration(i.uv, sceneColor);
	}

	// linear depth (standard Z, reversed perspective formula).
	const float depthNdc = depthTexture.Sample(sceneSampler, i.uv).r;
	const float ndcZ = depthNdc * 2.0 - 1.0;
	const float linearDepth = (2.0 * nearZ * farZ) / (farZ + nearZ - ndcZ * (farZ - nearZ));
	// sky (depth=1 -> linearDepth~far) should not be fogged.
	const float farThreshold = farZ * 0.85;
	const float fog = (linearDepth < farThreshold)
		? saturate(1.0 - exp(-linearDepth * fogDensity))
		: 0.0;

	// pre-tonemap combine: scene + bloom + dust + shaft + vignette (reference order).
	float3 preTonemap = sceneColor + bloomColor;
	preTonemap += SkylightShaft(bloomColor, i.uv, linearDepth, 0.6); // light shaft, intensity 0.6 (was 0.25 too faint)
	if ((effects & 2u) != 0u) {
		preTonemap += AtmosphericDust(i.uv, linearDepth); // key 2
	}
	if ((effects & 4u) != 0u) {
		preTonemap *= Vignette(i.uv); // key 3
	}

	const float3 tonemapped = ACESToneMap(preTonemap);
	const float3 finalColor = lerp(fogColor.rgb, tonemapped, 1.0 - fog);
	return float4(finalColor, 1.0);
}
