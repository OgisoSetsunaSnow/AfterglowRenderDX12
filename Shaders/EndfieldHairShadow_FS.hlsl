// Yvonne HairShadow FS：照搬参考项目 EndfieldHairShadow_FS.hlsl（shadowColor）。
#include "CharacterCommon.hlsl"

float4 PSMain(VSOutput input) : SV_TARGET {
	// shadowColor 来自 outlineColor（参考项目 .mat vectors shadowColor）。
	return float4(outlineColor.xyz, outlineColor.w);
}
