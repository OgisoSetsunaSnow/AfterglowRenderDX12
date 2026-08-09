#include "FullScreenPass.h"

#include <d3dx12.h>

#include <cfloat>
#include <cstring>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

FullScreenPass::FullScreenPass(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	DXGI_FORMAT rtvFormat)
	: _context(context) {
	createRootSignature();
	createPipelineState(vsBytes, psBytes, rtvFormat);
	createCbv();

	auto* device = context.device();
	D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
	samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerHeapDesc.NumDescriptors = 2; // s0（线性采样）+ s1（线性采样，Vertical 的 combined）
	samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create fullscreen pass sampler heap");

	D3D12_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.MinLOD = 0.0f;
	samplerDesc.MaxLOD = FLT_MAX; // 允许所有 mip
	D3D12_CPU_DESCRIPTOR_HANDLE s0 = _samplerHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateSampler(&samplerDesc, s0);
	D3D12_CPU_DESCRIPTOR_HANDLE s1 = s0;
	s1.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	device->CreateSampler(&samplerDesc, s1);
}

void FullScreenPass::createRootSignature() {
	// b0(root CBV) + t0/t1(2 SRV 独立表) + s0/s1(2 sampler 独立表)。
	D3D12_DESCRIPTOR_RANGE srvRange0{};
	srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange0.NumDescriptors = 1;
	srvRange0.BaseShaderRegister = 0; // t0
	srvRange0.RegisterSpace = 0;
	D3D12_DESCRIPTOR_RANGE srvRange1{};
	srvRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange1.NumDescriptors = 1;
	srvRange1.BaseShaderRegister = 1; // t1
	srvRange1.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange0{};
	samplerRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange0.NumDescriptors = 1;
	samplerRange0.BaseShaderRegister = 0; // s0
	samplerRange0.RegisterSpace = 0;
	D3D12_DESCRIPTOR_RANGE samplerRange1{};
	samplerRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange1.NumDescriptors = 1;
	samplerRange1.BaseShaderRegister = 1; // s1
	samplerRange1.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[5]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srvRange0;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &srvRange1;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &samplerRange0;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable.NumDescriptorRanges = 1;
	params[4].DescriptorTable.pDescriptorRanges = &samplerRange1;
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 5;
	rsDesc.pParameters = params;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize fullscreen pass root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create fullscreen pass root signature");
}

void FullScreenPass::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat) {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _rootSignature.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.PS = { ps.data(), ps.size() };

	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.StencilEnable = FALSE;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = rtvFormat;
	desc.SampleDesc.Count = 1;

	CHECK_OK(_context.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pso)),
		"Failed to create fullscreen pass pipeline state");
}

void FullScreenPass::createCbv() {
	auto* device = _context.device();
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(64);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_cbv)),
		"Failed to create fullscreen pass CBV");
}

void FullScreenPass::render(ID3D12DescriptorHeap* srvHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE srv,
	const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	UINT width, UINT height,
	D3D12_GPU_DESCRIPTOR_HANDLE srv2,
	const void* cbvData,
	std::size_t cbvSize) {
	auto* cl = _context.commandList();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	ID3D12DescriptorHeap* const heaps[] = { srvHeap, _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// b0：BloomParams（bloomIntensity + useCombinedTexture），没传则默认全 0。
	if (cbvData && cbvSize > 0) {
		void* mapped = nullptr;
		_cbv->Map(0, nullptr, &mapped);
		std::memset(mapped, 0, 64);
		std::memcpy(mapped, cbvData, cbvSize > 64 ? 64 : cbvSize);
		_cbv->Unmap(0, nullptr);
	} else {
		void* mapped = nullptr;
		_cbv->Map(0, nullptr, &mapped);
		std::memset(mapped, 0, 64);
		_cbv->Unmap(0, nullptr);
	}
	cl->SetGraphicsRootConstantBufferView(0, _cbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, srv);
	if (srv2.ptr != 0) {
		cl->SetGraphicsRootDescriptorTable(2, srv2);
	}
	cl->SetGraphicsRootDescriptorTable(3, _samplerHeap->GetGPUDescriptorHandleForHeapStart());
	D3D12_GPU_DESCRIPTOR_HANDLE s1 = _samplerHeap->GetGPUDescriptorHandleForHeapStart();
	s1.ptr += _context.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	cl->SetGraphicsRootDescriptorTable(4, s1);
	cl->DrawInstanced(3, 1, 0, 0);
}
