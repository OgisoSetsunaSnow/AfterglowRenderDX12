#pragma once

#include "D3D12Core.h"

#include <cstdint>
#include <vector>

// 阶段 10b：通用全屏 pass（输入 1 SRV + 1 sampler，输出到指定 RTV）。
// 用于 Bloom 的 DownSampling / HorizontalBlur / VerticalBlur 等。
class FullScreenPass {
public:
	FullScreenPass(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		DXGI_FORMAT rtvFormat);
	~FullScreenPass() = default;

	FullScreenPass(const FullScreenPass&) = delete;
	FullScreenPass& operator=(const FullScreenPass&) = delete;

	void render(ID3D12DescriptorHeap* srvHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE srv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		UINT width, UINT height);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat);

	D3D12Context& _context;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
};