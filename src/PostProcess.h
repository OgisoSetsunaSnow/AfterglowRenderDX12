#pragma once

#include "D3D12Core.h"

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

	// 渲染全屏后处理（10b 加 bloom 合成）。
	// sceneSrv 与 bloomSrv 必须来自同一个 srvHeap（D3D12 同一时刻只能有一个 CBV_SRV_UAV 堆）。
	void render(ID3D12DescriptorHeap* srvHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
		D3D12_GPU_DESCRIPTOR_HANDLE bloomSrv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);

	D3D12Context& _context;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
};
