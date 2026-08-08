#pragma once

#include "D3D12Core.h"

// 阶段 10：离屏场景渲染目标（HDR sceneColor + RTV/SRV），供后处理 pass 采样。
// sharedSrvHeap != nullptr 时，SRV 写入共享堆的 srvIndex 位置——
// D3D12 同一时刻只能设置一个 CBV_SRV_UAV 堆，多个纹理的 SRV 必须共享同一个堆。
class OffscreenRT {
public:
	OffscreenRT() = default;

	// clearColor 为资源创建时的优化 clear 值（nullptr 用默认背景色）。
	void create(D3D12Context& context, UINT width = 0, UINT height = 0,
		ID3D12DescriptorHeap* sharedSrvHeap = nullptr, UINT srvIndex = 0,
		const float* clearColor = nullptr);
	// compute 输出纹理：ALLOW_UNORDERED_ACCESS（R8G8B8A8_UNORM），SRV+UAV 写共享堆。
	void createUav(D3D12Context& context, UINT width, UINT height,
		ID3D12DescriptorHeap* sharedSrvHeap, UINT srvIndex, UINT uavIndex);
	~OffscreenRT() = default;

	OffscreenRT(const OffscreenRT&) = delete;
	OffscreenRT& operator=(const OffscreenRT&) = delete;

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle() const { return _rtvHeap->GetCPUDescriptorHandleForHeapStart(); }
	D3D12_GPU_DESCRIPTOR_HANDLE srvHandle() const { return _srvGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE uavHandle() const { return _uavGpu; }
	ID3D12DescriptorHeap* srvHeap() const { return _srvHeap; }
	ID3D12Resource* resource() const { return _color.Get(); }

	// 切换 sceneColor 资源状态（内部跟踪当前状态，重复调用安全）。
	void transitionColor(D3D12_RESOURCE_STATES newState);
	// 直接同步内部状态跟踪（外部已发出 barrier 时用，如诊断拷贝路径）。
	void setColorStateDirect(D3D12_RESOURCE_STATES newState) { _colorState = newState; }

private:
	D3D12Context* _context = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> _color;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _rtvHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _ownSrvHeap; // 未提供共享堆时自建
	ID3D12DescriptorHeap* _srvHeap = nullptr;
	D3D12_GPU_DESCRIPTOR_HANDLE _srvGpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE _uavGpu{};
	D3D12_RESOURCE_STATES _colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
};
