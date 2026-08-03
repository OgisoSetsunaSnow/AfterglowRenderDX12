#include "MeshRenderer.h"
#include "Mesh.h"
#include "Texture.h"

#include <d3dx12.h>

#include <algorithm>
#include <array>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

MeshRenderer::MeshRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	const std::vector<Texture*>& textures,
	UINT maxObjects)
	: _context(context), _maxObjects(maxObjects) {
	createRootSignature();
	createPipelineState(vsBytes, psBytes);
	createDepthBuffer();
	createTextureDescriptors(textures);
	createPerObjectCbv(maxObjects);
}

MeshRenderer::~MeshRenderer() = default;

void MeshRenderer::createRootSignature() {
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0; // t0
	srvRange.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 1;
	samplerRange.BaseShaderRegister = 0; // s0
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER rootParams[3]{};
	rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParams[0].Descriptor.ShaderRegister = 0; // b0
	rootParams[0].Descriptor.RegisterSpace = 0;
	rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // VS(mvp) + PS(baseColor) 共用 b0

	rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
	rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[2].DescriptorTable.pDescriptorRanges = &samplerRange;
	rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = rootParams;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize root signature: ") + msg);
	}
	hr = _context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
	CHECK_OK(hr, "Failed to create root signature");
}

void MeshRenderer::createPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps) {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = _rootSignature.Get();
	psoDesc.VS = { vs.data(), vs.size() };
	psoDesc.PS = { ps.data(), ps.size() };

	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 双面渲染，避免单面几何从某些视角消失
	psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;

	std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputLayout = { {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	} };
	psoDesc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };

	HRESULT hr = _context.device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pso));
	CHECK_OK(hr, "Failed to create graphics pipeline state");
}

void MeshRenderer::createDepthBuffer() {
	auto* device = _context.device();
	const UINT width = _context.width();
	const UINT height = _context.height();

	D3D12_RESOURCE_DESC depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_D32_FLOAT, width, height, 1, 1);
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;

	const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
		IID_PPV_ARGS(&_depthBuffer));
	CHECK_OK(hr, "Failed to create depth buffer");

	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = 1;
	hr = device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&_dsvHeap));
	CHECK_OK(hr, "Failed to create DSV heap");
	device->CreateDepthStencilView(_depthBuffer.Get(), nullptr,
		_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void MeshRenderer::createTextureDescriptors(const std::vector<Texture*>& textures) {
	auto* device = _context.device();
	_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// SRV heap：每个纹理一个描述符。
	const UINT srvCount = std::max<UINT>(1, static_cast<UINT>(textures.size()));
	_srvCount = srvCount;
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.NumDescriptors = srvCount;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&_srvHeap)),
		"Failed to create SRV descriptor heap");

	// 每个纹理写入 SRV。
	for (UINT i = 0; i < srvCount; ++i) {
		D3D12_CPU_DESCRIPTOR_HANDLE handle{
			_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr
			+ static_cast<SIZE_T>(i) * _srvDescriptorSize };
		textures[i]->createSrv(device, handle);
	}

	D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
	samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerHeapDesc.NumDescriptors = 1;
	samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create sampler descriptor heap");

	D3D12_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.MinLOD = 0.0f;
	samplerDesc.MaxLOD = 0.0f;
	device->CreateSampler(&samplerDesc, _samplerHeap->GetCPUDescriptorHandleForHeapStart());
}

void MeshRenderer::createPerObjectCbv(UINT maxObjects) {
	auto* device = _context.device();
	const std::uint64_t totalSize = static_cast<std::uint64_t>(maxObjects) * CbvStride;

	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
	HRESULT hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_perObjectCbv));
	CHECK_OK(hr, "Failed to create per-object CBV buffer");
}

void MeshRenderer::render(const glm::mat4& view, const glm::mat4& proj,
	const std::vector<DrawItem>& items) {
	auto* cl = _context.commandList();

	// 视口、清深度、绑定 RTV。
	const UINT w = _context.width();
	const UINT h = _context.height();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };

	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _context.currentRtv();
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = _dsvHeap->GetCPUDescriptorHandleForHeapStart();

	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 公共状态。
	ID3D12DescriptorHeap* const heaps[] = { _srvHeap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->SetGraphicsRootDescriptorTable(2, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	// 每物体 CBV + 绘制。
	PerObjectCB* mapped = nullptr;
	_perObjectCbv->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
	const D3D12_GPU_VIRTUAL_ADDRESS cbvBase = _perObjectCbv->GetGPUVirtualAddress();

	for (std::size_t i = 0; i < items.size() && i < _maxObjects; ++i) {
		const DrawItem& item = items[i];
		if (!item.mesh || !item.mesh->valid()) {
			continue;
		}
		PerObjectCB* cb = reinterpret_cast<PerObjectCB*>(
			reinterpret_cast<std::uint8_t*>(mapped) + i * CbvStride);
		cb->mvp = proj * view * item.model;
		cb->baseColor = item.color;

		// 绑定物体对应的纹理 SRV。
		const UINT srvIndex = std::min(item.srvIndex, _srvCount - 1);
		const D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{
			_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr
			+ static_cast<SIZE_T>(srvIndex) * _srvDescriptorSize };
		cl->SetGraphicsRootDescriptorTable(1, srvHandle);

		const D3D12_VERTEX_BUFFER_VIEW vbView = item.mesh->vbView();
		const D3D12_INDEX_BUFFER_VIEW ibView = item.mesh->ibView();
		cl->IASetVertexBuffers(0, 1, &vbView);
		cl->IASetIndexBuffer(&ibView);
		cl->SetGraphicsRootConstantBufferView(0, cbvBase + i * CbvStride);
		cl->DrawIndexedInstanced(item.mesh->indexCount(), 1, 0, 0, 0);
	}

	_perObjectCbv->Unmap(0, nullptr);
}
