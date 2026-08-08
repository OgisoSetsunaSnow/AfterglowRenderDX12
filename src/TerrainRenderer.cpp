#include "TerrainRenderer.h"
#include "Mesh.h"

#include <d3dx12.h>

#include <array>
#include <cfloat>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

// 描述符堆布局（SHADER_VISIBLE CBV_SRV_UAV）：
//   SRV 集中 0-11（graphics table t0-t10 连续 11 个全 SRV，避免读到 UAV 描述符）：
//     0=height 1=normal 2=surface 3=meteorograph 4=grassBC 5=grassN 6=grassAoRDp
//     7=slopeBC 8=slopeN 9=slopeAoRDp 10=ambient 11=pluviometer（TerrainData_CS 采样）
//   UAV 集中 12-19（CS 单独 dispatch）：
//     12=height 13=normal 14=surface 15=meteorograph 16=pluviometer
//     17=waterFlow 18=waterSediment 19=waterVelocity
namespace {
constexpr UINT kSrvHeight = 0;
constexpr UINT kSrvNormal = 1;
constexpr UINT kSrvSurface = 2;
constexpr UINT kSrvMeteorograph = 3;
constexpr UINT kSrvGrassAlbedo = 4;
constexpr UINT kSrvGrassNormal = 5;
constexpr UINT kSrvGrassProperty = 6;
constexpr UINT kSrvSlopeAlbedo = 7;
constexpr UINT kSrvSlopeNormal = 8;
constexpr UINT kSrvSlopeProperty = 9;
constexpr UINT kSrvAmbient = 10;
constexpr UINT kSrvPluviometer = 11;
constexpr UINT kUavHeight = 12;
constexpr UINT kUavNormal = 13;
constexpr UINT kUavSurface = 14;
constexpr UINT kUavMeteorograph = 15;
constexpr UINT kUavPluviometer = 16;
constexpr UINT kUavWaterFlow = 17;
constexpr UINT kUavWaterSediment = 18;
constexpr UINT kUavWaterVelocity = 19;
constexpr UINT kDescriptorCount = 20;
} // namespace

TerrainRenderer::TerrainRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	const TexturePaths& paths,
	UINT dataSideElements, DXGI_FORMAT rtvFormat)
	: _context(context), _dataSide(dataSideElements), _rtvFormat(rtvFormat) {
	createDataTextures();
	createSurfaceTextures(paths);
	createRootSignature();
	createPipelineState(vsBytes, psBytes, rtvFormat);
	createCbv();
}

void TerrainRenderer::createDataTextures() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create terrain descriptor heap");

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

	// height: R32G32_FLOAT (x terrainHeight, y waterHeight) — 参考 TerrainData.mat textureMode 7 = RG32Float
	D3D12_RESOURCE_DESC hDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32_FLOAT, _dataSide, _dataSide, 1, 1);
	hDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &hDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_heightTex)),
		"Failed to create terrain height texture");

	// normal: R8G8B8A8_SNORM — 参考 TerrainData.mat textureMode 3 = RGBA8Snorm（.rg terrain normal, .ba water normal）
	D3D12_RESOURCE_DESC nDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_SNORM, _dataSide, _dataSide, 1, 1);
	nDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &nDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_normalTex)),
		"Failed to create terrain normal texture");

	// surface: R8G8B8A8_UNORM
	D3D12_RESOURCE_DESC sDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, _dataSide, _dataSide, 1, 1);
	sDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &sDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_surfaceTex)),
		"Failed to create terrain surface texture");

	// meteorograph: R32G32B32A32_FLOAT (wind/humidity/temperature), 128x128 — 参考 Meteorograph.mat textureMode 9 = RGBA32Float
	const UINT metSide = 128;
	D3D12_RESOURCE_DESC mDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, metSide, metSide, 1, 1);
	mDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &mDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_meteorographTex)),
		"Failed to create meteorograph texture");

	// pluviometer: R16G16_FLOAT (rainfall), 128x128 — 参考 Meteorograph.mat textureMode 4 = RG16Float
	D3D12_RESOURCE_DESC pDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT, metSide, metSide, 1, 1);
	pDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &pDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_pluviometerTex)),
		"Failed to create pluviometer texture");

	// 水文数据纹理（参考 TerrainData.mat textureMode）：
	//   WaterFlow textureMode 5 = RGBA16Float、WaterSediment textureMode 6 = R32Float、WaterVelocity textureMode 4 = RG16Float。
	const auto makeWaterTex = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& out, const char* name, DXGI_FORMAT fmt) {
		D3D12_RESOURCE_DESC wDesc = CD3DX12_RESOURCE_DESC::Tex2D(fmt, _dataSide, _dataSide, 1, 1);
		wDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHECK_OK(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &wDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)),
			std::string("Failed to create terrain water texture: ") + name);
	};
	makeWaterTex(_waterFlowTex, "waterFlow", DXGI_FORMAT_R16G16B16A16_FLOAT);
	makeWaterTex(_waterSedimentTex, "waterSediment", DXGI_FORMAT_R32_FLOAT);
	makeWaterTex(_waterVelocityTex, "waterVelocity", DXGI_FORMAT_R16G16_FLOAT);

	// SRV/UAV 描述符。
	struct TexInfo { ID3D12Resource* res; DXGI_FORMAT fmt; UINT srvIdx; UINT uavIdx; bool hasSrv; bool hasUav; };
	const TexInfo infos[] = {
		{ _heightTex.Get(), DXGI_FORMAT_R32G32_FLOAT, kSrvHeight, kUavHeight, true, true },
		{ _normalTex.Get(), DXGI_FORMAT_R8G8B8A8_SNORM, kSrvNormal, kUavNormal, true, true },
		{ _surfaceTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvSurface, kUavSurface, true, true },
		{ _meteorographTex.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, kSrvMeteorograph, kUavMeteorograph, true, true },
		{ _pluviometerTex.Get(), DXGI_FORMAT_R16G16_FLOAT, kSrvPluviometer, kUavPluviometer, true, true },
		{ _waterFlowTex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 0, kUavWaterFlow, false, true }, // 仅 UAV（RWTexture2D 读+写）
		{ _waterSedimentTex.Get(), DXGI_FORMAT_R32_FLOAT, 0, kUavWaterSediment, false, true },
		{ _waterVelocityTex.Get(), DXGI_FORMAT_R16G16_FLOAT, 0, kUavWaterVelocity, false, true },
	};
	// 先建所有 SRV（写堆 0-11 区间），再建所有 UAV（写堆 12-19 区间）。
	// graphics SRV table t0-t10 连续 11 个全在 SRV 区，避免读到 UAV 描述符触发 access violation。
	for (const auto& info : infos) {
		if (!info.hasSrv) {
			continue;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = _heap->GetCPUDescriptorHandleForHeapStart();
		srvCpu.ptr += info.srvIdx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = info.fmt;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(info.res, &srv, srvCpu);
	}
	for (const auto& info : infos) {
		if (!info.hasUav) {
			continue;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = _heap->GetCPUDescriptorHandleForHeapStart();
		uavCpu.ptr += info.uavIdx * inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = info.fmt;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(info.res, nullptr, &uav, uavCpu);
	}
	_heightUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_heightUavGpu.ptr += kUavHeight * inc;
	_normalUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_normalUavGpu.ptr += kUavNormal * inc;
	_surfaceUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_surfaceUavGpu.ptr += kUavSurface * inc;
	_meteorographUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_meteorographUavGpu.ptr += kUavMeteorograph * inc;
	_pluviometerUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_pluviometerUavGpu.ptr += kUavPluviometer * inc;
	_waterFlowUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_waterFlowUavGpu.ptr += kUavWaterFlow * inc;
	_waterSedimentUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_waterSedimentUavGpu.ptr += kUavWaterSediment * inc;
	_waterVelocityUavGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_waterVelocityUavGpu.ptr += kUavWaterVelocity * inc;
	_meteorographSrvGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_meteorographSrvGpu.ptr += kSrvMeteorograph * inc;
	_pluviometerSrvGpu = _heap->GetGPUDescriptorHandleForHeapStart();
	_pluviometerSrvGpu.ptr += kSrvPluviometer * inc;

	// 采样器：s0 terrain (ANISOTROPIC/WRAP)，s1 ambient (LINEAR/CLAMP)。
	D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
	samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerHeapDesc.NumDescriptors = 2;
	samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create terrain sampler heap");
	D3D12_CPU_DESCRIPTOR_HANDLE s0 = _samplerHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE s1 = s0;
	s1.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	D3D12_SAMPLER_DESC terrSamp{};
	terrSamp.Filter = D3D12_FILTER_ANISOTROPIC;
	terrSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	terrSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	terrSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	terrSamp.MipLODBias = 0.0f; // 严格照搬参考：AfterglowSampler::initCreateInfo 无 mipLodBias（=0）
	terrSamp.MaxAnisotropy = 16; // 参考用设备最大 maxAnisotropy（通常=16，等价）
	terrSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	terrSamp.MinLOD = 0.0f;
	terrSamp.MaxLOD = FLT_MAX; // 参考每个纹理 sampler maxLod=mipLevels；SampleLevel 内部 clamp 等效
	device->CreateSampler(&terrSamp, s0);

	D3D12_SAMPLER_DESC ambSamp{};
	ambSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	ambSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	ambSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	ambSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	ambSamp.MaxAnisotropy = 1;
	ambSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	ambSamp.MinLOD = 0.0f;
	ambSamp.MaxLOD = FLT_MAX;
	device->CreateSampler(&ambSamp, s1);
}

void TerrainRenderer::createSurfaceTextures(const TexturePaths& paths) {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const auto writeSrv = [&](Texture& tex, UINT srvIdx, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += srvIdx * inc;
		tex.createSrv(device, cpu);
	};

	_grassAlbedo.load(_context, paths.grassAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	writeSrv(_grassAlbedo, kSrvGrassAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	_grassNormal.load(_context, paths.grassNormal, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeSrv(_grassNormal, kSrvGrassNormal, DXGI_FORMAT_R8G8B8A8_UNORM);
	_grassProperty.load(_context, paths.grassProperty, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeSrv(_grassProperty, kSrvGrassProperty, DXGI_FORMAT_R8G8B8A8_UNORM);

	_slopeAlbedo.load(_context, paths.slopeAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	writeSrv(_slopeAlbedo, kSrvSlopeAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	_slopeNormal.load(_context, paths.slopeNormal, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeSrv(_slopeNormal, kSrvSlopeNormal, DXGI_FORMAT_R8G8B8A8_UNORM);
	_slopeProperty.load(_context, paths.slopeProperty, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeSrv(_slopeProperty, kSrvSlopeProperty, DXGI_FORMAT_R8G8B8A8_UNORM);

	if (!paths.ambient.empty()) {
		_ambient.load(_context, paths.ambient, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
		writeSrv(_ambient, kSrvAmbient, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	}
}

void TerrainRenderer::createRootSignature() {
	// t0-t10 连续 SRV 表（对应描述符堆 index 0..10，全为 SRV）。
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 11; // t0 height..t10 ambient
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 2; // s0 terrain, s1 ambient
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
		throw std::runtime_error(std::string("Failed to serialize terrain root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create terrain root signature");
}

void TerrainRenderer::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat) {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _rootSignature.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.PS = { ps.data(), ps.size() };

	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	// 参考 TerrainMesh.mat cullMode:2 = Back（背面剔除）。网格用同一 GridIndexPack 绕序 + rotate(-90X)
	// 保持绕序（det=+1），Back 剔除下正面仍可见。
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	desc.RasterizerState.FrontCounterClockwise = TRUE;
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // LESS + write
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
		"Failed to create terrain pipeline state");
}

void TerrainRenderer::createCbv() {
	auto* device = _context.device();
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_cbv)),
		"Failed to create terrain CBV buffer");
}

void TerrainRenderer::render(const glm::mat4& viewProj, const glm::vec3& cameraPos,
	const Mesh& mesh, const glm::mat4& model,
	const glm::vec3& dirLightDir, const glm::vec4& dirLightColor) {
	auto* cl = _context.commandList();
	const UINT w = _context.width();
	const UINT h = _context.height();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);

	ID3D12DescriptorHeap* const heaps[] = { _heap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	struct TerrainCB {
		glm::mat4 viewProj;
		glm::mat4 model;
		glm::vec4 cameraPos;
		glm::vec4 dirLightDirection;
		glm::vec4 dirLightColor;
	};
	TerrainCB* cb = nullptr;
	_cbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
	cb->viewProj = viewProj;
	cb->model = model;
	cb->cameraPos = glm::vec4(cameraPos, 1.0f);
	cb->dirLightDirection = glm::vec4(glm::normalize(dirLightDir), 0.0f);
	cb->dirLightColor = dirLightColor;
	_cbv->Unmap(0, nullptr);

	cl->SetGraphicsRootConstantBufferView(0, _cbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, _heap->GetGPUDescriptorHandleForHeapStart());
	cl->SetGraphicsRootDescriptorTable(2, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	const D3D12_VERTEX_BUFFER_VIEW vbView = mesh.vbView();
	const D3D12_INDEX_BUFFER_VIEW ibView = mesh.ibView();
	cl->IASetVertexBuffers(0, 1, &vbView);
	cl->IASetIndexBuffer(&ibView);
	cl->DrawIndexedInstanced(mesh.indexCount(), 1, 0, 0, 0);
}
