#include "D3D12Core.h"

#include "Window.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>

#include <format>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void throwHr(HRESULT hr, const char* what) {
	// 常见错误给出可读信息。
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		throw std::runtime_error(std::string(what) + " (device lost)");
	}
	if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
		throw std::runtime_error(std::string(what) + " (debug layer missing)");
	}
	throw std::runtime_error(std::string(what) + " (HRESULT 0x" +
		std::format("{:08X}", static_cast<unsigned>(hr)) + ")");
}

#define CHECK_HR(hr, msg) do { if (FAILED(hr)) throwHr(hr, msg); } while (0)

} // namespace

D3D12Context::D3D12Context(Window& window) : _window(&window) {
	createFactoryAndDevice();
	createCommandQueue();
	createSwapChain();
	createFrameResources();
}

D3D12Context::~D3D12Context() {
	waitForGpu();
	if (_fenceEvent) {
		CloseHandle(_fenceEvent);
		_fenceEvent = nullptr;
	}
}

void D3D12Context::createFactoryAndDevice() {
	UINT factoryFlags = 0;
#ifdef _DEBUG
	ComPtr<ID3D12Debug> debugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();
		factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif
	CHECK_HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&_factory)), "Failed to create DXGI factory");

	ComPtr<IDXGIAdapter1> adapter;
	// 优先使用性能最好的硬件适配器。
	for (UINT i = 0; ; ++i) {
		ComPtr<IDXGIAdapter1> candidate;
		if (_factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(candidate->GetDesc1(&desc))) {
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
				continue; // 跳过软件适配器（WARP 作为后备）。
			}
			adapter = candidate;
			break;
		}
	}

	if (adapter) {
		CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device)),
			"Failed to create D3D12 device (hardware)");
	} else {
		// 回退到 WARP 软件渲染，保证程序至少能跑。
		ComPtr<IDXGIAdapter1> warp;
		CHECK_HR(_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "Failed to find WARP adapter");
		CHECK_HR(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device)),
			"Failed to create D3D12 device (WARP)");
		std::cerr << "[D3D12] Using WARP software adapter (no hardware D3D12 device found)." << std::endl;
	}
}

void D3D12Context::createCommandQueue() {
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;
	CHECK_HR(_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&_commandQueue)),
		"Failed to create command queue");
}

void D3D12Context::createSwapChain() {
	uint32_t w, h;
	_window->getFramebufferSize(w, h);
	_width = w > 0 ? w : 1280;
	_height = h > 0 ? h : 720;

	DXGI_SWAP_CHAIN_DESC desc{};
	desc.BufferDesc.Width = _width;
	desc.BufferDesc.Height = _height;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.RefreshRate.Numerator = 0;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
	desc.BufferCount = BackBufferCount;
	desc.OutputWindow = static_cast<HWND>(_window->nativeHandle());
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	ComPtr<IDXGISwapChain> rawSwapChain;
	CHECK_HR(_factory->CreateSwapChain(_commandQueue.Get(), &desc, &rawSwapChain),
		"Failed to create swap chain");
	CHECK_HR(rawSwapChain.As(&_swapChain), "Failed to query IDXGISwapChain3");

	initSwapChainInternal(true);
}

void D3D12Context::initSwapChainInternal(bool initial) {
	// 获取后台缓冲。
	_backBuffers.resize(BackBufferCount);
	for (UINT i = 0; i < BackBufferCount; ++i) {
		CHECK_HR(_swapChain->GetBuffer(i, IID_PPV_ARGS(&_backBuffers[i])),
			"Failed to get swap chain buffer");
	}

	if (initial) {
		// 创建 RTV 描述符堆。
		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
		rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvDesc.NumDescriptors = BackBufferCount;
		rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		CHECK_HR(_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&_rtvHeap)),
			"Failed to create RTV descriptor heap");
		_rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	for (UINT i = 0; i < BackBufferCount; ++i) {
		D3D12_CPU_DESCRIPTOR_HANDLE handle{ _rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr
			+ static_cast<SIZE_T>(i) * _rtvDescriptorSize };
		_device->CreateRenderTargetView(_backBuffers[i].Get(), nullptr, handle);
	}

	_currentBackBufferIndex = _swapChain->GetCurrentBackBufferIndex();
}

void D3D12Context::resizeSwapChain() {
	waitForGpu();

	for (auto& buffer : _backBuffers) {
		buffer.Reset();
	}

	CHECK_HR(_swapChain->ResizeBuffers(BackBufferCount, _width, _height, DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING), "Failed to resize swap chain");

	initSwapChainInternal(false);
	_needsResize = false;
}

void D3D12Context::createFrameResources() {
	CHECK_HR(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_commandAllocator)),
		"Failed to create command allocator");
	CHECK_HR(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _commandAllocator.Get(),
		nullptr, IID_PPV_ARGS(&_commandList)), "Failed to create command list");
	CHECK_HR(_commandList->Close(), "Failed to close command list");

	CHECK_HR(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)),
		"Failed to create fence");
	_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!_fenceEvent) {
		throw std::runtime_error("Failed to create fence event");
	}
	_fenceValue = 0;
}

bool D3D12Context::beginFrame() {
	// 检测窗口尺寸变化。
	uint32_t w, h;
	_window->getFramebufferSize(w, h);
	if ((w != _width || h != _height) && w > 0 && h > 0) {
		_width = w;
		_height = h;
		_needsResize = true;
	}
	if (_needsResize) {
		resizeSwapChain();
	}

	// 简单帧同步：等待上一帧完成，确保命令分配器可安全重置。
	if (_fenceValue > 0) {
		const UINT64 completed = _fence->GetCompletedValue();
		if (completed < _fenceValue) {
			CHECK_HR(_fence->SetEventOnCompletion(_fenceValue, _fenceEvent), "SetEventOnCompletion failed");
			WaitForSingleObject(_fenceEvent, INFINITE);
		}
	}

	CHECK_HR(_commandAllocator->Reset(), "Failed to reset command allocator");
	CHECK_HR(_commandList->Reset(_commandAllocator.Get(), nullptr), "Failed to reset command list");

	_currentBackBufferIndex = _swapChain->GetCurrentBackBufferIndex();
	_frameBegun = true;
	return true;
}

void D3D12Context::endFrame() {
	if (!_frameBegun) {
		return;
	}

	CHECK_HR(_commandList->Close(), "Failed to close command list");

	ID3D12CommandList* const lists[] = { _commandList.Get() };
	_commandQueue->ExecuteCommandLists(1, lists);

	CHECK_HR(_swapChain->Present(1, 0), "Present failed");

	// 记录 fence，供下一帧等待。
	++_fenceValue;
	CHECK_HR(_commandQueue->Signal(_fence.Get(), _fenceValue), "Signal failed");

	_frameBegun = false;
}

void D3D12Context::submitAndWait() {
	ID3D12CommandList* const lists[] = { _commandList.Get() };
	_commandQueue->ExecuteCommandLists(1, lists);

	++_fenceValue;
	CHECK_HR(_commandQueue->Signal(_fence.Get(), _fenceValue), "Signal failed");

	const UINT64 completed = _fence->GetCompletedValue();
	if (completed < _fenceValue) {
		CHECK_HR(_fence->SetEventOnCompletion(_fenceValue, _fenceEvent), "SetEventOnCompletion failed");
		WaitForSingleObject(_fenceEvent, INFINITE);
	}
	_frameBegun = false;
}

void D3D12Context::waitForGpu() {
	if (_frameBegun) {
		// 若帧未结束，先补一次 signal。
		CHECK_HR(_commandQueue->Signal(_fence.Get(), ++_fenceValue), "Signal failed");
		_frameBegun = false;
	}
	if (_fenceValue == 0) {
		return;
	}
	const UINT64 completed = _fence->GetCompletedValue();
	if (completed < _fenceValue) {
		CHECK_HR(_fence->SetEventOnCompletion(_fenceValue, _fenceEvent), "SetEventOnCompletion failed");
		WaitForSingleObject(_fenceEvent, INFINITE);
	}
}

void D3D12Context::transitionCurrentBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	if (before == after) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = currentBackBuffer();
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	_commandList->ResourceBarrier(1, &barrier);
}
