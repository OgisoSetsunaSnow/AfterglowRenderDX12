#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

class Window;

// D3D12 渲染基础上下文：设备、命令队列、交换链、帧资源与 GPU 同步。
// 阶段 1 采用"每帧等待 GPU 完成"的简单帧同步，保证正确性，后续再优化为多帧在途。
class D3D12Context {
public:
	explicit D3D12Context(Window& window);
	~D3D12Context();

	D3D12Context(const D3D12Context&) = delete;
	D3D12Context& operator=(const D3D12Context&) = delete;

	// 开始录制一帧。若窗口不可见（最小化）返回 false，跳过本帧渲染。
	bool beginFrame();
	// 提交命令列表并 Present。
	void endFrame();

	// 提交已录制的命令列表并等待 GPU 完成（用于初始化阶段的一次性上传）。
	void submitAndWait();

	// 等待 GPU 完成所有已提交工作。
	void waitForGpu();

	// 供渲染代码使用的基础对象。
	ID3D12Device* device() const { return _device.Get(); }
	ID3D12CommandQueue* commandQueue() const { return _commandQueue.Get(); }
	ID3D12GraphicsCommandList* commandList() const { return _commandList.Get(); }
	ID3D12DescriptorHeap* rtvHeap() const { return _rtvHeap.Get(); }

	ID3D12CommandAllocator* commandAllocator() const { return _commandAllocator.Get(); }

	ID3D12Resource* currentBackBuffer() const { return _backBuffers[_currentBackBufferIndex].Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE currentRtv() const {
		return D3D12_CPU_DESCRIPTOR_HANDLE{ _rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr
			+ static_cast<SIZE_T>(_currentBackBufferIndex) * _rtvDescriptorSize };
	}

	uint32_t width() const { return _width; }
	uint32_t height() const { return _height; }
	bool needsResize() const { return _needsResize; }
	void setNeedsResize() { _needsResize = true; }

	// 便捷 barrier：交换链后台缓冲状态切换。
	void transitionCurrentBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

private:
	void createFactoryAndDevice();
	void createCommandQueue();
	void createSwapChain();
	void createFrameResources();
	void resizeSwapChain();

	void initSwapChainInternal(bool initial);

	Window* _window = nullptr;

	ComPtr<IDXGIFactory4> _factory;
	ComPtr<ID3D12Device> _device;
	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<IDXGISwapChain3> _swapChain;

	ComPtr<ID3D12DescriptorHeap> _rtvHeap;
	UINT _rtvDescriptorSize = 0;

	ComPtr<ID3D12CommandAllocator> _commandAllocator;
	ComPtr<ID3D12GraphicsCommandList> _commandList;

	ComPtr<ID3D12Fence> _fence;
	UINT64 _fenceValue = 0;
	HANDLE _fenceEvent = nullptr;

	std::vector<ComPtr<ID3D12Resource>> _backBuffers;

	static constexpr UINT BackBufferCount = 3;
	UINT _currentBackBufferIndex = 0;

	uint32_t _width = 0;
	uint32_t _height = 0;
	bool _needsResize = false;
	bool _frameBegun = false;
};
