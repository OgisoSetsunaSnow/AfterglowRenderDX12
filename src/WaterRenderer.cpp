#include "WaterRenderer.h"
#include "ComputePass.h"
#include "Mesh.h"

#include <d3dx12.h>

#include <array>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

// 描述符堆布局（本 heap，shader visible CBV_SRV_UAV，9 个描述符）：
//   0 = waterSceneCopy（离屏场景色拷贝，折射采样源）
//   1 = depth（场景深度）
//   2 = TerrainHeight
//   3 = TerrainNormal
//   4 = WaterSediment
//   5 = WaterVelocity
//   6 = FractalSimplexNoise SRV
//   7 = ambientTexture（DefaultSky IBL）
//   8 = FractalSimplexNoise UAV（init dispatch 用，shader visible）
namespace {
constexpr UINT kSrvSceneCopy = 0;
constexpr UINT kSrvDepth = 1;
constexpr UINT kSrvHeight = 2;
constexpr UINT kSrvNormal = 3;
constexpr UINT kSrvSediment = 4;
constexpr UINT kSrvVelocity = 5;
constexpr UINT kSrvFractal = 6;
constexpr UINT kSrvAmbient = 7;
constexpr UINT kUavFractal = 8;
constexpr UINT kDescriptorCount = 9;
constexpr UINT kFractalSide = 256;
} // namespace

WaterRenderer::WaterRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	const std::vector<std::uint8_t>& fractalInitCsBytes,
	ID3D12Resource* heightTex, ID3D12Resource* normalTex,
	ID3D12Resource* sedimentTex, ID3D12Resource* velocityTex,
	ID3D12Resource* depthTex, const Texture& ambientTex,
	DXGI_FORMAT rtvFormat)
	: _context(context), _fractalInitCs(fractalInitCsBytes),
	_heightTex(heightTex), _normalTex(normalTex),
	_sedimentTex(sedimentTex), _velocityTex(velocityTex), _depthTex(depthTex),
	_ambient(ambientTex), _ambientFormat(ambientTex.format()) {
	createTextures();
	createRootSignature();
	createPipelineState(vsBytes, psBytes, rtvFormat);
	createCbv();
	createSamplers();

	// 水网格：256x256 interval 4.0（照搬参考 WaterVertexInitializer/WaterIndexInitializer）。
	_waterMesh = std::make_unique<Mesh>();
	_waterMesh->create(context, Mesh::makeTerrainVertices(256, 4.0f), Mesh::makeTerrainIndices(256));
}

void WaterRenderer::createTextures() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create water descriptor heap");

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	const UINT w = _context.width();
	const UINT h = _context.height();

	// waterSceneCopy：与 offscreen 同尺寸同格式（R16G16B16A16_FLOAT），仅作拷贝目标+SRV。
	{
		D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1, 1);
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_sceneCopy)),
			"Failed to create water scene copy texture");
	}

	// FractalSimplexNoise：256x256 R32Float（参考 FractalNoise.mat textureMode 6 = R32Float）。
	{
		D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R32_FLOAT, kFractalSide, kFractalSide, 1, 1);
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_fractalTex)),
			"Failed to create fractal simplex noise texture");
	}

	// SRV 描述符（写本 heap index 0-7）。
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
	writeSrv(kSrvSceneCopy, _sceneCopy.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
	writeSrv(kSrvDepth, _depthTex, DXGI_FORMAT_R32_FLOAT);
	writeSrv(kSrvHeight, _heightTex, DXGI_FORMAT_R32G32_FLOAT);
	writeSrv(kSrvNormal, _normalTex, DXGI_FORMAT_R8G8B8A8_SNORM);
	writeSrv(kSrvSediment, _sedimentTex, DXGI_FORMAT_R32_FLOAT);
	writeSrv(kSrvVelocity, _velocityTex, DXGI_FORMAT_R16G16_FLOAT);
	writeSrv(kSrvFractal, _fractalTex.Get(), DXGI_FORMAT_R32_FLOAT);
	writeSrv(kSrvAmbient, _ambient.resource(), _ambientFormat);
	_fractalSrvGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_fractalSrvGpu.ptr += kSrvFractal * inc;

	// fractal UAV（写本 heap index 8，shader visible → dispatch 可用）。
	{
		D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = _heap->GetCPUDescriptorHandleForHeapStart();
		uavCpu.ptr += kUavFractal * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_FLOAT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(_fractalTex.Get(), nullptr, &uav, uavCpu);
		_fractalUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
		_fractalUavGpu.ptr += kUavFractal * inc;
	}
}

void WaterRenderer::initializeFractalNoise() {
	// 独立 command list 一次性 dispatch（照搬 Main 中 terrain init 模式）。
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	auto* device = _context.device();
	CHECK_OK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
		"Failed to create water init allocator");
	CHECK_OK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
		nullptr, IID_PPV_ARGS(&list)),
		"Failed to create water init command list");

	// ComputePass（0 SRV + 1 UAV）+ dispatch（256/32=8 groups）。
	ComputePass fractalPass(_context, _fractalInitCs, 0, 1);
	fractalPass.dispatch(_heap.Get(), _fractalUavGpu, 8, 8, 1, {}, list.Get());

	// barrier fractal → 采样态（供水 pass 采样）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_fractalTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &b);
	}

	CHECK_OK(list->Close(), "Failed to close water init command list");
	ID3D12CommandList* const lists[] = { list.Get() };
	_context.commandQueue()->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	CHECK_OK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
		"Failed to create water init fence");
	CHECK_OK(_context.commandQueue()->Signal(fence.Get(), 1), "Signal failed (water init)");
	if (fence->GetCompletedValue() < 1) {
		HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(fence->SetEventOnCompletion(1, ev))) {
			throw std::runtime_error("SetEventOnCompletion failed (water init)");
		}
		WaitForSingleObject(ev, INFINITE);
		CloseHandle(ev);
	}
}

void WaterRenderer::copySceneColor(ID3D12Resource* sceneResource) {
	auto* cl = _context.commandList();
	// sceneResource（offscreen）当前应处于 COPY_SOURCE（调用方已 barrier）。
	// waterSceneCopy 需先回 COPY_DEST（上帧末是 PIXEL_SHADER_RESOURCE）。
	if (_sceneCopyState != D3D12_RESOURCE_STATE_COPY_DEST) {
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_sceneCopy.Get(), _sceneCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
		cl->ResourceBarrier(1, &b);
		_sceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	// 拷贝离屏全图到 waterSceneCopy。
	D3D12_TEXTURE_COPY_LOCATION dst{ _sceneCopy.Get() };
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{ sceneResource };
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;
	cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	// barrier 回采样态（供水 pass 折射采样）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_sceneCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		cl->ResourceBarrier(1, &b);
	}
	_sceneCopyState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void WaterRenderer::render(const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	const D3D12_CPU_DESCRIPTOR_HANDLE& dsv,
	const glm::mat4& viewProj, const glm::mat4& model,
	const glm::vec3& cameraPos, const glm::vec3& cameraFwd,
	const glm::vec3& lightDir, const glm::vec4& lightColor,
	UINT width, UINT height, float nearZ, float farZ, float time) {
	auto* cl = _context.commandList();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);

	cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

	ID3D12DescriptorHeap* const heaps[] = { _heap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	struct WaterCB {
		glm::mat4 viewProj;
		glm::mat4 model;
		glm::vec4 cameraPosition;
		glm::vec4 cameraVector;
		glm::vec4 dirLightDirection;
		glm::vec4 dirLightColor;
		glm::vec2 screenResolution;
		glm::vec2 invScreenResolution;
		float nearZ;
		float farZ;
		float time;
		float _pad;
	};
	static_assert(sizeof(WaterCB) % 16 == 0, "WaterCB must be 16-byte aligned");
	WaterCB* cb = nullptr;
	_cbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
	cb->viewProj = viewProj;
	cb->model = model;
	cb->cameraPosition = glm::vec4(cameraPos, 1.0f);
	cb->cameraVector = glm::vec4(cameraFwd, 0.0f);
	cb->dirLightDirection = glm::vec4(glm::normalize(lightDir), 0.0f);
	cb->dirLightColor = lightColor;
	cb->screenResolution = glm::vec2(static_cast<float>(width), static_cast<float>(height));
	cb->invScreenResolution = glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
	cb->nearZ = nearZ;
	cb->farZ = farZ;
	cb->time = time;
	cb->_pad = 0.0f;
	_cbv->Unmap(0, nullptr);

	cl->SetGraphicsRootConstantBufferView(0, _cbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, _heap->GetGPUDescriptorHandleForHeapStart());
	cl->SetGraphicsRootDescriptorTable(2, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	const D3D12_VERTEX_BUFFER_VIEW vbView = _waterMesh->vbView();
	const D3D12_INDEX_BUFFER_VIEW ibView = _waterMesh->ibView();
	cl->IASetVertexBuffers(0, 1, &vbView);
	cl->IASetIndexBuffer(&ibView);
	cl->DrawIndexedInstanced(_waterMesh->indexCount(), 1, 0, 0, 0);
}

void WaterRenderer::createRootSignature() {
	// b0 CBV + t0-t7 连续 SRV 表（8 个）+ s0-s1 sampler 表。
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = kDescriptorCount;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 2;
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[3]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srvRange;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &samplerRange;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize water root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create water root signature");
}

void WaterRenderer::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat) {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _rootSignature.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.PS = { ps.data(), ps.size() };

	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	// 参考 WaterMesh.mat cullMode 0 = None（双面）。
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.FrontCounterClockwise = TRUE;
	// 参考 WaterMesh.mat depthWrite false；深度测试仍启用（水被地形遮挡）。
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = rtvFormat;
	desc.SampleDesc.Count = 1;

	std::array<D3D12_INPUT_ELEMENT_DESC, 4> inputLayout = { {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	} };
	desc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };

	CHECK_OK(_context.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pso)),
		"Failed to create water pipeline state");
}

void WaterRenderer::createCbv() {
	auto* device = _context.device();
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_cbv)),
		"Failed to create water CBV buffer");
}

void WaterRenderer::createSamplers() {
	auto* device = _context.device();
	D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
	samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerHeapDesc.NumDescriptors = 2;
	samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create water sampler heap");

	D3D12_CPU_DESCRIPTOR_HANDLE s0 = _samplerHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE s1 = s0;
	s1.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	// s0: waterSampler（地形数据/噪声采样，ANISO16/WRAP 双线性等效）。
	D3D12_SAMPLER_DESC waterSamp{};
	waterSamp.Filter = D3D12_FILTER_ANISOTROPIC;
	waterSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	waterSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	waterSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	waterSamp.MipLODBias = 0.0f;
	waterSamp.MaxAnisotropy = 16;
	waterSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	waterSamp.MinLOD = 0.0f;
	waterSamp.MaxLOD = FLT_MAX;
	device->CreateSampler(&waterSamp, s0);

	// s1: sceneColorTextureSampler（折射场景色，LINEAR/CLAMP）。
	D3D12_SAMPLER_DESC sceneSamp{};
	sceneSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sceneSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sceneSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sceneSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sceneSamp.MaxAnisotropy = 1;
	sceneSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sceneSamp.MinLOD = 0.0f;
	sceneSamp.MaxLOD = FLT_MAX;
	device->CreateSampler(&sceneSamp, s1);
}
