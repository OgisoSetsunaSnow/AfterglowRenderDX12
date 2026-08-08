// 简单 Unlit VS（照搬参考项目 Unlit_VS.hlsl），EyeShadow 等无光照材质用。
#include "CharacterCommon.hlsl"

UnlitVSOutput VSMain(VSInput input) {
	UnlitVSOutput output;
	output.position = mul(viewProjection, mul(model, float4(input.pos, 1.0)));
	output.texCoord0 = input.uv;
	return output;
}
