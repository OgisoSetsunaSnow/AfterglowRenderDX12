#pragma once

#include "D3D12Core.h"

#include <cstdint>
#include <vector>

// 阶段 10b：通用全屏 pass（输入 1~2 SRV + 可选 b0 CBV，输出到指定 RTV）。
// 用于 Bloom 的 DownSampling / HorizontalBlur / VerticalBlurCombination 等。
class FullScreenPass {
public:
	FullScreenPass(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		DXGI_FORMAT rtvFormat);
	~FullScreenPass() = default;

	FullScreenPass(const FullScreenPass&) = delete;
	FullScreenPass& operator=(const FullScreenPass&) = delete;

	// srv2 可选（VerticalBlurCombination 读 t1=上一级 combined）；cbvData 可选（b0 BloomParams）。
	// 根签名：b0(root CBV) + t0/t1(2 SRV) + s0/s1(2 sampler)。
	void render(ID3D12DescriptorHeap* srvHeap,
		D3D12_GPU_DESCRIPTOR_HANDLE srv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		UINT width, UINT height,
		D3D12_GPU_DESCRIPTOR_HANDLE srv2 = {},
		const void* cbvData = nullptr,
		std::size_t cbvSize = 0);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat);
	void createCbv();

	D3D12Context& _context;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> _cbv; // b0（BloomParams）
};
