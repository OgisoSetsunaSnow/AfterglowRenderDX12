#include "FractalNoise.h"
#include "ComputePass.h"

#include <d3dx12.h>

#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

namespace {
constexpr UINT kSrvSimplex = 0;
constexpr UINT kSrvWorley = 1;
constexpr UINT kSrvNormal = 2;
constexpr UINT kUavSimplex = 3;
constexpr UINT kUavWorley = 4;
constexpr UINT kUavNormal = 5;
constexpr UINT kDescriptorCount = 6;
constexpr UINT kSide = 256;
} // namespace

FractalNoise::FractalNoise(D3D12Context& context,
	const std::vector<std::uint8_t>& simplexInitCs,
	const std::vector<std::uint8_t>& worleyInitCs,
	const std::vector<std::uint8_t>& fractalNoiseCs)
	: _context(context),
	_simplexInitCs(simplexInitCs), _worleyInitCs(worleyInitCs), _fractalNoiseCs(fractalNoiseCs) {
	createTextures();
	createDescriptors();
	createSamplerHeap();
}

D3D12_GPU_DESCRIPTOR_HANDLE FractalNoise::simplexSrv() const { return _simplexSrvGpu; }
D3D12_GPU_DESCRIPTOR_HANDLE FractalNoise::worleySrv() const { return _worleySrvGpu; }
D3D12_GPU_DESCRIPTOR_HANDLE FractalNoise::normalSrv() const { return _normalSrvGpu; }

void FractalNoise::createTextures() {
	auto* device = _context.device();
	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

	const auto makeTex = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& out,
		const char* name, DXGI_FORMAT fmt) {
		D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(fmt, kSide, kSide, 1, 1);
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)),
			std::string("Failed to create fractal texture: ") + name);
	};
	makeTex(_simplexTex, "simplex", DXGI_FORMAT_R32_FLOAT);
	makeTex(_worleyTex, "worley", DXGI_FORMAT_R32_FLOAT);
	makeTex(_normalTex, "normal", DXGI_FORMAT_R8G8B8A8_SNORM);
}

void FractalNoise::createDescriptors() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create fractal descriptor heap");

	const auto writeSrv = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = fmt;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(res, &srv, cpu);
	};
	writeSrv(kSrvSimplex, _simplexTex.Get(), DXGI_FORMAT_R32_FLOAT);
	writeSrv(kSrvWorley, _worleyTex.Get(), DXGI_FORMAT_R32_FLOAT);
	writeSrv(kSrvNormal, _normalTex.Get(), DXGI_FORMAT_R8G8B8A8_SNORM);

	const auto writeUav = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = fmt;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
	};
	writeUav(kUavSimplex, _simplexTex.Get(), DXGI_FORMAT_R32_FLOAT);
	writeUav(kUavWorley, _worleyTex.Get(), DXGI_FORMAT_R32_FLOAT);
	writeUav(kUavNormal, _normalTex.Get(), DXGI_FORMAT_R8G8B8A8_SNORM);

	D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_simplexSrvGpu.ptr = gpu.ptr + kSrvSimplex * inc;
	_worleySrvGpu.ptr = gpu.ptr + kSrvWorley * inc;
	_normalSrvGpu.ptr = gpu.ptr + kSrvNormal * inc;
	_simplexUavGpu.ptr = gpu.ptr + kUavSimplex * inc;
	_worleyUavGpu.ptr = gpu.ptr + kUavWorley * inc;
	_normalUavGpu.ptr = gpu.ptr + kUavNormal * inc;
}

void FractalNoise::createSamplerHeap() {
	auto* device = _context.device();
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create fractal sampler heap");

	D3D12_SAMPLER_DESC sd{};
	sd.Filter = D3D12_FILTER_ANISOTROPIC;
	sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sd.MipLODBias = 0.0f;
	sd.MaxAnisotropy = 16;
	sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = FLT_MAX;
	device->CreateSampler(&sd, _samplerHeap->GetCPUDescriptorHandleForHeapStart());
}

void FractalNoise::initialize() {
	// 独立 command list 一次性 dispatch（顺序：simplex → worley → normal，barrier 回采样态）。
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	auto* device = _context.device();
	CHECK_OK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
		"Failed to create fractal init allocator");
	CHECK_OK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
		nullptr, IID_PPV_ARGS(&list)),
		"Failed to create fractal init command list");

	// 1. simplex（1 UAV）。
	ComputePass simplexPass(_context, _simplexInitCs, 0, 1);
	simplexPass.dispatch(_heap.Get(), _simplexUavGpu, 8, 8, 1, {}, list.Get());
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_simplexTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &b);
	}
	// 2. worley（1 UAV）。
	ComputePass worleyPass(_context, _worleyInitCs, 0, 1);
	worleyPass.dispatch(_heap.Get(), _worleyUavGpu, 8, 8, 1, {}, list.Get());
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_worleyTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &b);
	}
	// 3. normal（2 SRV + 1 UAV：t0=simplex t1=worley → u0=normal）。
	ComputePass fractalPass(_context, _fractalNoiseCs, 2, 1);
	fractalPass.dispatch(_heap.Get(),
		{ _simplexSrvGpu, _worleySrvGpu }, { _normalUavGpu },
		8, 8, 1, list.Get(), 0);
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_normalTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &b);
	}

	CHECK_OK(list->Close(), "Failed to close fractal init command list");
	ID3D12CommandList* const lists[] = { list.Get() };
	_context.commandQueue()->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	CHECK_OK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
		"Failed to create fractal init fence");
	CHECK_OK(_context.commandQueue()->Signal(fence.Get(), 1), "Signal failed (fractal init)");
	if (fence->GetCompletedValue() < 1) {
		HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(fence->SetEventOnCompletion(1, ev))) {
			throw std::runtime_error("SetEventOnCompletion failed (fractal init)");
		}
		WaitForSingleObject(ev, INFINITE);
		CloseHandle(ev);
	}
}
