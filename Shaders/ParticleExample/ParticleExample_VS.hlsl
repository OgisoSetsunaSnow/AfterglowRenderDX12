// ParticleExample 粒子 VS（照搬参考 ParticleExample_VS.hlsl 逻辑）。
// SV_VertexID 读 ParticleSSBO；点大小（像素）以自定义语义 POINTSIZE 传给 GS（D3D12 点精灵 SV_PointSize 不可靠，
// 改用 GS 公告板扩展成四边形）。viewProjection 为 z-up 版（proj*view*rotate(-90X)），粒子模拟在 z-up。
#include "ParticleCommon.hlsl"

cbuffer ParticleRenderParams : register(b0) {
	float4x4 viewProjection;   // z-up
	float2 screenResolution;
	float2 invScreenResolution;
	float cameraFov;           // 弧度
	float _pad0;
	float2 _pad1;
};

StructuredBuffer<ParticleSSBOStruct> ParticleSSBOIn : register(t0);

struct VSOutput {
	float4 clipPos : SV_POSITION;
	float3 velocity : VELOCITY;
	float4 color : COLOR;
	float pointSize : POINTSIZE; // 点直径（像素，GS 用）
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	ParticleSSBOStruct p = ParticleSSBOIn[vertexID];
	float4 clipPos = mul(viewProjection, float4(p.position, 1.0));
	output.clipPos = clipPos;
	output.velocity = p.velocity;
	output.color = p.color;
	// 注意：局部常量不能与结构体成员同名（kPointSize），否则 HLSL 可能解析为自引用 → 0。
	const static float kPointSize = 0.08; // 参考 ParticleExample_VS pointSize（小粒子）
	// 点直径 = kPointSize / w * 720 / fov（像素）。clamp 上限防贴相机粒子巨大 quad。
	output.pointSize = min(kPointSize / clipPos.w * screenResolution.y / cameraFov, 20.0);
	return output;
}
