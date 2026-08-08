// Yvonne EyeShadow：照搬参考项目 EndfieldEyeShadow_FS.hlsl（黑色眼影 + uv.y 渐变 alpha）。
#include "CharacterCommon.hlsl"

float4 PSMain(UnlitFSInput input) : SV_TARGET {
	float4 color;
	color.xyz = 0.0;
	color.a = 1.0 - input.texCoord0.y;
	return color;
}
