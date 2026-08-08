// Yvonne 描边：照搬参考项目 Shaders/Toon/EndfieldOutline_VS.hlsl。
// stTex 采样 st.x 做外扩权重；clamp 0.001。
#include "CharacterCommon.hlsl"

Texture2D stTex : register(t3);
SamplerState stTexSampler : register(s3);

UnlitVSOutput VSMain(VSInput input) {
	UnlitVSOutput output;
	float4 worldPosition = mul(model, float4(input.pos, 1.0));
	float3 worldNormal = normalize(mul((float3x3)invTransModel, input.normal));

	// Expand world position by normal.
	float distanceFade = min(distance(cameraPosition, worldPosition.xyz) * outlineParams.y, 0.001);
	float fovFade = invProjection[0][0] * invScreenAspectRatio;

	float4 st = stTex.SampleLevel(stTexSampler, input.uv, 0);

	worldPosition.xyz += outlineParams.x * distanceFade * fovFade * worldNormal * st.x;

	output.position = mul(viewProjection, worldPosition);
	output.texCoord0 = input.uv;
	return output;
}
