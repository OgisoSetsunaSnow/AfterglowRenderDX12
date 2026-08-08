// Ankha 身体 mask 顶点着色器（照搬 debug 版 Ankha.mat → Shaders/ShellFur/ShellFurMask_VS.hlsl）。
// 用顶点色 color.x（毛发生长遮罩）+ sizeExpand 沿法线外扩 + depthOffset 深度偏移（推近相机）。
// 标量槽位（本工程 MeshUniform.materialParams3）：z=sizeExpand(0.16), w=depthOffset(0.0002)。
#include "../CharacterCommon.hlsl"

VSOutput VSMain(VSInput input) {
	VSOutput output;
	float3 position = input.pos;

	position += input.normal * input.color.x * materialParams3.z; // sizeExpand

	float4 worldPosition = mul(model, float4(position, 1.0));
	output.position = mul(viewProjection, worldPosition);
	output.position.z -= materialParams3.w / output.position.w * input.color.x; // depthOffset

	output.worldPosition = worldPosition.xyz;
	output.worldNormal = normalize(mul(invTransModel, float4(input.normal, 0.0)).xyz);
	output.worldTangent = normalize(mul(invTransModel, float4(input.tangent, 0.0)).xyz);
	output.worldBitangent = cross(output.worldNormal, output.worldTangent);
	output.color = input.color;
	output.objectID = (uint)objectID;
	output.texCoord0 = input.uv;
	return output;
}
