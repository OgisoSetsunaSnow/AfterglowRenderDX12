#include "BoidsRenderer.h"
#include "ComputePass.h"
#include "Mesh.h"

#include <d3dx12.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

// 描述符堆布局（shader visible CBV_SRV_UAV，9 desc）：
//   0 = InstanceBuffer SRV（graphics VS t0）
//   1 = TerrainHeight SRV（sim t0）
//   2 = BoidBuffer UAV（sim u1 / attribute init u0）
//   3 = InstanceBuffer UAV（sim u0 / instance init u0）
//   4 = albedo SRV（graphics PS t0，白）
//   5 = normal SRV（graphics PS t1，DefaultNormal）
//   6-8 = 占位（graphics PS t2-t4，复用 albedo 避免空 SRV）
//   （graphics PS t5 ambient = index 4 albedo 复用，简化用同槽？——需独立 ambient）
//   修正：6 = ambient SRV（graphics PS t5）
namespace {
constexpr UINT kSrvInstance = 0;
constexpr UINT kSrvTerrain = 1;
constexpr UINT kUavBoid = 2;
constexpr UINT kUavInstance = 3;
constexpr UINT kSrvAlbedo = 4;
constexpr UINT kSrvNormal = 5;
constexpr UINT kSrvAmbient = 6;
constexpr UINT kDescriptorCount = 7;
constexpr UINT kInstanceCount = 1024;
constexpr UINT kGroups = kInstanceCount / 256; // 4
} // namespace

BoidsRenderer::BoidsRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& attributeInitCs,
	const std::vector<std::uint8_t>& instanceInitCs,
	const std::vector<std::uint8_t>& simCs,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	ID3D12Resource* terrainHeightTex,
	ID3D12Resource* albedoTex,
	ID3D12Resource* normalTex,
	ID3D12Resource* ambientTex)
	: _context(context),
	_attributeInitCs(attributeInitCs), _instanceInitCs(instanceInitCs), _simCs(simCs),
	_terrainHeightTex(terrainHeightTex), _albedoTex(albedoTex),
	_normalTex(normalTex), _ambientTex(ambientTex) {
	createBuffers();
	createComputeRootSignature();
	createGraphicsRootSignature();
	createPipelineState(vsBytes, psBytes);
	createSamplers();
	// ComputePass 成员（避免局部变量析构导致 #921）。
	_attrPass = std::make_unique<ComputePass>(_context, _attributeInitCs, 0, 1);
	_instPass = std::make_unique<ComputePass>(_context, _instanceInitCs, 0, 1);
	_simPass = std::make_unique<ComputePass>(_context, _simCs, 1, 2, true);
}

void BoidsRenderer::createBuffers() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create boids descriptor heap");

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);

	// InstanceBuffer：1024 × 192B（3 mat4）。
	const UINT64 instanceSize = sizeof(InstanceBufferElement) * kInstanceCount;
	D3D12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(instanceSize,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_instanceBuffer)),
		"Failed to create boids instance buffer");

	// BoidBuffer：1024 × 128B（8 × float4）。
	const UINT64 boidSize = sizeof(BoidBufferElement) * kInstanceCount;
	D3D12_RESOURCE_DESC bbDesc = CD3DX12_RESOURCE_DESC::Buffer(boidSize,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bbDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_boidBuffer)),
		"Failed to create boids boid buffer");

	const auto writeBufSrv = [&](UINT idx, ID3D12Resource* res, UINT stride) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.FirstElement = 0;
		srv.Buffer.NumElements = kInstanceCount;
		srv.Buffer.StructureByteStride = stride;
		device->CreateShaderResourceView(res, &srv, cpu);
	};
	const auto writeBufUav = [&](UINT idx, ID3D12Resource* res, UINT stride) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = kInstanceCount;
		uav.Buffer.StructureByteStride = stride;
		device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
	};
	const auto writeTexSrv = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = fmt;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = -1;
		device->CreateShaderResourceView(res, &srv, cpu);
	};

	// 0: InstanceBuffer SRV（graphics VS）
	writeBufSrv(kSrvInstance, _instanceBuffer.Get(), sizeof(InstanceBufferElement));
	// 1: TerrainHeight（纹理）
	{
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += kSrvTerrain * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32G32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(_terrainHeightTex, &srv, cpu);
	}
	// 2: BoidBuffer UAV（sim u1 / attribute init u0）
	writeBufUav(kUavBoid, _boidBuffer.Get(), sizeof(BoidBufferElement));
	// 3: InstanceBuffer UAV（sim u0 / instance init u0）
	writeBufUav(kUavInstance, _instanceBuffer.Get(), sizeof(InstanceBufferElement));
	// 4-6: albedo / normal / ambient 纹理
	writeTexSrv(kSrvAlbedo, _albedoTex, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	writeTexSrv(kSrvNormal, _normalTex, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeTexSrv(kSrvAmbient, _ambientTex, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

	// 上传堆：_simCbv（compute b0） + _meshCbv（graphics b0） + _frameCbv（graphics b1）。
	const CD3DX12_RESOURCE_DESC simCbvDesc = CD3DX12_RESOURCE_DESC::Buffer(192); // time/dt(16)+model(64)+invTrans(64)+objectID(16)
	const CD3DX12_RESOURCE_DESC meshCbvDesc = CD3DX12_RESOURCE_DESC::Buffer(512);
	const CD3DX12_RESOURCE_DESC frameCbvDesc = CD3DX12_RESOURCE_DESC::Buffer(768);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &simCbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_simCbv)),
		"Failed to create boids sim CBV");
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &meshCbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_meshCbv)),
		"Failed to create boids mesh CBV");
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &frameCbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_frameCbv)),
		"Failed to create boids frame CBV");
}

void BoidsRenderer::createComputeRootSignature() {
	// b0 CBV + t0 SRV（TerrainHeight）+ u0 UAV（InstanceBuffer）+ u1 UAV（BoidBuffer）。
	// 独立表，offset=0。
	D3D12_DESCRIPTOR_RANGE srv0{}, uav0{}, uav1{};
	srv0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv0.NumDescriptors = 1; srv0.BaseShaderRegister = 0;
	uav0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav0.NumDescriptors = 1; uav0.BaseShaderRegister = 0;
	uav1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav1.NumDescriptors = 1; uav1.BaseShaderRegister = 1;

	D3D12_ROOT_PARAMETER params[4]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srv0;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &uav0;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &uav1;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 4;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize boids compute root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_computeRootSig)),
		"Failed to create boids compute root signature");
}

void BoidsRenderer::createGraphicsRootSignature() {
	// b0 MeshUniform + b1 GlobalUniform（root CBV，ALL）。
	// VS t0 = InstanceBuffer（VERTEX）；PS t0-t5 纹理（PIXEL，同 register 不同 stage 合法）+ s0-s5。
	D3D12_DESCRIPTOR_RANGE vsSrv{};
	vsSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	vsSrv.NumDescriptors = 1;
	vsSrv.BaseShaderRegister = 0;
	vsSrv.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE psSrv{};
	psSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	psSrv.NumDescriptors = 6; // t0-t5
	psSrv.BaseShaderRegister = 0;
	psSrv.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 6; // s0-s5
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[5]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0 MeshUniform
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1; // b1 GlobalUniform
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &vsSrv;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &psSrv;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable.NumDescriptorRanges = 1;
	params[4].DescriptorTable.pDescriptorRanges = &samplerRange;
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 5;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize boids graphics root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_graphicsRootSig)),
		"Failed to create boids graphics root signature");
}

void BoidsRenderer::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps) {
	auto* device = _context.device();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _graphicsRootSig.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.PS = { ps.data(), ps.size() };
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 参考 BoidInstancing.mat cullMode 0
	desc.RasterizerState.FrontCounterClockwise = TRUE;
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;

	std::array<D3D12_INPUT_ELEMENT_DESC, 5> inputLayout = { {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	} };
	desc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };
	CHECK_OK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pso)),
		"Failed to create boids graphics PSO");
}

void BoidsRenderer::createSamplers() {
	auto* device = _context.device();
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heapDesc.NumDescriptors = 6;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create boids sampler heap");
	for (UINT i = 0; i < 6; ++i) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _samplerHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
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
		device->CreateSampler(&sd, cpu);
	}
}

void BoidsRenderer::initialize() {
	// 独立 command list：attribute init → instance init（用 ComputePass 精确 dispatch）。
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	auto* device = _context.device();
	CHECK_OK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
		"Failed to create boids init allocator");
	CHECK_OK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
		nullptr, IID_PPV_ARGS(&list)),
		"Failed to create boids init command list");

	const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = _heap->GetGPUDescriptorHandleForHeapStart();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_GPU_DESCRIPTOR_HANDLE boidUav{ heapStart.ptr + kUavBoid * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE instanceUav{ heapStart.ptr + kUavInstance * inc };

	_attrPass->dispatch(_heap.Get(), boidUav, kGroups, 1, 1, {}, list.Get());

	_instPass->dispatch(_heap.Get(), instanceUav, kGroups, 1, 1, {}, list.Get());

	CHECK_OK(list->Close(), "Failed to close boids init command list");
	ID3D12CommandList* const lists[] = { list.Get() };
	_context.commandQueue()->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	CHECK_OK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
		"Failed to create boids init fence");
	CHECK_OK(_context.commandQueue()->Signal(fence.Get(), 1), "Signal failed (boids init)");
	if (fence->GetCompletedValue() < 1) {
		HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(fence->SetEventOnCompletion(1, ev))) {
			throw std::runtime_error("SetEventOnCompletion failed (boids init)");
		}
		WaitForSingleObject(ev, INFINITE);
		CloseHandle(ev);
	}
}

void BoidsRenderer::render(const glm::mat4& view, const glm::mat4& proj,
	const glm::vec3& cameraPos, float time, float deltaTime,
	const Mesh& mesh, const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	const D3D12_CPU_DESCRIPTOR_HANDLE& dsv) {
	auto* cl = _context.commandList();
	const UINT inc = _context.device()->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = _heap->GetGPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE terrainSrv{ heapStart.ptr + kSrvTerrain * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE instanceUav{ heapStart.ptr + kUavInstance * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE boidUav{ heapStart.ptr + kUavBoid * inc };

	// 场景 model：scale 0.2 + rotate -90X（y-up）。
	const glm::mat4 model = glm::rotate(glm::scale(glm::mat4(1.0f), glm::vec3(0.2f)),
		glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::mat4 invTransModel = glm::inverseTranspose(model);

	// b0（sim）：time/deltaTime + model/invTransModel/objectID。
	{
		struct SimCB {
			float time, deltaTime; float pad[2];
			glm::mat4 model; glm::mat4 invTransModel;
			float objectID; float pad2[3];
		};
		static_assert(sizeof(SimCB) <= 192, "SimCB too large");
		SimCB* cb = nullptr;
		_simCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->time = time;
		cb->deltaTime = deltaTime;
		cb->model = model;
		cb->invTransModel = invTransModel;
		cb->objectID = 0.0f;
		_simCbv->Unmap(0, nullptr);
	}

	// 1) sim dispatch（t0=TerrainHeight, u0=InstanceBuffer, u1=BoidBuffer）。
	_simPass->dispatch(_heap.Get(),
		{ terrainSrv }, { instanceUav, boidUav },
		kGroups, 1, 1, nullptr, _simCbv->GetGPUVirtualAddress());

	// 2) InstanceBuffer 从 UAV → SRV（供 VS 读）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_instanceBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		cl->ResourceBarrier(1, &b);
	}

	// 3) 绑定并绘制（实例化 1024）。
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
		static_cast<float>(_context.width()), static_cast<float>(_context.height()), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(_context.width()), static_cast<LONG>(_context.height()) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

	ID3D12DescriptorHeap* const heaps[] = { _heap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_graphicsRootSig.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// MeshUniform（b0，graphics）：model=identity（实例变换来自 InstanceBuffer）。
	struct MeshUniformCB {
		glm::mat4 model; glm::mat4 invTransModel;
		glm::vec3 minAABB; float objectID;
		glm::vec3 maxAABB; float indexCount;
		glm::vec4 materialParams, materialParams2, materialParams3;
		glm::vec4 outlineColor, objectForward, objectRight, objectUp, outlineParams;
	};
	MeshUniformCB* mc = nullptr;
	_meshCbv->Map(0, nullptr, reinterpret_cast<void**>(&mc));
	mc->model = glm::mat4(1.0f);
	mc->invTransModel = glm::mat4(1.0f);
	mc->minAABB = glm::vec3(0.0f); mc->objectID = 0.0f;
	mc->maxAABB = glm::vec3(0.0f); mc->indexCount = 0.0f;
	mc->materialParams = glm::vec4(1.0f);
	mc->materialParams2 = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f); // metallic/specular/roughness/anisotropy
	mc->materialParams3 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // subsurfaceOpacity
	mc->outlineColor = glm::vec4(0.0f);
	mc->objectForward = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
	mc->objectRight = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
	mc->objectUp = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	mc->outlineParams = glm::vec4(0.0f);
	_meshCbv->Unmap(0, nullptr);

	// GlobalUniform（b1，graphics）：照搬 CharacterRenderer GlobalUniformCB。
	struct GlobalUniformCB {
		glm::mat4 view, projection, viewProjection, invView, invProjection, invViewProjection;
		glm::vec4 dirLightDirection, dirLightColor;
		glm::vec4 cameraPositionNear, cameraVectorFar;
		glm::vec2 screenResolution, invScreenResolution;
		glm::vec2 cursorPosition;
		float time, deltaTime;
		glm::vec4 frustumPlaneL, frustumPlaneR, frustumPlaneB, frustumPlaneT, frustumPlaneN, frustumPlaneF;
		float cameraFov, screenAspectRatio, invScreenAspectRatio, frameIndex;
	};
	static_assert(sizeof(GlobalUniformCB) <= 768, "GlobalUniformCB too large");
	GlobalUniformCB* gc = nullptr;
	_frameCbv->Map(0, nullptr, reinterpret_cast<void**>(&gc));
	gc->view = view;
	gc->projection = proj;
	gc->viewProjection = proj * view;
	gc->invView = glm::inverse(view);
	gc->invProjection = glm::inverse(proj);
	gc->invViewProjection = glm::inverse(proj * view);
	// 方向光（与 Main 一致：0xFFEEDD00 + π）。此处简化用白光 1.0，视觉近似。
	gc->dirLightDirection = glm::vec4(glm::normalize(glm::vec3(-0.5f, 0.7f, 0.4f)), 0.0f);
	gc->dirLightColor = glm::vec4(1.0f, 0.93f, 0.87f, 3.14159265f);
	gc->cameraPositionNear = glm::vec4(cameraPos, 0.1f);
	gc->cameraVectorFar = glm::vec4(0.0f, 0.0f, -1.0f, 1000.0f);
	gc->screenResolution = glm::vec2(static_cast<float>(_context.width()), static_cast<float>(_context.height()));
	gc->invScreenResolution = glm::vec2(1.0f / static_cast<float>(_context.width()), 1.0f / static_cast<float>(_context.height()));
	gc->cursorPosition = glm::vec2(0.0f);
	gc->time = time;
	gc->deltaTime = deltaTime;
	gc->cameraFov = 45.0f;
	gc->screenAspectRatio = static_cast<float>(_context.width()) / static_cast<float>(_context.height());
	gc->invScreenAspectRatio = 1.0f / gc->screenAspectRatio;
	gc->frameIndex = 0.0f;
	_frameCbv->Unmap(0, nullptr);

	cl->SetGraphicsRootConstantBufferView(0, _meshCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootConstantBufferView(1, _frameCbv->GetGPUVirtualAddress());
	// VS t0 = InstanceBuffer（heap index 0），PS t0-t5 = 纹理（heap index 4-9）。
	D3D12_GPU_DESCRIPTOR_HANDLE instanceSrv{ heapStart.ptr + kSrvInstance * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE texTable{ heapStart.ptr + kSrvAlbedo * inc };
	cl->SetGraphicsRootDescriptorTable(2, instanceSrv);
	cl->SetGraphicsRootDescriptorTable(3, texTable);
	cl->SetGraphicsRootDescriptorTable(4, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	const D3D12_VERTEX_BUFFER_VIEW vbView = mesh.vbView();
	const D3D12_INDEX_BUFFER_VIEW ibView = mesh.ibView();
	cl->IASetVertexBuffers(0, 1, &vbView);
	cl->IASetIndexBuffer(&ibView);
	cl->DrawIndexedInstanced(mesh.indexCount(), kInstanceCount, 0, 0, 0);

	// 4) InstanceBuffer 回 UAV（下帧 sim 写）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_instanceBuffer.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cl->ResourceBarrier(1, &b);
	}
}
