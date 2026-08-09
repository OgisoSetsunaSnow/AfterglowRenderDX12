#include "ParticleRenderer.h"
#include "ComputePass.h"

#include <d3dx12.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

// 描述符堆布局（shader visible CBV_SRV_UAV，7 desc）：
//   0 = ParticleSSBO UAV（init/sim u0）
//   1 = ParticleSSBO SRV（graphics VS t0）
//   2 = TerrainHeight SRV（sim t0）
//   3 = Meteorograph SRV（sim t1）
//   4 = sceneCopy SRV（graphics FS t0，折射采样）
//   5 = TestTexture SRV（graphics FS t1）
//   6 = TestTexture UAV（init u1）
namespace {
constexpr UINT kUavParticle = 0;
constexpr UINT kSrvParticle = 1;
constexpr UINT kSrvTerrain = 2;
constexpr UINT kSrvMeteorograph = 3;
constexpr UINT kSrvSceneCopy = 4;
constexpr UINT kSrvTestTexture = 5;
constexpr UINT kUavTestTexture = 6;
constexpr UINT kDescriptorCount = 7;

constexpr UINT kInitGroups = 16; // 4096/256（Initializer 同 dispatchGroup [16,1,1]）
constexpr UINT kSimGroups = 16;  // 4096/256
} // namespace

ParticleRenderer::ParticleRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& initCs,
	const std::vector<std::uint8_t>& simCs,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& gsBytes,
	const std::vector<std::uint8_t>& psBytes,
	ID3D12Resource* terrainHeightTex, ID3D12Resource* meteorographTex,
	DXGI_FORMAT rtvFormat)
	: _context(context), _rtvFormat(rtvFormat),
	_terrainHeightTex(terrainHeightTex), _meteorographTex(meteorographTex) {
	createBuffers();
	createGraphicsRootSignature();
	createPipelineState(vsBytes, gsBytes, psBytes);
	createSamplers();
	_initPass = std::make_unique<ComputePass>(_context, initCs, 0, 2); // u0 particle + u1 TestTexture
	_simPass = std::make_unique<ComputePass>(_context, simCs, 2, 1, true, 1); // t0/t1 + u0 + b0 + s0
}

void ParticleRenderer::createBuffers() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create particle descriptor heap");

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);

	// ParticleSSBO：4096 × 48B（UAV + SRV）。
	D3D12_RESOURCE_DESC pbDesc = CD3DX12_RESOURCE_DESC::Buffer(
		static_cast<UINT64>(kParticleStride) * kParticleCount,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &pbDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_particleBuffer)),
		"Failed to create particle buffer");

	// TestTexture：128×32 R8G8B8A8_UNORM（init 写、FS Load(0,0)）。
	{
		D3D12_RESOURCE_DESC tDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R8G8B8A8_UNORM, 128, 32, 1, 1);
		tDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_testTexture)),
			"Failed to create particle test texture");
	}

	// sceneCopy：与 offscreen 同尺寸同格式（R16G16B16A16_FLOAT），折射采样源。
	{
		D3D12_RESOURCE_DESC sDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R16G16B16A16_FLOAT, _context.width(), _context.height(), 1, 1);
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &sDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_sceneCopy)),
			"Failed to create particle scene copy texture");
	}

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
	const auto writeBufSrv = [&](UINT idx, ID3D12Resource* res, UINT stride, UINT num) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Buffer.FirstElement = 0;
		srv.Buffer.NumElements = num;
		srv.Buffer.StructureByteStride = stride;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		device->CreateShaderResourceView(res, &srv, cpu);
	};
	const auto writeTexSrv = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = fmt;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		device->CreateShaderResourceView(res, &srv, cpu);
	};
	const auto writeTexUav = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = fmt;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(res, nullptr, &uav, cpu);
	};

	writeBufUav(kUavParticle, _particleBuffer.Get(), kParticleStride, kParticleCount);
	writeBufSrv(kSrvParticle, _particleBuffer.Get(), kParticleStride, kParticleCount);
	writeTexSrv(kSrvTerrain, _terrainHeightTex, DXGI_FORMAT_R32G32_FLOAT);
	writeTexSrv(kSrvMeteorograph, _meteorographTex, DXGI_FORMAT_R32G32B32A32_FLOAT);
	writeTexSrv(kSrvSceneCopy, _sceneCopy.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
	writeTexSrv(kSrvTestTexture, _testTexture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
	writeTexUav(kUavTestTexture, _testTexture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

	// b0/b1 上传 CBV 缓冲。
	const CD3DX12_RESOURCE_DESC cbvDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_simCbv)),
		"Failed to create particle sim CBV");
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_renderCbv)),
		"Failed to create particle render CBV");
}

void ParticleRenderer::createGraphicsRootSignature() {
	D3D12_DESCRIPTOR_RANGE vsSrv{};
	vsSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	vsSrv.NumDescriptors = 1; // t0 = ParticleSSBO（VS）
	vsSrv.BaseShaderRegister = 0;
	vsSrv.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE psSrv{};
	psSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	psSrv.NumDescriptors = 2; // t0 = sceneColor, t1 = TestTexture
	psSrv.BaseShaderRegister = 0;
	psSrv.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE psSampler{};
	psSampler.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	psSampler.NumDescriptors = 1; // s0 = sceneColorSampler
	psSampler.BaseShaderRegister = 0;
	psSampler.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[4]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &vsSrv;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &psSrv;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &psSampler;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 4;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // 无 IA（点精灵用 SV_VertexID）

	Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize particle root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_graphicsRootSig)),
		"Failed to create particle graphics root signature");
}

void ParticleRenderer::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& gs,
	const std::vector<std::uint8_t>& ps) {
	auto* device = _context.device();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _graphicsRootSig.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.GS = { gs.data(), gs.size() };
	desc.PS = { ps.data(), ps.size() };
	// Transparency 域：alpha blend（参考 ParticleExample.mat domain Transparency）。
	D3D12_BLEND_DESC blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	blend.RenderTarget[0].BlendEnable = TRUE;
	// 参考 ParticleExample.mat（Transparency）：alpha blend。
	blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState = blend;
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; // 参考 cullMode 2（GS winding 已修正保证两三角形 front-facing）
	desc.RasterizerState.FrontCounterClockwise = TRUE;
	D3D12_DEPTH_STENCIL_DESC ds = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	ds.DepthEnable = TRUE;
	ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // 参考 depthWrite false
	ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	desc.DepthStencilState = ds;
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = _rtvFormat;
	desc.SampleDesc.Count = 1;
	CHECK_OK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pso)),
		"Failed to create particle graphics PSO");
}

void ParticleRenderer::createSamplers() {
	auto* device = _context.device();
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create particle sampler heap");
	D3D12_SAMPLER_DESC sd{};
	sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.MaxAnisotropy = 1;
	sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = FLT_MAX;
	device->CreateSampler(&sd, _samplerHeap->GetCPUDescriptorHandleForHeapStart());
}

void ParticleRenderer::initialize() {
	// 一次性：ParticleExampleInitializer_CS（4096 粒子 + TestTexture）。
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	auto* device = _context.device();
	CHECK_OK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
		"Failed to create particle init allocator");
	CHECK_OK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
		nullptr, IID_PPV_ARGS(&list)),
		"Failed to create particle init command list");

	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = _heap->GetGPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE particleUav{ heapStart.ptr + kUavParticle * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE testTexUav{ heapStart.ptr + kUavTestTexture * inc };
	_initPass->dispatch(_heap.Get(), {}, { particleUav, testTexUav },
		kInitGroups, 1, 1, list.Get());
	// TestTexture：UAV → SRV（FS Load(0,0)）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_testTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &b);
	}

	CHECK_OK(list->Close(), "Failed to close particle init command list");
	ID3D12CommandList* const lists[] = { list.Get() };
	_context.commandQueue()->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	CHECK_OK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
		"Failed to create particle init fence");
	CHECK_OK(_context.commandQueue()->Signal(fence.Get(), 1), "Signal failed (particle init)");
	if (fence->GetCompletedValue() < 1) {
		HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(fence->SetEventOnCompletion(1, ev))) {
			throw std::runtime_error("SetEventOnCompletion failed (particle init)");
		}
		WaitForSingleObject(ev, INFINITE);
		CloseHandle(ev);
	}
}

void ParticleRenderer::render(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
	float time, float deltaTime, ID3D12Resource* sceneColorResource,
	const D3D12_CPU_DESCRIPTOR_HANDLE& rtv, const D3D12_CPU_DESCRIPTOR_HANDLE& dsv) {
	auto* cl = _context.commandList();
	const UINT inc = _context.device()->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = _heap->GetGPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE terrainSrv{ heapStart.ptr + kSrvTerrain * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE meteorographSrv{ heapStart.ptr + kSrvMeteorograph * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE particleUav{ heapStart.ptr + kUavParticle * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE particleSrv{ heapStart.ptr + kSrvParticle * inc };
	D3D12_GPU_DESCRIPTOR_HANDLE psTable{ heapStart.ptr + kSrvSceneCopy * inc }; // t0 sceneCopy + t1 TestTexture

	// b0（sim）：cameraPosition/prevCameraPosition/cameraFwd/Right/Up/FovY/FovX（z-up）+ time/deltaTime。
	// prevCameraPosition 用于粒子跟随相机（camDelta 平移，修复"粒子固定在初始区域"bug）。
	// cameraFwd/Right/Up/Fov 用于粒子在视锥内分布（修复"球体分布投影集中在屏幕中间"bug）。
	{
		const glm::vec3 camZup(cameraPos.x, -cameraPos.z, cameraPos.y); // y-up → z-up
		struct SimCB {
			glm::vec3 cameraPosition;     // 0-11
			float time;                   // 12-15
			glm::vec3 prevCameraPosition; // 16-27（z-up，粒子跟随相机 camDelta）
			float deltaTime;              // 28-31
		};
		SimCB* cb = nullptr;
		_simCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->cameraPosition = camZup;
		cb->time = time;
		cb->prevCameraPosition = _prevCameraValid ? _prevCameraPosYup : camZup; // 首帧 delta=0
		cb->deltaTime = deltaTime;
		_simCbv->Unmap(0, nullptr);
		_prevCameraPosYup = camZup;
		_prevCameraValid = true;
	}

	// 1) sim dispatch（u0 ParticleSSBO in-place）。
	_simPass->dispatch(_heap.Get(),
		{ terrainSrv, meteorographSrv }, { particleUav },
		kSimGroups, 1, 1, nullptr, _simCbv->GetGPUVirtualAddress(),
		_samplerHeap.Get(), { _samplerHeap->GetGPUDescriptorHandleForHeapStart() });

	// 2) offscreen 拷贝到 sceneCopy（折射采样；D3D12 禁止同资源 RTV+SRV）。
	{
		// sceneColorResource 当前 RENDER_TARGET → COPY_SOURCE。
		const D3D12_RESOURCE_BARRIER b1 = CD3DX12_RESOURCE_BARRIER::Transition(
			sceneColorResource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		cl->ResourceBarrier(1, &b1);
		if (_sceneCopyState != D3D12_RESOURCE_STATE_COPY_DEST) {
			const D3D12_RESOURCE_BARRIER b2 = CD3DX12_RESOURCE_BARRIER::Transition(
				_sceneCopy.Get(), _sceneCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
			cl->ResourceBarrier(1, &b2);
		}
		D3D12_TEXTURE_COPY_LOCATION dst{ _sceneCopy.Get() };
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION src{ sceneColorResource };
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		const D3D12_RESOURCE_BARRIER b3 = CD3DX12_RESOURCE_BARRIER::Transition(
			_sceneCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		cl->ResourceBarrier(1, &b3);
		_sceneCopyState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		const D3D12_RESOURCE_BARRIER b4 = CD3DX12_RESOURCE_BARRIER::Transition(
			sceneColorResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->ResourceBarrier(1, &b4);
	}

	// 3) ParticleSSBO UAV → SRV（VS 读）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_particleBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		cl->ResourceBarrier(1, &b);
	}

	// 4) 绑定并绘制 4096 个点。
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
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

	// b0（graphics）：viewProjection z-up + screenResolution + cameraFov。
	{
		const glm::mat4 rotateMinus90X = glm::rotate(glm::mat4(1.0f),
			glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		struct RenderCB {
			glm::mat4 viewProjection;
			glm::vec2 screenResolution;
			glm::vec2 invScreenResolution;
			float cameraFov;
			float pad0;
			glm::vec2 pad1;
		};
		RenderCB* cb = nullptr;
		_renderCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->viewProjection = proj * view * rotateMinus90X; // z-up viewProj
		cb->screenResolution = glm::vec2(static_cast<float>(_context.width()),
			static_cast<float>(_context.height()));
		cb->invScreenResolution = 1.0f / cb->screenResolution;
		cb->cameraFov = 0.785398f; // 45°
		cb->pad0 = 0.0f;
		cb->pad1 = glm::vec2(0.0f);
		_renderCbv->Unmap(0, nullptr);
	}
	cl->SetGraphicsRootConstantBufferView(0, _renderCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, particleSrv);
	cl->SetGraphicsRootDescriptorTable(2, psTable);
	cl->SetGraphicsRootDescriptorTable(3, _samplerHeap->GetGPUDescriptorHandleForHeapStart());
	cl->DrawInstanced(kParticleCount, 1, 0, 0);

	// 5) ParticleSSBO SRV → UAV（下帧 sim 写）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_particleBuffer.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cl->ResourceBarrier(1, &b);
	}
}
