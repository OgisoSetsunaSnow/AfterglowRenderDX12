#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <wrl/client.h>
#include <cstdint>
#include <vector>

class Mesh;
class Texture;

// 每物体的常量缓冲内容（与 shader cbuffer PerObject 对应）。
struct PerObjectCB {
	glm::mat4 viewProj;      // 0-64（proj * view）
	glm::mat4 world;         // 64-128
	glm::mat4 invTransModel; // 128-192（world 的逆转置，法线/描边外扩用）
	glm::vec4 baseColor;     // 192-208
	float outlineWidth;      // 208-212（描边外扩宽度，世界单位）
	glm::vec3 outlineColor;  // 212-224（描边颜色，参考项目 EndfieldOutline.outlineColor）
};

// 每帧共享的常量缓冲（与 shader cbuffer Frame 对应）。
struct FrameCB {
	glm::vec4 lightDir;              // 指向光源的单位方向（世界空间）
	glm::vec4 lightColor;            // 光源颜色（w = 强度，与参考项目 dirLightColor.a 一致）
	glm::vec4 ambientColor;          // 环境光
	glm::vec4 cameraPosition;        // 世界空间相机位置
	float invProjection00;           // 1 / proj[0][0]，FOV 补偿（参考项目 fovFade）
	float invScreenAspectRatio;      // 高度/宽度
	float outlineDistanceFadeFactor; // 描边距离衰减系数（参考项目 outlineDistanceFadeFactor）
	float _pad;                      // 填充到 16 字节
};

struct DrawItem {
	const Mesh* mesh = nullptr;
	glm::mat4 model{ 1.0f };
	glm::vec4 color{ 1.0f };
	UINT srvIndex = 0;      // 使用的纹理在 SRV heap 中的索引
	bool outline = false;   // 是否绘制描边（对齐参考项目：仅模型材质启用，平面/测试物体无）
	bool disableDepth = false; // 禁用深度测试（天空盒/全屏：depth=1 会被物体 depth=浅 挡住）
	float outlineWidth = 10.0f; // 描边外扩宽度（参考项目 ArcOutline.outlineWidth=10 / EndfieldOutline=16）
	bool sky = false;       // SkySphere 专用：用 Unlit 天空 shader（参考项目 SkySphere.mat）
	glm::vec3 outlineColor{ 0.0f }; // 描边颜色（参考项目 outlineColor：Yvonne Body 深红棕/Cloth 黑/Hair 深紫）
};

// 网格渲染器：支持多物体绘制 + 多纹理。
// 根签名：b0 CBV root descriptor（PerObjectCB）、t0 SRV 描述符表（每物体不同纹理）、s0 采样器。
class MeshRenderer {
public:
	MeshRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		const std::vector<Texture*>& textures,
		UINT maxObjects,
		const std::vector<std::uint8_t>& outlineVsBytes = {},
		const std::vector<std::uint8_t>& outlinePsBytes = {},
		DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM, // 默认后台缓冲；离屏 HDR 传 R16G16B16A16_FLOAT
		ID3D12DescriptorHeap* sharedSrvHeap = nullptr,       // 阶段 10d：depth SRV 写入共享堆
		UINT depthSrvIndex = 0,
		const std::vector<std::uint8_t>& skyVsBytes = {},    // SkySphere 专用 Unlit shader（参考项目 SkySphere.mat）
		const std::vector<std::uint8_t>& skyPsBytes = {});
	~MeshRenderer();

	MeshRenderer(const MeshRenderer&) = delete;
	MeshRenderer& operator=(const MeshRenderer&) = delete;

	// 设置场景方向光（世界空间）。
	void setLight(const glm::vec3& dir, const glm::vec3& color, const glm::vec3& ambient);

	// 每帧调用：绘制所有物体，输出到交换链后台缓冲。
	void render(const glm::mat4& view, const glm::mat4& proj, const std::vector<DrawItem>& items);
	// 绘制到指定 RTV（阶段 10：离屏 sceneColor）。clearDepth=false 时保留已有深度（程序化地形已写入）。
	void render(const glm::mat4& view, const glm::mat4& proj, const std::vector<DrawItem>& items,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv, bool clearDepth = true);

	// 阶段 10d：depth 的 SRV（供后处理采样），及资源状态切换。
	D3D12_GPU_DESCRIPTOR_HANDLE depthSrvHandle() const { return _depthSrvGpu; }
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle() const { return _dsvHeap->GetCPUDescriptorHandleForHeapStart(); }
	ID3D12Resource* depthResource() const { return _depthBuffer.Get(); }
	void transitionDepthTo(D3D12_RESOURCE_STATES newState);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	// 创建描边 PSO（对齐参考项目 ArcOutline.mat：CullMode=FRONT + CCW front）。
	void createOutlinePipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	// 创建无深度 PSO（天空盒：DepthEnable=FALSE，深度不测不写）。
	void createNoDepthPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	// 创建 SkySphere PSO（Unlit 天空 shader + CullMode=BACK + 深度 ALWAYS 不写，对齐参考项目 SkySphere.mat）。
	void createSkyPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	void createDepthBuffer();
	void createTextureDescriptors(const std::vector<Texture*>& textures);
	void createPerObjectCbv(UINT maxObjects);
	void createFrameCbv();

	D3D12Context& _context;
	DXGI_FORMAT _rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _psoNoDepth; // 无深度 PSO（DepthEnable=FALSE）
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _psoSky;     // SkySphere PSO（Unlit + CullMode=BACK + 深度 ALWAYS 不写）
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _outlinePso; // 描边 PSO（剔正面，画远侧）

	Microsoft::WRL::ComPtr<ID3D12Resource> _depthBuffer;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _dsvHeap;
	ID3D12DescriptorHeap* _sharedSrvHeap = nullptr; // 阶段 10d：depth SRV 写入共享堆
	UINT _depthSrvIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE _depthSrvGpu{}; // depth SRV（供后处理采样）
	D3D12_RESOURCE_STATES _depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _srvHeap;     // CBV_SRV_UAV，含所有纹理 SRV
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap; // 采样器堆
	UINT _srvDescriptorSize = 0;
	UINT _srvCount = 1;

	Microsoft::WRL::ComPtr<ID3D12Resource> _perObjectCbv; // 上传堆，maxObjects * CbvStride
	Microsoft::WRL::ComPtr<ID3D12Resource> _frameCbv;     // 上传堆，帧共享光照 CBV
	UINT _maxObjects = 0;

	// 场景光照（默认左上方向光）。
	glm::vec3 _lightDir{ 0.4f, 1.0f, 0.3f };
	glm::vec3 _lightColor{ 1.0f, 1.0f, 1.0f };
	glm::vec3 _ambient{ 0.12f, 0.12f, 0.15f };

	static constexpr UINT CbvStride = 256; // 每物体 CBV 对齐
};
