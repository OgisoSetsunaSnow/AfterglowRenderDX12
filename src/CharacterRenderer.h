#pragma once

#include "D3D12Core.h"
#include "Texture.h"

#include <glm/glm.hpp>

#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <vector>

class Mesh;

// 材质 ID（参考项目 .mat：BattleMage=ArcToon，Yvonne=Endfield*，Ankha=DefaultLit）。
// 顺序同时决定 PSO 索引与纹理组索引（createTextureHeap 按此布局）。
enum class CharacterMaterial : int {
	ArcToon = 0,        // BattleMage（albedo+sss+ilm）
	EndfieldBody,       // Yvonne Body/Neck（albedo+normal+ramp，EndfieldBody_VS）
	EndfieldPBR,        // Yvonne Cloth/Tail（albedo+normal+property，cull NONE）
	EndfieldThinFilm,   // Yvonne ThinFilm（PBR_FS + mask/ramp，半透明）
	EndfieldFace,       // Yvonne Face（albedo+sdf+ramp+mask+highlightMask）
	EndfieldHair,       // Yvonne Hair（albedo+hn+property+st+hairRamp）
	EndfieldIris,       // Yvonne Iris（albedo+matcap，EndfieldIris_VS）
	EndfieldEyelash,    // Yvonne Eyelash（albedo+ramp，半透明）
	EndfieldEyeShadow,  // Yvonne EyeShadow（无纹理，UnlitVS，半透明）
	EndfieldHairShadow, // Yvonne HairShadow（无纹理，HairShadow_VS，半透明）
	DefaultLit,         // Ankha（albedo+normal）
	Count
};

// 每材质 PSO 配置（对应参考项目 .mat：cullMode / domain / depthWrite）。
struct CharacterMaterialDesc {
	D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK; // 0=None 1=Front 2=Back
	bool depthWrite = true;                            // Transparency 域为 false
	bool blend = false;                                // Transparency 域 alpha blend
};

// 角色材质参数（对应 Shaders/CharacterCommon.hlsl cbuffer MeshUniform 的 materialParams*）。
struct CharacterMaterialParams {
	glm::vec4 materialParams{ 1.0f };  // Endfield: x=rimLightWidth y=rimLightIntensity z=subsurfaceMFP w=viewAttenuation
	glm::vec4 materialParams2{ 1.0f }; // DefaultLit: x=metallic y=specular z=roughness w=anisotropy
	glm::vec4 materialParams3{ 1.0f }; // x=subsurfaceOpacity y=irisDepth z=sizeExpand
	glm::vec4 outlineColor{ 0.0f, 0.0f, 0.0f, 1.0f }; // EndfieldOutline.outlineColor
	glm::vec4 outlineParams{ 10.0f, 0.001f, 0.0f, 0.0f }; // x=outlineWidth y=outlineDistanceFadeFactor
};

struct CharacterDrawItem {
	const Mesh* mesh = nullptr;
	glm::mat4 model{ 1.0f };
	CharacterMaterial material = CharacterMaterial::ArcToon;
	// 材质纹理组索引（0-based，对应构造 textures 参数里的材质分组）。
	UINT textureGroup = 0;
	// 描边用纹理组（EndfieldOutline_VS 采样 t3=stTex）；UINT_MAX 表示沿用 textureGroup。
	UINT outlineTextureGroup = UINT_MAX;
	CharacterMaterialParams params{};
	bool outline = false;
	glm::vec3 objectUp{ 0.0f, 1.0f, 0.0f };      // EndfieldFace 用（globalUp）
	glm::vec3 objectForward{ 0.0f, 0.0f, 1.0f };  // EndfieldFace 用（globalForward）
	glm::vec3 objectRight{ 1.0f, 0.0f, 0.0f };    // EndfieldFace 用（globalRight）
};

// 角色材质渲染器：严格照搬参考项目材质 shader（ArcToon/Endfield*/DefaultLit）。
// 根签名：b0 MeshUniform(CBV) + b1 GlobalUniform(CBV) + t0-t5 SRV 表 + s0-s5 采样器表。
// 纹理布局：每材质一组 6 槽 = [t0..t4 材质纹理, t5 ambient(DefaultSky)]。
class CharacterRenderer {
public:
	// materialVsBytes/materialFsBytes/materialDescs：按 CharacterMaterial 顺序，每材质独立 VS/FS/PSO 配置。
	// materialTextures：按材质分组的纹理数组（每组 5 个：albedo/t1/t2/t3/t4，缺的用占位纹理）。
	// ambientTex：DefaultSky（每组 t5 共享）。
	CharacterRenderer(D3D12Context& context,
		const std::vector<std::vector<std::uint8_t>>& materialVsBytes,  // 每材质 VS
		const std::vector<std::vector<std::uint8_t>>& materialFsBytes,  // 每材质 FS
		const std::vector<CharacterMaterialDesc>& materialDescs,        // 每材质 PSO 配置
		const std::vector<std::vector<std::uint8_t>>& outlineVsBytes,   // [ArcOutline, EndfieldOutline]
		const std::vector<std::vector<std::uint8_t>>& outlineFsBytes,
		DXGI_FORMAT rtvFormat,
		const std::vector<Texture*>& materialTextures, // 5*Count 个纹理
		const Texture& ambientTex);
	~CharacterRenderer();

	CharacterRenderer(const CharacterRenderer&) = delete;
	CharacterRenderer& operator=(const CharacterRenderer&) = delete;

	// 每帧绘制所有角色（outline pass 先画 + main pass）。
	void render(const glm::mat4& view, const glm::mat4& proj, const std::vector<CharacterDrawItem>& items,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv, const D3D12_CPU_DESCRIPTOR_HANDLE& dsv);

	void setLight(const glm::vec3& dir, const glm::vec3& color, float intensity);

private:
	void createRootSignature();
	void createPipelines(const std::vector<std::vector<std::uint8_t>>& materialVsBytes,
		const std::vector<std::vector<std::uint8_t>>& materialFsBytes,
		const std::vector<CharacterMaterialDesc>& materialDescs,
		const std::vector<std::vector<std::uint8_t>>& outlineVsBytes,
		const std::vector<std::vector<std::uint8_t>>& outlineFsBytes);
	void createTextureHeap(const std::vector<Texture*>& materialTextures, const Texture& ambientTex);
	void createCbvs();
	void createSamplers();
	// 每物体 MeshUniform 写入（含材质参数），返回写入偏移。
	UINT writeMeshUniform(char* dst, const CharacterDrawItem& item, UINT index, const glm::mat4& viewProj);

	D3D12Context& _context;
	DXGI_FORMAT _rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> _psos;
	std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> _outlinePsos; // [ArcOutline, EndfieldOutline]

	Microsoft::WRL::ComPtr<ID3D12Resource> _meshUniformCbv;   // 每物体 MeshUniform（stride 512）
	Microsoft::WRL::ComPtr<ID3D12Resource> _globalUniformCbv; // 每帧 GlobalUniform
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _textureHeap; // SRV 堆（每材质 6 槽，shader visible）
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
	UINT _srvDescriptorSize = 0;
	UINT _samplerDescriptorSize = 0;

	glm::vec3 _lightDir{ 0.0f, 1.0f, 0.0f };
	glm::vec3 _lightColor{ 1.0f, 1.0f, 1.0f };
	float _lightIntensity = 1.0f;

	static constexpr UINT CbvStride = 512; // MeshUniform（含材质参数）
};
