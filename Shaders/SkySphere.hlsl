// SkySphere 天空盒专用 Unlit shader。
// 对齐参考项目：SkySphere.mat = SkySphere_VS.hlsl + Unlit_FS.hlsl（无光照，直接采样天空纹理）。
// 关键：VS 在世界空间加相机位置 → 球心永远在相机处，相机始终被天空包住（无穷远天空效果）。
// UV 采用方向向量程序化计算（equirectangular），不依赖 fbx UV 朝向，彻底解决球面 UV 错位问题。
// cbuffer 布局与 MeshRenderer PerObjectCB/FrameCB 对齐（复用同一根签名）。
cbuffer PerObject : register(b0) {
	float4x4 viewProj;
	float4x4 world;
	float4x4 invTransModel;
	float4 baseColor;
	float outlineWidth;
	float3 _padPerObject;
};

cbuffer Frame : register(b1) {
	float4 lightDir;
	float4 lightColor;
	float4 ambientColor;
	float4 cameraPosition;   // 世界空间相机位置（SkySphere VS 用）
	float invProjection00;
	float invScreenAspectRatio;
	float outlineDistanceFadeFactor;
	float _padFrame;
};

Texture2D baseTexture : register(t0);
SamplerState baseSampler : register(s0);

struct VSIn {
	float3 pos : POSITION;
	float2 uv : TEXCOORD; // 仍用于 input layout 对齐 VB stride，仅占位
};

struct PSIn {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
};

PSIn VSMain(VSIn input) {
	PSIn output;
	// 参考项目 SkySphere_VS.hlsl：world transform + cameraPosition（球心跟随相机）。
	float3 worldPos = mul(world, float4(input.pos, 1.0)).xyz + cameraPosition.xyz;
	output.pos = mul(viewProj, float4(worldPos, 1.0));

	// 球面方向 = 球面顶点 - 球心（相机）；球心=相机，所以方向 = worldPos - cameraPosition，
	// 因 worldPos 本身已是 (scale*模型pos + cameraPos) 形式，方向 = scale*模型pos。
	float3 dir = normalize(worldPos - cameraPosition.xyz);

	// equirectangular 标准全景图投影（行业标准 SkySphere，不依赖 fbx UV 方向）：
	//   u = 0.5 + atan2(x, z) / 2π   水平方位 -> [0,1]
	//   v = 0.5 - asin(y) / π        +Y(顶) -> v=0(天顶), -Y(底) -> v=1(地面)
	// DefaultSky.png 采样验证：v=0=R12G83B214 蓝(天顶), v=0.5=R254G232B215 米黄(地平线), v=1=R1G11B49 暗紫(地面)。
	// 相机正前方 (0,0,1) -> u=0.5, v=0.5 -> 采样地平线米黄区域（正确）。
	float u = 0.5 + atan2(dir.x, dir.z) / 6.2831853;
	float v = 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265;
	output.uv = float2(u, v);
	return output;
}

float4 PSMain(PSIn input) : SV_TARGET {
	// 参考项目 Unlit_FS.hlsl：albedoTex.Sample(...)，无光照。
	return baseTexture.Sample(baseSampler, input.uv);
}
