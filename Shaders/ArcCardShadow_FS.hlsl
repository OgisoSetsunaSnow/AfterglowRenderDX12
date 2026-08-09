// BattleMage 卡面阴影（照搬参考项目 Shaders/Toon/ArcCardShadow_FS.hlsl）。
// 参考：output.color = float4(color.xyz, (1.0 - input.color.w) * color.w);
//   color = 材质 uniform 向量（BattleMageCardShadow.mati color=(0,0,0,0.65)），
//   input.color.w = 顶点色 alpha（FBX 烘焙的阴影淡出梯度）。
// 本工程 color 槽复用 MeshUniform.outlineColor（见 Main.cpp BattleMage mesh1）。
// Domain=Transparency：alpha blend + 不写深度（CharacterMaterialDesc blend=true depthWrite=false）。
#include "CharacterCommon.hlsl"

struct FSOutput {
	float4 color : SV_TARGET;
};

FSOutput PSMain(VSOutput input) {
	FSOutput output;
	output.color = float4(outlineColor.xyz, (1.0 - input.color.w) * outlineColor.w);
	return output;
}
