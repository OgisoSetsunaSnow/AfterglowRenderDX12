#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <wrl/client.h>
#include <cstdint>
#include <vector>

class Mesh;
class Texture;

// 每物体的常量缓冲内容（与 shader cbuffer 对应）。
struct PerObjectCB {
	glm::mat4 mvp;
	glm::vec4 baseColor;
};

struct DrawItem {
	const Mesh* mesh = nullptr;
	glm::mat4 model{ 1.0f };
	glm::vec4 color{ 1.0f };
	UINT srvIndex = 0; // 使用的纹理在 SRV heap 中的索引
};

// 网格渲染器：支持多物体绘制 + 多纹理。
// 根签名：b0 CBV root descriptor（PerObjectCB）、t0 SRV 描述符表（每物体不同纹理）、s0 采样器。
class MeshRenderer {
public:
	MeshRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		const std::vector<Texture*>& textures,
		UINT maxObjects);
	~MeshRenderer();

	MeshRenderer(const MeshRenderer&) = delete;
	MeshRenderer& operator=(const MeshRenderer&) = delete;

	// 每帧调用：绘制所有物体，后台缓冲须已过渡为 RENDER_TARGET。
	void render(const glm::mat4& view, const glm::mat4& proj, const std::vector<DrawItem>& items);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	void createDepthBuffer();
	void createTextureDescriptors(const std::vector<Texture*>& textures);
	void createPerObjectCbv(UINT maxObjects);

	D3D12Context& _context;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;

	Microsoft::WRL::ComPtr<ID3D12Resource> _depthBuffer;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _dsvHeap;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _srvHeap;     // CBV_SRV_UAV，含所有纹理 SRV
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap; // 采样器堆
	UINT _srvDescriptorSize = 0;
	UINT _srvCount = 1;

	Microsoft::WRL::ComPtr<ID3D12Resource> _perObjectCbv; // 上传堆，maxObjects * CbvStride
	UINT _maxObjects = 0;

	static constexpr UINT CbvStride = 256; // 每物体 CBV 对齐
};
