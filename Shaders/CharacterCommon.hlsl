// Character material common: cbuffer layout ported from reference project
// GlobalUniform (UniformBufferObjects.h) + MeshUniform + per-material params.
// 槽位约定（共享 6 槽 SRV 表 t0-t5）：
//   t0 = albedo   t1 = normal/sss/hn   t2 = ramp/ilm/property   t3 = mask/st/property   t4 = highlight/hairRamp/ramp   t5 = ambient
#ifndef CHARACTER_COMMON_HLSL
#define CHARACTER_COMMON_HLSL

// ---- b0: MeshUniform (per object, ported from ubo::MeshUniform) ----
cbuffer MeshUniform : register(b0) {
	float4x4 model;          // Model to World
	float4x4 invTransModel;  // model.inverse().transpose()
	float3 minAABB;          // Object space AABB
	float objectID;
	float3 maxAABB;          // Object space AABB
	float indexCount;
	// 材质参数（参考项目 .mat/.mati scalars，按材质解释）
	float4 materialParams;   // Endfield: x=rimLightWidth y=rimLightIntensity z=subsurfaceMFP w=viewAttenuation
	float4 materialParams2;  // DefaultLit: x=metallic y=specular z=roughness w=anisotropy
	float4 materialParams3;  // x=subsurfaceOpacity y=irisDepth z=sizeExpand
	float4 outlineColor;     // EndfieldOutline.outlineColor / EndfieldHairShadow.shadowColor
	// 对象方向（参考项目 materialShared 注入 objectForward/objectRight/objectUp，EndfieldFace 用）
	float4 objectForward;
	float4 objectRight;
	float4 objectUp;
	// 描边参数（参考项目 .mat outlineWidth / outlineDistanceFadeFactor）
	float4 outlineParams; // x=outlineWidth y=outlineDistanceFadeFactor
};

// ---- b1: GlobalUniform (per frame, ported from ubo::GlobalUniform) ----
cbuffer GlobalUniform : register(b1) {
	float4x4 view;
	float4x4 projection;
	float4x4 viewProjection;
	float4x4 invView;
	float4x4 invProjection;
	float4x4 invViewProjection;
	float4 dirLightDirection;
	float4 dirLightColor;   // rgb color, w = intensity
	float3 cameraPosition;
	float cameraNear;
	float3 cameraVector;
	float cameraFar;
	float2 screenResolution;
	float2 invScreenResolution;
	float2 cursorPosition;
	float time;
	float deltaTime;
	float4 frustumPlaneL;
	float4 frustumPlaneR;
	float4 frustumPlaneB;
	float4 frustumPlaneT;
	float4 frustumPlaneN;
	float4 frustumPlaneF;
	float cameraFov;
	float screenAspectRatio;
	float invScreenAspectRatio;
	float frameIndex;
};

// 环境贴图（IBL/EnvLighting，参考项目 ambientTexture = DefaultSky equirect），共享 t5。
Texture2D ambientTexture : register(t5);
SamplerState ambientTextureSampler : register(s5);

struct VSInput {
	float3 pos : POSITION;
	float2 uv : TEXCOORD;
	float3 normal : NORMAL;
	float4 color : COLOR;
	float3 tangent : TANGENT;
};

struct VSOutput {
	float4 position : SV_POSITION;
	float3 worldPosition : POSITION;
	uint objectID : OBJECT_ID;
	float3 worldNormal : NORMAL;
	float3 worldTangent : TANGENT;
	float3 worldBitangent : BITANGENT;
	float4 color : COLOR;
	float2 texCoord0 : TEXCOORD;
};

// 参考项目 VertexStructs.hlsl 的 UnlitVSOutput/UnlitFSInput（描边/眼影等 unlit 材质共用）。
// VS 输出与 PS 输入必须结构一致（TEXCOORD 硬件寄存器位置对齐），否则 D3D12 #660 链接错误。
struct UnlitVSOutput {
	float4 position : SV_POSITION;
	float2 texCoord0 : TEXCOORD;
};

struct UnlitFSInput {
	float4 position : SV_POSITION;
	float2 texCoord0 : TEXCOORD;
	bool isFrontFace : SV_ISFRONTFACE;
};

#endif
