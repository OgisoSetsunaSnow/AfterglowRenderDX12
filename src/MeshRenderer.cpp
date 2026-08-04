#include "MeshRenderer.h"
#include "Mesh.h"
#include "Texture.h"

#include <d3dx12.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

MeshRenderer::MeshRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	const std::vector<Texture*>& textures,
	UINT maxObjects,
	const std::vector<std::uint8_t>& outlineVsBytes,
	const std::vector<std::uint8_t>& outlinePsBytes,
	DXGI_FORMAT rtvFormat)
	: _context(context), _maxObjects(maxObjects), _rtvFormat(rtvFormat) {
	createRootSignature();
	createPipelineState(vsBytes, psBytes);
	if (!outlineVsBytes.empty() && !outlinePsBytes.empty()) {
		createOutlinePipelineState(outlineVsBytes, outlinePsBytes);
	}
	createDepthBuffer();
	createTextureDescriptors(textures);
	createPerObjectCbv(maxObjects);
	createFrameCbv();
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

	D3D12_ROOT_PARAMETER rootParams[4]{};
	rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParams[0].Descriptor.ShaderRegister = 0; // b0
	rootParams[0].Descriptor.RegisterSpace = 0;
	rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // VS(mvp/world) + PS(baseColor) 共用 b0

	rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParams[1].Descriptor.ShaderRegister = 1; // b1 FrameCB（光照 PS 用；描边 VS 用 camera/投影参数）
	rootParams[1].Descriptor.RegisterSpace = 0;
	rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange;
	rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[3].DescriptorTable.pDescriptorRanges = &samplerRange;
	rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 4;
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
	psoDesc.RTVFormats[0] = _rtvFormat;
	psoDesc.SampleDesc.Count = 1;

	std::array<D3D12_INPUT_ELEMENT_DESC, 4> inputLayout = { {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	} };
	psoDesc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };

	HRESULT hr = _context.device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pso));
	CHECK_OK(hr, "Failed to create graphics pipeline state");
}

void MeshRenderer::createOutlinePipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps) {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = _rootSignature.Get();
	psoDesc.VS = { vs.data(), vs.size() };
	psoDesc.PS = { ps.data(), ps.size() };

	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	// 对齐参考项目 ArcOutline.mat：cullMode=Front（VK_CULL_MODE_FRONT_BIT）+ CCW front。
	// 即剔除正面、只画背面（远侧），配合 VS 沿法线外扩形成轮廓。
	// 参考项目仅对闭合模型网格启用 outline，因此单 pass 在任意角度都正确。
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = _rtvFormat;
	psoDesc.SampleDesc.Count = 1;

	// 描边 shader 只需要 pos + normal。
	std::array<D3D12_INPUT_ELEMENT_DESC, 2> inputLayout = { {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	} };
	psoDesc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };

	HRESULT hr = _context.device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_outlinePso));
	CHECK_OK(hr, "Failed to create outline pipeline state");
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

void MeshRenderer::createFrameCbv() {
	auto* device = _context.device();

	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(CbvStride);
	HRESULT hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_frameCbv));
	CHECK_OK(hr, "Failed to create frame CBV buffer");
}

void MeshRenderer::setLight(const glm::vec3& dir, const glm::vec3& color, const glm::vec3& ambient) {
	_lightDir = dir;
	_lightColor = color;
	_ambient = ambient;
}

void MeshRenderer::render(const glm::mat4& view, const glm::mat4& proj,
	const std::vector<DrawItem>& items) {
	render(view, proj, items, _context.currentRtv());
}

void MeshRenderer::render(const glm::mat4& view, const glm::mat4& proj,
	const std::vector<DrawItem>& items, const D3D12_CPU_DESCRIPTOR_HANDLE& rtv) {
	auto* cl = _context.commandList();

	// 视口、清深度、绑定 RTV（默认后台缓冲，可指定离屏 sceneColor）。
	const UINT w = _context.width();
	const UINT h = _context.height();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };

	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = _dsvHeap->GetCPUDescriptorHandleForHeapStart();

	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 公共状态。
	ID3D12DescriptorHeap* const heaps[] = { _srvHeap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->SetGraphicsRootDescriptorTable(3, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	// 每帧常量 CBV（b1）：光照 + 相机 + 描边参数。
	FrameCB* frame = nullptr;
	_frameCbv->Map(0, nullptr, reinterpret_cast<void**>(&frame));
	const glm::mat4 invView = glm::inverse(view);
	frame->lightDir = glm::vec4(glm::normalize(_lightDir), 0.0f);
	frame->lightColor = glm::vec4(_lightColor, 1.0f); // w = 光强（同参考项目 dirLightColor.a，默认 1.0）
	frame->ambientColor = glm::vec4(_ambient, 0.0f);
	frame->cameraPosition = glm::vec4(glm::vec3(invView[3]), 0.0f);
	frame->invProjection00 = 1.0f / proj[0][0];
	frame->invScreenAspectRatio = static_cast<float>(h) / static_cast<float>(w);
	frame->outlineDistanceFadeFactor = 0.001f; // 参考项目 ArcOutline.outlineDistanceFadeFactor
	_frameCbv->Unmap(0, nullptr);
	cl->SetGraphicsRootConstantBufferView(1, _frameCbv->GetGPUVirtualAddress());

	// 每物体 CBV + 绘制。
	PerObjectCB* mapped = nullptr;
	_perObjectCbv->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
	const D3D12_GPU_VIRTUAL_ADDRESS cbvBase = _perObjectCbv->GetGPUVirtualAddress();

	// 阶段 9：描边（背面外扩）→ 主体，同一根签名，仅 PSO 不同。
	const auto drawMesh = [&](const DrawItem& item, UINT index, bool isOutline) {
		PerObjectCB* cb = reinterpret_cast<PerObjectCB*>(
			reinterpret_cast<std::uint8_t*>(mapped) + index * CbvStride);
		cb->viewProj = proj * view;
		cb->world = item.model;
		cb->invTransModel = glm::transpose(glm::inverse(item.model)); // 逆转置，法线正确变换
		cb->baseColor = isOutline ? glm::vec4(0.0f) : item.color;
		cb->outlineWidth = item.outlineWidth;

		if (!isOutline) {
			const UINT srvIndex = std::min(item.srvIndex, _srvCount - 1);
			const D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{
				_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr
				+ static_cast<SIZE_T>(srvIndex) * _srvDescriptorSize };
			cl->SetGraphicsRootDescriptorTable(2, srvHandle);
		}

		const D3D12_VERTEX_BUFFER_VIEW vbView = item.mesh->vbView();
		const D3D12_INDEX_BUFFER_VIEW ibView = item.mesh->ibView();
		cl->IASetVertexBuffers(0, 1, &vbView);
		cl->IASetIndexBuffer(&ibView);
		cl->SetGraphicsRootConstantBufferView(0, cbvBase + index * CbvStride);
		cl->DrawIndexedInstanced(item.mesh->indexCount(), 1, 0, 0, 0);
	};

	if (_outlinePso) {
		cl->SetPipelineState(_outlinePso.Get());
		for (std::size_t i = 0; i < items.size() && i < _maxObjects; ++i) {
			// 仅对显式开启 outline 的物体画描边（对齐参考项目：材质级，非全局）。
			if (items[i].mesh && items[i].mesh->valid() && items[i].outline) {
				drawMesh(items[i], static_cast<UINT>(i), true);
			}
		}
	}

	cl->SetPipelineState(_pso.Get());
	for (std::size_t i = 0; i < items.size() && i < _maxObjects; ++i) {
		if (items[i].mesh && items[i].mesh->valid()) {
			drawMesh(items[i], static_cast<UINT>(i), false);
		}
	}

	_perObjectCbv->Unmap(0, nullptr);
}
