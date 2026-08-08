#pragma once

#include "D3D12Core.h"

#include <cstdint>
#include <vector>

// D3D12 compute pipeline 基础设施：编译 CS + root signature + CS PSO + dispatch。
// 根签名：numSrvRanges 个 SRV 描述符表（t0..）+ numUavRanges 个 UAV 描述符表（u0..）
//         + numSamplerRanges 个 Sampler 描述符表（s0..）+ 可选 b0 CBV。
// 参考项目 compute 大量使用（地形/水/草地/Boids/粒子），此为后续所有 compute 效果的地基。
class ComputePass {
public:
	ComputePass(D3D12Context& context, const std::vector<std::uint8_t>& csBytes,
		UINT numSrvRanges = 0, UINT numUavRanges = 1, bool hasCbv = false,
		UINT numSamplerRanges = 0);
	~ComputePass() = default;

	ComputePass(const ComputePass&) = delete;
	ComputePass& operator=(const ComputePass&) = delete;

	// dispatch（新接口）：srvs/uavs/samplers 按 t0.. / u0.. / s0.. 顺序传对应堆内 GPU 句柄。
	// 调用方需保证 heap/samplerHeap 已 SetDescriptorHeaps，且 UAV 资源当前处于 UNORDERED_ACCESS 状态。
	// cbvAddress：可选 b0 CBV 的 GPU 虚拟地址（hasCbv 时有效）。
	void dispatch(ID3D12DescriptorHeap* heap,
		const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& srvs,
		const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& uavs,
		UINT threadGroupsX, UINT threadGroupsY, UINT threadGroupsZ,
		ID3D12GraphicsCommandList* cmdList = nullptr,
		D3D12_GPU_VIRTUAL_ADDRESS cbvAddress = 0,
		ID3D12DescriptorHeap* samplerHeap = nullptr,
		const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& samplers = {});

	// dispatch（旧接口兼容）：单 UAV + 可选 SRV base。
	void dispatch(ID3D12DescriptorHeap* heap, D3D12_GPU_DESCRIPTOR_HANDLE uav,
		UINT threadGroupsX, UINT threadGroupsY, UINT threadGroupsZ,
		D3D12_GPU_DESCRIPTOR_HANDLE srvBase = {},
		ID3D12GraphicsCommandList* cmdList = nullptr);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& csBytes);

	D3D12Context& _context;
	UINT _numSrvRanges = 0;
	UINT _numUavRanges = 1;
	bool _hasCbv = false;
	UINT _numSamplerRanges = 0;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
};
