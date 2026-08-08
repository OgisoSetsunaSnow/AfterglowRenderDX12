// Yvonne 描边 FS：照搬参考项目 EndfieldOutline_FS.hlsl（outlineColor 材质参数）。
#include "CharacterCommon.hlsl"

float4 PSMain(UnlitFSInput input) : SV_TARGET {
	// 参考项目 EndfieldOutline_FS: output.color = outlineColor。
	return float4(outlineColor.xyz, 1.0);
}
