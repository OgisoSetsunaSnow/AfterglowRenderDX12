#include "OffscreenRT.h"

#include <d3dx12.h>

#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

void OffscreenRT::create(D3D12Context& context, UINT width, UINT height,
	ID3D12DescriptorHeap* sharedSrvHeap, UINT srvIndex, const float* clearColor) {
	_context = &context;
	auto* device = context.device();
	const UINT w = width ? width : context.width();
	const UINT h = height ? height : context.height();

	const float defClear[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
	const float* cc = clearColor ? clearColor : defClear;

	// HDR sceneColor：R16G16B16A16_FLOAT（参考项目 colorFormat），可 RT + 可采样。
	D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1, 1);
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	clear.Color[0] = cc[0];
	clear.Color[1] = cc[1];
	clear.Color[2] = cc[2];
	clear.Color[3] = cc[3];

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&_color)),
		"Failed to create offscreen color target");

	// RTV。
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.NumDescriptors = 1;
	CHECK_OK(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_rtvHeap)),
		"Failed to create offscreen RTV heap");
	device->CreateRenderTargetView(_color.Get(), nullptr, _rtvHeap->GetCPUDescriptorHandleForHeapStart());

	// SRV（shader visible，供后处理 pass 采样）。
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;

	const UINT srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	if (sharedSrvHeap) {
		// 写入共享堆：D3D12 同一时刻只能有一个 CBV_SRV_UAV 堆。
		_srvHeap = sharedSrvHeap;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(srvIndex) * srvIncrement;
		device->CreateShaderResourceView(_color.Get(), &srv, cpu);
		_srvGpu = sharedSrvHeap->GetGPUDescriptorHandleForHeapStart();
		_srvGpu.ptr += static_cast<SIZE_T>(srvIndex) * srvIncrement;
	} else {
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.NumDescriptors = 1;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		CHECK_OK(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&_ownSrvHeap)),
			"Failed to create offscreen SRV heap");
		_srvHeap = _ownSrvHeap.Get();
		device->CreateShaderResourceView(_color.Get(), &srv, _ownSrvHeap->GetCPUDescriptorHandleForHeapStart());
		_srvGpu = _ownSrvHeap->GetGPUDescriptorHandleForHeapStart();
	}
}

void OffscreenRT::createUav(D3D12Context& context, UINT width, UINT height,
	ID3D12DescriptorHeap* sharedSrvHeap, UINT srvIndex, UINT uavIndex) {
	_context = &context;
	auto* device = context.device();
	const UINT w = width ? width : context.width();
	const UINT h = height ? height : context.height();

	// compute 输出纹理：R8G8B8A8_UNORM + ALLOW_UNORDERED_ACCESS。
	D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1);
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_color)),
		"Failed to create compute UAV texture");
	_colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// SRV（写共享堆 srvIndex，供 graphics 采样）。
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	_srvHeap = sharedSrvHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(srvIndex) * inc;
	device->CreateShaderResourceView(_color.Get(), &srv, srvCpu);
	_srvGpu = sharedSrvHeap->GetGPUDescriptorHandleForHeapStart();
	_srvGpu.ptr += static_cast<SIZE_T>(srvIndex) * inc;

	// UAV（写共享堆 uavIndex，供 compute 写）。
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
	uavCpu.ptr += static_cast<SIZE_T>(uavIndex) * inc;
	device->CreateUnorderedAccessView(_color.Get(), nullptr, &uav, uavCpu);
	_uavGpu = sharedSrvHeap->GetGPUDescriptorHandleForHeapStart();
	_uavGpu.ptr += static_cast<SIZE_T>(uavIndex) * inc;
}

void OffscreenRT::transitionColor(D3D12_RESOURCE_STATES newState) {
	if (_colorState == newState) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		_color.Get(), _colorState, newState);
	_context->commandList()->ResourceBarrier(1, &barrier);
	_colorState = newState;
}
