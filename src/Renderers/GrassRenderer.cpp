#include "GrassRenderer.h"
#include "ComputePass.h"
#include "Mesh.h"

#include <d3dx12.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

// 描述符堆布局（shader visible CBV_SRV_UAV，13 desc）：
//   0 = GrassData SRV（instancingPass t1）
//   1 = TerrainHeight SRV（GrassData_CS t0 / instancingPass t0）
//   2 = Meteorograph SRV（instancingPass t2）
//   3 = GrassData UAV（GrassData_CS u0）
//   4 = InstanceBuffer UAV（instancingPass u0）
//   5 = IndirectBuffer UAV（instancingPass u1）
//   6 = InstanceBuffer SRV（graphics VS t0）
//   7 = grassAlbedo SRV（graphics FS t0）
//   8 = FractalNormal SRV（graphics FS t1）
//   9 = TerrainNormal SRV（graphics FS t2）
//   10 = TerrainSurface SRV（graphics FS t3）
//   11 = Meteorograph SRV（graphics FS t4）
//   12 = ambient SRV（graphics FS t5）
namespace {
constexpr UINT kSrvGrassData = 0;
constexpr UINT kSrvTerrainHeight = 1;
constexpr UINT kSrvMeteorograph = 2;
constexpr UINT kUavGrassData = 3;
constexpr UINT kUavInstance = 4;
constexpr UINT kUavIndirect = 5;
constexpr UINT kSrvInstance = 6;
constexpr UINT kSrvGrassAlbedo = 7;
constexpr UINT kSrvFractalNormal = 8;
constexpr UINT kSrvTerrainNormal = 9;
constexpr UINT kSrvTerrainSurface = 10;
constexpr UINT kSrvMeteorographGfx = 11;
constexpr UINT kSrvAmbient = 12;
constexpr UINT kDescriptorCount = 13;

constexpr UINT kGrassDataSide = 2048;
constexpr UINT kGrassDataGroups = kGrassDataSide / 32; // 64
constexpr UINT kInstancingGroups = 128 / 32;           // 4

// InstanceBuffer struct（mat4 + mat4 + float2，HLSL 16B 对齐 → 144B）。
constexpr UINT kInstanceStride = 144;
} // namespace

GrassRenderer::GrassRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& grassDataCs,
	const std::vector<std::uint8_t>& instancingCs,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	ID3D12Resource* terrainHeightTex,
	ID3D12Resource* terrainNormalTex,
	ID3D12Resource* terrainSurfaceTex,
	ID3D12Resource* meteorographTex,
	const Texture& grassAlbedoTex,
	ID3D12Resource* fractalNormalTex,
	ID3D12Resource* ambientTex)
	: _context(context),
	_grassDataCs(grassDataCs), _instancingCs(instancingCs),
	_terrainHeightTex(terrainHeightTex), _terrainNormalTex(terrainNormalTex),
	_terrainSurfaceTex(terrainSurfaceTex), _meteorographTex(meteorographTex),
	_fractalNormalTex(fractalNormalTex), _ambientTex(ambientTex),
	_grassAlbedo(grassAlbedoTex) {
	createBuffers();
	createGraphicsRootSignature();
	createPipelineState(vsBytes, psBytes);
	createCommandSignature();
	createSamplers();
	// 两个 compute pass（各自根签名）。
	// GrassInstancing_CS 声明 SamplerState MeteorographSampler(s0) → 需 numSamplerRanges=1。
	_grassDataPass = std::make_unique<ComputePass>(_context, _grassDataCs, 1, 1, true);
	_instancingPass = std::make_unique<ComputePass>(_context, _instancingCs, 3, 2, true, 1);
}

void GrassRenderer::createBuffers() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create grass descriptor heap");

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);

	// GrassData 纹理：2048² R8G8B8A8_UNORM。
	{
		D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R8G8B8A8_UNORM, kGrassDataSide, kGrassDataSide, 1, 1);
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_grassDataTex)),
			"Failed to create grass data texture");
	}

	// InstanceBuffer：16384 × 144B。
	D3D12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(
		144ULL * kMaxInstances, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_instanceBuffer)),
		"Failed to create grass instance buffer");

	// IndirectBuffer：D3D12_DRAW_INDEXED_ARGUMENTS（20B）。
	D3D12_RESOURCE_DESC indDesc = CD3DX12_RESOURCE_DESC::Buffer(64,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_indirectBuffer)),
		"Failed to create grass indirect buffer");

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
	const auto writeBufSrv = [&](UINT idx, ID3D12Resource* res, UINT stride, UINT num) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.FirstElement = 0;
		srv.Buffer.NumElements = num;
		srv.Buffer.StructureByteStride = stride;
		device->CreateShaderResourceView(res, &srv, cpu);
	};
	const auto writeBufUav = [&](UINT idx, ID3D12Resource* res, UINT stride, UINT num) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = num;
		uav.Buffer.StructureByteStride = stride;
		device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
	};
	const auto writeTexUav = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = fmt;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
	};

	writeTexSrv(kSrvGrassData, _grassDataTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
	writeTexSrv(kSrvTerrainHeight, _terrainHeightTex, DXGI_FORMAT_R32G32_FLOAT);
	writeTexSrv(kSrvMeteorograph, _meteorographTex, DXGI_FORMAT_R32G32B32A32_FLOAT);
	writeTexUav(kUavGrassData, _grassDataTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
	writeBufUav(kUavInstance, _instanceBuffer.Get(), kInstanceStride, kMaxInstances);
	writeBufUav(kUavIndirect, _indirectBuffer.Get(), 20, 1);
	writeBufSrv(kSrvInstance, _instanceBuffer.Get(), kInstanceStride, kMaxInstances);
	writeTexSrv(kSrvGrassAlbedo, _grassAlbedo.resource(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	writeTexSrv(kSrvFractalNormal, _fractalNormalTex, DXGI_FORMAT_R8G8B8A8_SNORM);
	writeTexSrv(kSrvTerrainNormal, _terrainNormalTex, DXGI_FORMAT_R8G8B8A8_SNORM);
	writeTexSrv(kSrvTerrainSurface, _terrainSurfaceTex, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeTexSrv(kSrvMeteorographGfx, _meteorographTex, DXGI_FORMAT_R32G32B32A32_FLOAT);
	writeTexSrv(kSrvAmbient, _ambientTex, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

	const CD3DX12_RESOURCE_DESC cbvDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_simCbv)),
		"Failed to create grass sim CBV");
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_vsCbv)),
		"Failed to create grass VS CBV");
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_globalCbv)),
		"Failed to create grass global CBV");
	// 间接参数上传堆（每帧写 D3D12_DRAW_INDEXED_ARGUMENTS 到 _indirectBuffer）。
	const CD3DX12_RESOURCE_DESC indUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(64);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &indUploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_indirectUpload)),
		"Failed to create grass indirect upload buffer");

	// 启动诊断：IndirectBuffer（20B）+ 前 4 个实例（4×144B）的 READBACK 堆。
	const CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
	D3D12_RESOURCE_DESC dbgIndDesc = CD3DX12_RESOURCE_DESC::Buffer(32);
	CHECK_OK(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &dbgIndDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_debugReadbackIndirect)),
		"Failed to create grass indirect readback buffer");
	D3D12_RESOURCE_DESC dbgInstDesc = CD3DX12_RESOURCE_DESC::Buffer(4 * kInstanceStride);
	CHECK_OK(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &dbgInstDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_debugReadbackInstance)),
		"Failed to create grass instance readback buffer");
}

void GrassRenderer::createGraphicsRootSignature() {
	D3D12_DESCRIPTOR_RANGE vsSrv{};
	vsSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	vsSrv.NumDescriptors = 1;
	vsSrv.BaseShaderRegister = 0;
	vsSrv.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE psSrv{};
	psSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	psSrv.NumDescriptors = 6;
	psSrv.BaseShaderRegister = 0;
	psSrv.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 6;
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[5]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1; // b1
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
		throw std::runtime_error(std::string("Failed to serialize grass graphics root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_graphicsRootSig)),
		"Failed to create grass graphics root signature");
}

void GrassRenderer::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps) {
	auto* device = _context.device();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _graphicsRootSig.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.PS = { ps.data(), ps.size() };
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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
		"Failed to create grass graphics PSO");
}

void GrassRenderer::createCommandSignature() {
	// D3D12 #743：DRAW_INDEXED 参数不修改 root 参数 → CreateCommandSignature 的 pRootSignature 必须为 nullptr
	// （传根签名会报 CREATECOMMANDSIGNATURE_INVALID）。根签名在执行时通过 SetGraphicsRootSignature 独立设置。
	D3D12_INDIRECT_ARGUMENT_DESC arg{};
	arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
	sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); // 20
	sigDesc.NumArgumentDescs = 1;
	sigDesc.pArgumentDescs = &arg;
	CHECK_OK(_context.device()->CreateCommandSignature(&sigDesc, nullptr,
		IID_PPV_ARGS(&_cmdSig)),
		"Failed to create grass command signature");
}

void GrassRenderer::createSamplers() {
	auto* device = _context.device();
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heapDesc.NumDescriptors = 6;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create grass sampler heap");
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

void GrassRenderer::initialize() {
	// GrassData 一次性生成（GrassData_CS frameIndex=0 分支）。
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	auto* device = _context.device();
	CHECK_OK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
		"Failed to create grass init allocator");
	CHECK_OK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
		nullptr, IID_PPV_ARGS(&list)),
		"Failed to create grass init command list");

	const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = _heap->GetGPUDescriptorHandleForHeapStart();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_GPU_DESCRIPTOR_HANDLE terrainSrv{ heapStart.ptr + kSrvTerrainHeight * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE grassDataUav{ heapStart.ptr + kUavGrassData * inc };

	struct GrassDataCB { float frameIndex; float pad[3]; };
	GrassDataCB* cb = nullptr;
	_simCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
	cb->frameIndex = 0.0f;
	_simCbv->Unmap(0, nullptr);

	_grassDataPass->dispatch(_heap.Get(), { terrainSrv }, { grassDataUav },
		kGrassDataGroups, kGrassDataGroups, 1, list.Get(), _simCbv->GetGPUVirtualAddress());
	// GrassData UAV → SRV（供 instancingPass 读）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_grassDataTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &b);
	}

	CHECK_OK(list->Close(), "Failed to close grass init command list");
	ID3D12CommandList* const lists[] = { list.Get() };
	_context.commandQueue()->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	CHECK_OK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
		"Failed to create grass init fence");
	CHECK_OK(_context.commandQueue()->Signal(fence.Get(), 1), "Signal failed (grass init)");
	if (fence->GetCompletedValue() < 1) {
		HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(fence->SetEventOnCompletion(1, ev))) {
			throw std::runtime_error("SetEventOnCompletion failed (grass init)");
		}
		WaitForSingleObject(ev, INFINITE);
		CloseHandle(ev);
	}
	_grassDataInit = true;
}

void GrassRenderer::render(const glm::mat4& view, const glm::mat4& proj,
	const glm::vec3& cameraPos, const glm::vec3& cameraFwd,
	float time, float deltaTime, const Mesh& mesh,
	const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	const D3D12_CPU_DESCRIPTOR_HANDLE& dsv) {
	auto* cl = _context.commandList();
	const UINT inc = _context.device()->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = _heap->GetGPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE terrainSrv{ heapStart.ptr + kSrvTerrainHeight * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE grassDataSrv{ heapStart.ptr + kSrvGrassData * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE meteorographSrv{ heapStart.ptr + kSrvMeteorograph * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE instanceUav{ heapStart.ptr + kUavInstance * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE indirectUav{ heapStart.ptr + kUavIndirect * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE instanceSrv{ heapStart.ptr + kSrvInstance * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE texTable{ heapStart.ptr + kSrvGrassAlbedo * inc };

	// 启动诊断：帧 120 已把 IndirectBuffer/InstanceBuffer copy 到 READBACK，本帧读回打印
	// （beginFrame 已等待上一帧完成）。验证实例生成数 + 实例矩阵是否合法。
	if (_debugCopyPending) {
		debugPrintReadback();
		_debugCopyPending = false;
	}
	++_frameCounter;

	// b0（sim）：GrassInstancingParams（cameraPosition/cameraVector/frustumPlane 全 z-up）。
	// z-up 转换：y-up (x,y,z) → z-up (x, -z, y)。
	// cameraVector 语义：参考项目 = cameraPos - target（指向相机），所以 -cameraVector.xy 指向镜头前方。
	// Main.cpp 传入 cameraFwdGrass = 相机前向 (target-pos)，与参考相反，故这里对 fwd_zup 取反。
	{
		struct SimCB {
			float time, deltaTime; float pad[2];
			glm::vec3 cameraPosition; float cameraNear;
			glm::vec3 cameraVector; float cameraFar;
			glm::vec4 frustumPlaneL, frustumPlaneR, frustumPlaneB, frustumPlaneT, frustumPlaneN, frustumPlaneF;
		};
		static_assert(sizeof(SimCB) <= 256, "Grass SimCB too large");
		SimCB* cb = nullptr;
		_simCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->time = time;
		cb->deltaTime = deltaTime;
		cb->cameraPosition = glm::vec3(cameraPos.x, -cameraPos.z, cameraPos.y); // y-up → z-up
		// -fwd_zup = (pos - target) z-up：让 -cameraVector.xy 指向相机正前方（否则草长在身后）。
		cb->cameraVector = glm::vec3(-cameraFwd.x, cameraFwd.z, -cameraFwd.y);
		cb->cameraNear = 0.1f;
		cb->cameraFar = 1000.0f;
		// 视锥平面：从 z-up viewProj（proj*view*rotate(-90X)）用 Gribb-Hartmann 提取
		// （照搬参考 updateGlobalUniformFrustumPlanes：公式 + normalizePlane 归一化）。
		// CS 中 worldPos 是 z-up 坐标，平面也在 z-up 顶点空间，二者一致。
		// 必须归一化：球剔除 dist<-radius 的 radius 语义要求法线单位长，未归一化 → 有效半径缩小
		// （top/bottom |n|≈2.6 → 0.38m）→ 视锥边缘草被过度剔除（俯视时最明显）。
		{
			const glm::mat4 rotateMinus90X = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			const glm::mat4 vp = proj * view * rotateMinus90X;
			const glm::mat4 mt = glm::transpose(vp); // mt[i] = 原第 i 行
			const glm::vec4 r0 = mt[0], r1 = mt[1], r2 = mt[2], r3 = mt[3];
			const auto normPlane = [](const glm::vec4& p) {
				const float len = glm::length(glm::vec3(p));
				return len > 0.0f ? p / len : p;
			};
			// Gribb-Hartmann：left=row3+row0, right=row3-row0, bottom=row3+row1, top=row3-row1,
			// near=row3+row2, far=row3-row2（非 reverseDepth）。
			cb->frustumPlaneL = normPlane(r3 + r0);
			cb->frustumPlaneR = normPlane(r3 - r0);
			cb->frustumPlaneB = normPlane(r3 + r1);
			cb->frustumPlaneT = normPlane(r3 - r1);
			cb->frustumPlaneN = normPlane(r3 + r2);
			cb->frustumPlaneF = normPlane(r3 - r2);
		}
		_simCbv->Unmap(0, nullptr);
	}

	// 0) 每帧重置间接参数：indexCount = 网格索引数，instanceCount = 0。
	//    IndirectBuffer 当前为 UAV 状态（创建时），CPU 无法直接写 → 用上传堆 + CopyBufferRegion。
	{
		D3D12_DRAW_INDEXED_ARGUMENTS* args = nullptr;
		_indirectUpload->Map(0, nullptr, reinterpret_cast<void**>(&args));
		args->IndexCountPerInstance = mesh.indexCount();
		args->InstanceCount = 0;
		args->StartIndexLocation = 0;
		args->BaseVertexLocation = 0;
		args->StartInstanceLocation = 0;
		_indirectUpload->Unmap(0, nullptr);
		// UAV → COPY_DEST → 拷贝 → COPY_SOURCE（保持 UAV 供 dispatch 写）。
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_indirectBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COPY_DEST);
			cl->ResourceBarrier(1, &b);
		}
		cl->CopyBufferRegion(_indirectBuffer.Get(), 0, _indirectUpload.Get(), 0, 20);
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_indirectBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cl->ResourceBarrier(1, &b);
		}
	}

	// 1) instancing dispatch：t0=TerrainHeight t1=GrassData t2=Meteorograph；u0=InstanceBuffer u1=IndirectBuffer；s0=MeteorographSampler。
	_instancingPass->dispatch(_heap.Get(),
		{ terrainSrv, grassDataSrv, meteorographSrv },
		{ instanceUav, indirectUav },
		kInstancingGroups, kInstancingGroups, 1, nullptr, _simCbv->GetGPUVirtualAddress(),
		_samplerHeap.Get(), { _samplerHeap->GetGPUDescriptorHandleForHeapStart() });

	// 2) barrier：InstanceBuffer UAV→SRV（VS 读）；IndirectBuffer UAV→INDIRECT_ARGUMENT（ExecuteIndirect 读）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_instanceBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		cl->ResourceBarrier(1, &b);
	}
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_indirectBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		cl->ResourceBarrier(1, &b);
	}

	// 3) 绑定并 ExecuteIndirect。
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

	// b0（VS）：GrassUniform，布局必须与 GrassInstancing_VS/FS 的 cbuffer 一致：
	//   { float time; float deltaTime; float2 _pad0; float4x4 viewProjection; float objectID; float3 _pad1; }
	//   viewProjection 在 offset 16（不是 0）！之前把矩阵写在 offset 0 → shader 读到垃圾 → 拉伸大三角形。
	{
		const glm::mat4 rotateMinus90X = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		const glm::mat4 viewProjZup = proj * view * rotateMinus90X;
		struct VsCB {
			float time, deltaTime; float pad0[2]; // 0-15
			glm::mat4 viewProjection;             // 16-79
			float objectID; float pad1[3];        // 80+
		};
		VsCB* cb = nullptr;
		_vsCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->time = time;
		cb->deltaTime = deltaTime;
		cb->viewProjection = viewProjZup;
		cb->objectID = 0.0f;
		_vsCbv->Unmap(0, nullptr);
	}
	// b1（FS）：GrassGlobalUniform。FS 的 worldPosition/worldNormal 是 z-up（照搬参考），
	// 因此 cameraPosition/cameraVector/dirLightDirection 必须转 z-up（y-up (x,y,z) → z-up (x,-z,y)）。
	{
		struct GlobalCB {
			glm::vec3 cameraPosition; float gpad0;
			glm::vec3 dirLightDirection; float gpad1;
			glm::vec4 dirLightColor;
			glm::vec3 cameraVector; float gpad2;
		};
		GlobalCB* cb = nullptr;
		_globalCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->cameraPosition = glm::vec3(cameraPos.x, -cameraPos.z, cameraPos.y); // y-up → z-up
		cb->dirLightDirection = glm::normalize(glm::vec3(-0.5f, -0.4f, 0.7f));   // y-up(-0.5,0.7,0.4) → z-up
		cb->dirLightColor = glm::vec4(1.0f, 0.93f, 0.87f, 3.14159265f);
		// 与 SimCB 一致：cameraVector z-up = -fwd_zup = (pos-target) z-up（FS 未用，但保持语义一致）。
		cb->cameraVector = glm::vec3(-cameraFwd.x, cameraFwd.z, -cameraFwd.y);
		_globalCbv->Unmap(0, nullptr);
	}

	cl->SetGraphicsRootConstantBufferView(0, _vsCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootConstantBufferView(1, _globalCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(2, instanceSrv);
	cl->SetGraphicsRootDescriptorTable(3, texTable);
	cl->SetGraphicsRootDescriptorTable(4, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	const D3D12_VERTEX_BUFFER_VIEW vbView = mesh.vbView();
	const D3D12_INDEX_BUFFER_VIEW ibView = mesh.ibView();
	cl->IASetVertexBuffers(0, 1, &vbView);
	cl->IASetIndexBuffer(&ibView);
	cl->ExecuteIndirect(_cmdSig.Get(), 1, _indirectBuffer.Get(), 0, nullptr, 0);

	// 4) InstanceBuffer/IndirectBuffer 回 UAV（下帧 CS 写）。
	//    帧 120：先把 IndirectBuffer/InstanceBuffer copy 到 READBACK（启动诊断），再回 UAV。
	if (_frameCounter == 120) {
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_indirectBuffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			cl->ResourceBarrier(1, &b);
		}
		cl->CopyBufferRegion(_debugReadbackIndirect.Get(), 0, _indirectBuffer.Get(), 0, 20);
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_indirectBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cl->ResourceBarrier(1, &b);
		}
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_instanceBuffer.Get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			cl->ResourceBarrier(1, &b);
		}
		cl->CopyBufferRegion(_debugReadbackInstance.Get(), 0, _instanceBuffer.Get(), 0, 4 * kInstanceStride);
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_instanceBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cl->ResourceBarrier(1, &b);
		}
		_debugCopyPending = true;
	} else {
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_instanceBuffer.Get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cl->ResourceBarrier(1, &b);
		}
		{
			const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
				_indirectBuffer.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cl->ResourceBarrier(1, &b);
		}
	}
}

void GrassRenderer::debugPrintReadback() {
	D3D12_DRAW_INDEXED_ARGUMENTS args{};
	if (_debugReadbackIndirect) {
		void* p = nullptr;
		if (SUCCEEDED(_debugReadbackIndirect->Map(0, nullptr, &p)) && p) {
			std::memcpy(&args, p, sizeof(args));
			_debugReadbackIndirect->Unmap(0, nullptr);
		}
	}
	std::cout << "[Grass] indirect: indexCount=" << args.IndexCountPerInstance
		<< " instanceCount=" << args.InstanceCount
		<< " startIndex=" << args.StartIndexLocation
		<< " baseVertex=" << args.BaseVertexLocation
		<< " firstInstance=" << args.StartInstanceLocation << std::endl;
	if (_debugReadbackInstance) {
		float* f = nullptr;
		if (SUCCEEDED(_debugReadbackInstance->Map(0, nullptr, reinterpret_cast<void**>(&f))) && f) {
			for (int i = 0; i < 4; ++i) {
				const float* m = f + i * (kInstanceStride / 4); // 144B / 4 = 36 floats
				// HLSL row-major float4x4 内存：平移 = m[3](x) m[7](y) m[11](z)；wind 在 offset 128（float 32..33）。
				std::cout << "[Grass] inst[" << i << "] pos=(" << m[3] << ", " << m[7] << ", " << m[11]
					<< ") wind=(" << m[32] << ", " << m[33] << ")" << std::endl;
			}
			_debugReadbackInstance->Unmap(0, nullptr);
		}
	}
}
