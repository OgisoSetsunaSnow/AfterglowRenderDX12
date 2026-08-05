#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 阶段 10：全屏后处理 pass（对应参考项目 PostProcess pass + PostProcess_VS/FS）。
// 采样离屏 sceneColor（+ 后续 bloomCombinedTexture），输出到交换链后台缓冲。
class PostProcess {
public:
	PostProcess(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes);
	~PostProcess() = default;

	PostProcess(const PostProcess&) = delete;
	PostProcess& operator=(const PostProcess&) = delete;

	// 渲染全屏后处理（10b bloom 合成 + 10c ACES + 10d 雾）。
	// sceneSrv/bloomSrv/depthSrv 必须来自同一个 srvHeap（D3D12 限制：一个 CBV_SRV_UAV 堆）。
	void render(ID3D12DescriptorHeap* srvHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
		D3D12_GPU_DESCRIPTOR_HANDLE bloomSrv,
		D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		const glm::vec3& cameraPos, float nearZ, float farZ);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	void createParamCbv();

	D3D12Context& _context;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> _paramCbv; // b0：相机/雾参数
};
