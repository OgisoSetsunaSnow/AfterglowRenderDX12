#include "CharacterRenderer.h"
#include "Mesh.h"

#include <d3dx12.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)
#define CHECK_OK_HR(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(std::string(msg) + " (HRESULT 0x" + [&]{ char b[16]; std::snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }() + ")"); } while (0)

namespace {

// GlobalUniform cbuffer 布局（Shaders/CharacterCommon.hlsl b1，port from ubo::GlobalUniform）。
struct GlobalUniformCB {
	glm::mat4 view;
	glm::mat4 projection;
	glm::mat4 viewProjection;
	glm::mat4 invView;
	glm::mat4 invProjection;
	glm::mat4 invViewProjection;
	glm::vec4 dirLightDirection;
	glm::vec4 dirLightColor;
	glm::vec4 cameraPositionNear; // xyz + near
	glm::vec4 cameraVectorFar;    // xyz + far
	glm::vec2 screenResolution;
	glm::vec2 invScreenResolution;
	glm::vec2 cursorPosition;
	float time;
	float deltaTime;
	glm::vec4 frustumPlaneL, frustumPlaneR, frustumPlaneB, frustumPlaneT, frustumPlaneN, frustumPlaneF;
	float cameraFov;
	float screenAspectRatio;
	float invScreenAspectRatio;
	float frameIndex;
};
static_assert(sizeof(GlobalUniformCB) <= 768, "GlobalUniformCB too large");

// MeshUniform cbuffer 布局（Shaders/CharacterCommon.hlsl b0）。
struct MeshUniformCB {
	glm::mat4 model;
	glm::mat4 invTransModel;
	glm::vec3 minAABB;
	float objectID;
	glm::vec3 maxAABB;
	float indexCount;
	glm::vec4 materialParams;
	glm::vec4 materialParams2;
	glm::vec4 materialParams3;
	glm::vec4 outlineColor;
	glm::vec4 objectForward;
	glm::vec4 objectRight;
	glm::vec4 objectUp;
	glm::vec4 outlineParams;
};
static_assert(sizeof(MeshUniformCB) <= 512, "MeshUniformCB too large");

// 输入布局：pos/uv/normal/color/tangent（MeshVertex）。
std::array<D3D12_INPUT_ELEMENT_DESC, 5> inputLayout() {
	return { {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	} };
}

} // namespace

CharacterRenderer::CharacterRenderer(D3D12Context& context,
	const std::vector<std::vector<std::uint8_t>>& materialVsBytes,
	const std::vector<std::vector<std::uint8_t>>& materialFsBytes,
	const std::vector<CharacterMaterialDesc>& materialDescs,
	const std::vector<std::vector<std::uint8_t>>& outlineVsBytes,
	const std::vector<std::vector<std::uint8_t>>& outlineFsBytes,
	DXGI_FORMAT rtvFormat,
	const std::vector<Texture*>& materialTextures,
	const Texture& ambientTex)
	: _context(context), _rtvFormat(rtvFormat) {
	createRootSignature();
	createPipelines(materialVsBytes, materialFsBytes, materialDescs, outlineVsBytes, outlineFsBytes);
	createTextureHeap(materialTextures, ambientTex);
	createCbvs();
	createSamplers();
}

CharacterRenderer::~CharacterRenderer() = default;

void CharacterRenderer::createRootSignature() {
	// b0 MeshUniform + b1 GlobalUniform（root CBV，ALL）。
	D3D12_ROOT_PARAMETER params[4]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0 MeshUniform
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1; // b1 GlobalUniform
	params[1].Descriptor.RegisterSpace = 0;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// t0-t5 SRV 表（5 材质纹理 + ambient）。
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 6;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &srvRange;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// s0-s5 采样器表。
	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 6;
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &samplerRange;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 4;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize character root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create character root signature");
}

void CharacterRenderer::createPipelines(const std::vector<std::vector<std::uint8_t>>& materialVsBytes,
	const std::vector<std::vector<std::uint8_t>>& materialFsBytes,
	const std::vector<CharacterMaterialDesc>& materialDescs,
	const std::vector<std::vector<std::uint8_t>>& outlineVsBytes,
	const std::vector<std::vector<std::uint8_t>>& outlineFsBytes) {
	const auto il = inputLayout();

	const auto createPSO = [&](const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& fsBytes, D3D12_CULL_MODE cullMode,
		bool depthEnabled, D3D12_COMPARISON_FUNC depthFunc, D3D12_DEPTH_WRITE_MASK depthWrite,
		bool blendEnable) -> Microsoft::WRL::ComPtr<ID3D12PipelineState> {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = _rootSignature.Get();
		desc.VS = { vsBytes.data(), vsBytes.size() };
		desc.PS = { fsBytes.data(), fsBytes.size() };
		desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		if (blendEnable) {
			desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
			desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
			desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
			desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
			desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		}
		desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = cullMode;
		desc.RasterizerState.FrontCounterClockwise = TRUE;
		desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		desc.DepthStencilState.DepthEnable = depthEnabled;
		desc.DepthStencilState.DepthFunc = depthFunc;
		desc.DepthStencilState.DepthWriteMask = depthWrite;
		desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		desc.SampleMask = UINT_MAX;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = _rtvFormat;
		desc.SampleDesc.Count = 1;
		desc.InputLayout = { il.data(), static_cast<UINT>(il.size()) };

		Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
		HRESULT hr = _context.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		CHECK_OK_HR(hr, "Failed to create character pipeline state");
		return pso;
	};

	// 材质主 PSO：每材质独立配置（参考项目 .mat cullMode/domain/depthWrite）。
	for (std::size_t i = 0; i < materialFsBytes.size(); ++i) {
		const CharacterMaterialDesc& d = materialDescs[i];
		try {
			_psos.push_back(createPSO(materialVsBytes[i], materialFsBytes[i], d.cullMode, true,
				D3D12_COMPARISON_FUNC_LESS,
				d.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO,
				d.blend));
		} catch (const std::runtime_error& e) {
			throw std::runtime_error(std::string(e.what()) + " [material " + std::to_string(i) + "]");
		}
	}

	// 描边 PSO：参考项目 cullMode:1=Front（ArcOutline/EndfieldOutline.mat），深度 LESS+write。
	_outlinePsos[0] = createPSO(outlineVsBytes[0], outlineFsBytes[0], D3D12_CULL_MODE_FRONT, true,
		D3D12_COMPARISON_FUNC_LESS, D3D12_DEPTH_WRITE_MASK_ALL, false);
	_outlinePsos[1] = createPSO(outlineVsBytes[1], outlineFsBytes[1], D3D12_CULL_MODE_FRONT, true,
		D3D12_COMPARISON_FUNC_LESS, D3D12_DEPTH_WRITE_MASK_ALL, false);
}

void CharacterRenderer::createTextureHeap(const std::vector<Texture*>& materialTextures, const Texture& ambientTex) {
	auto* device = _context.device();
	_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// 每材质 6 槽 = [5 材质纹理, ambient]。材质数 = materialTextures.size()/5。
	const UINT numMaterials = static_cast<UINT>(materialTextures.size()) / 5;
	const UINT numDescriptors = numMaterials * 6;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = numDescriptors;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_textureHeap)),
		"Failed to create character texture heap");

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = _textureHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT m = 0; m < numMaterials; ++m) {
		for (UINT slot = 0; slot < 5; ++slot) {
			const Texture* tex = materialTextures[m * 5 + slot];
			if (!tex || !tex->valid()) {
				// 缺纹理用组内 albedo（slot0）占位，避免 null SRV。
				tex = materialTextures[m * 5];
			}
			tex->createSrv(device, cpu);
			cpu.ptr += _srvDescriptorSize;
		}
		// t5 = ambient（每材质组第 6 槽写 DefaultSky）。
		ambientTex.createSrv(device, cpu);
		cpu.ptr += _srvDescriptorSize;
	}
}

void CharacterRenderer::createCbvs() {
	auto* device = _context.device();
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);

	// 每物体 MeshUniform：预留足够空间（512*64 物体上限）。
	const CD3DX12_RESOURCE_DESC meshDesc = CD3DX12_RESOURCE_DESC::Buffer(512 * 64);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &meshDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_meshUniformCbv)),
		"Failed to create character mesh uniform CBV");

	const CD3DX12_RESOURCE_DESC globalDesc = CD3DX12_RESOURCE_DESC::Buffer(1024);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &globalDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_globalUniformCbv)),
		"Failed to create character global uniform CBV");
}

void CharacterRenderer::createSamplers() {
	auto* device = _context.device();
	_samplerDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heapDesc.NumDescriptors = 6;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create character sampler heap");

	D3D12_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.MaxAnisotropy = 16;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.MinLOD = 0.0f;
	samplerDesc.MaxLOD = FLT_MAX;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = _samplerHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < 6; ++i) {
		device->CreateSampler(&samplerDesc, cpu);
		cpu.ptr += _samplerDescriptorSize;
	}
}

void CharacterRenderer::setLight(const glm::vec3& dir, const glm::vec3& color, float intensity) {
	_lightDir = dir;
	_lightColor = color;
	_lightIntensity = intensity;
}

UINT CharacterRenderer::writeMeshUniform(char* dst, const CharacterDrawItem& item, UINT index,
	const glm::mat4& viewProj) {
	MeshUniformCB* cb = reinterpret_cast<MeshUniformCB*>(dst + index * CbvStride);
	std::memset(cb, 0, sizeof(MeshUniformCB));
	cb->model = item.model;
	cb->invTransModel = glm::transpose(glm::inverse(item.model));
	// 参考项目 MeshUniform.minAABB/maxAABB（EndfieldBody_VS 用 normalized position）。
	if (item.mesh) {
		cb->minAABB = item.mesh->aabbMin();
		cb->maxAABB = item.mesh->aabbMax();
	} else {
		cb->minAABB = glm::vec3(-1.0f);
		cb->maxAABB = glm::vec3(1.0f);
	}
	cb->objectID = static_cast<float>(index);
	cb->indexCount = item.mesh ? static_cast<float>(item.mesh->indexCount()) : 0.0f;
	cb->materialParams = item.params.materialParams;
	cb->materialParams2 = item.params.materialParams2;
	cb->materialParams3 = item.params.materialParams3;
	cb->outlineColor = item.params.outlineColor;
	cb->objectForward = glm::vec4(glm::normalize(item.objectForward), 0.0f);
	cb->objectRight = glm::vec4(glm::normalize(item.objectRight), 0.0f);
	cb->objectUp = glm::vec4(glm::normalize(item.objectUp), 0.0f);
	cb->outlineParams = item.params.outlineParams;
	return index * CbvStride;
}

void CharacterRenderer::render(const glm::mat4& view, const glm::mat4& proj,
	const std::vector<CharacterDrawItem>& items,
	const D3D12_CPU_DESCRIPTOR_HANDLE& rtv, const D3D12_CPU_DESCRIPTOR_HANDLE& dsv) {
	if (items.empty()) {
		return;
	}
	auto* cl = _context.commandList();
	const UINT w = _context.width();
	const UINT h = _context.height();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

	ID3D12DescriptorHeap* const heaps[] = { _textureHeap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->SetGraphicsRootDescriptorTable(3, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	// GlobalUniform（b1）。
	GlobalUniformCB* g = nullptr;
	_globalUniformCbv->Map(0, nullptr, reinterpret_cast<void**>(&g));
	g->view = view;
	g->projection = proj;
	g->viewProjection = proj * view;
	g->invView = glm::inverse(view);
	g->invProjection = glm::inverse(proj);
	g->invViewProjection = glm::inverse(proj * view);
	g->dirLightDirection = glm::vec4(glm::normalize(_lightDir), 0.0f);
	g->dirLightColor = glm::vec4(_lightColor, _lightIntensity);
	const glm::mat4 invView = glm::inverse(view);
	const glm::vec3 cameraPos(invView[3]);
	g->cameraPositionNear = glm::vec4(cameraPos, 0.1f);
	g->cameraVectorFar = glm::vec4(0.0f, 0.0f, -1.0f, 1000.0f); // cameraVector（近似 + far）
	g->screenResolution = glm::vec2(static_cast<float>(w), static_cast<float>(h));
	g->invScreenResolution = glm::vec2(1.0f) / g->screenResolution;
	g->cursorPosition = glm::vec2(0.0f);
	g->time = 0.0f;
	g->deltaTime = 0.0f;
	g->frustumPlaneL = glm::vec4(0.0f);
	g->frustumPlaneR = glm::vec4(0.0f);
	g->frustumPlaneB = glm::vec4(0.0f);
	g->frustumPlaneT = glm::vec4(0.0f);
	g->frustumPlaneN = glm::vec4(0.0f);
	g->frustumPlaneF = glm::vec4(0.0f);
	g->cameraFov = 0.785398f; // 45°
	g->screenAspectRatio = static_cast<float>(w) / static_cast<float>(h);
	g->invScreenAspectRatio = static_cast<float>(h) / static_cast<float>(w);
	g->frameIndex = 0.0f;
	_globalUniformCbv->Unmap(0, nullptr);
	cl->SetGraphicsRootConstantBufferView(1, _globalUniformCbv->GetGPUVirtualAddress());

	// 写所有物体 MeshUniform。
	MeshUniformCB* mapped = nullptr;
	_meshUniformCbv->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
	const D3D12_GPU_VIRTUAL_ADDRESS cbvBase = _meshUniformCbv->GetGPUVirtualAddress();
	for (std::size_t i = 0; i < items.size(); ++i) {
		writeMeshUniform(reinterpret_cast<char*>(mapped), items[i], static_cast<UINT>(i), proj * view);
	}
	_meshUniformCbv->Unmap(0, nullptr);

	const D3D12_GPU_DESCRIPTOR_HANDLE texTableBase = _textureHeap->GetGPUDescriptorHandleForHeapStart();

	const auto drawMesh = [&](const CharacterDrawItem& item, UINT index, ID3D12PipelineState* pso,
		UINT group) {
		cl->SetPipelineState(pso);
		const D3D12_GPU_DESCRIPTOR_HANDLE table{
			texTableBase.ptr + static_cast<SIZE_T>(group * 6) * _srvDescriptorSize };
		cl->SetGraphicsRootDescriptorTable(2, table);
		cl->SetGraphicsRootConstantBufferView(0, cbvBase + index * CbvStride);
		const D3D12_VERTEX_BUFFER_VIEW vbView = item.mesh->vbView();
		const D3D12_INDEX_BUFFER_VIEW ibView = item.mesh->ibView();
		cl->IASetVertexBuffers(0, 1, &vbView);
		cl->IASetIndexBuffer(&ibView);
		cl->DrawIndexedInstanced(item.mesh->indexCount(), 1, 0, 0, 0);
	};

	// Outline pass 先画（参考项目 drawIndex=1 的 outline 材质；BattleMage=ArcOutline，Yvonne=EndfieldOutline）。
	for (std::size_t i = 0; i < items.size(); ++i) {
		const auto& item = items[i];
		if (!item.outline || !item.mesh || !item.mesh->valid()) {
			continue;
		}
		// ArcToon(BattleMage) 用 ArcOutline(0)；Endfield*（Yvonne）用 EndfieldOutline(1）。
		const int outlineIdx = (item.material == CharacterMaterial::ArcToon) ? 0 : 1;
		// 描边纹理组：默认沿用材质组；EndfieldOutline 采样 t3=stTex（布料用白 st 组）。
		const UINT outlineGroup = (item.outlineTextureGroup != UINT_MAX)
			? item.outlineTextureGroup : item.textureGroup;
		drawMesh(item, static_cast<UINT>(i), _outlinePsos[outlineIdx].Get(), outlineGroup);
	}

	// Main pass。
	for (std::size_t i = 0; i < items.size(); ++i) {
		const auto& item = items[i];
		if (!item.mesh || !item.mesh->valid()) {
			continue;
		}
		const int matIdx = static_cast<int>(item.material);
		drawMesh(item, static_cast<UINT>(i), _psos[matIdx].Get(), item.textureGroup);
	}
}
