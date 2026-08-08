#include "FurRenderer.h"
#include "Mesh.h"

#include <d3dx12.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <array>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

// 描述符堆布局（shader visible CBV_SRV_UAV，4 desc）：
//   0 = albedo SRV（FS t0）
//   1 = normal SRV（FS t1）
//   2 = FractalWorleyNoise SRV（FS t2, R32F）
//   3 = FractalNormal SRV（FS t3, R8G8B8A8_SNORM）
namespace {
constexpr UINT kSrvAlbedo = 0;
constexpr UINT kSrvNormal = 1;
constexpr UINT kSrvWorley = 2;
constexpr UINT kSrvFractalNormal = 3;
constexpr UINT kDescriptorCount = 4;
} // namespace

FurRenderer::FurRenderer(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes,
	ID3D12Resource* albedoTex, ID3D12Resource* normalTex,
	ID3D12Resource* fractalWorleyTex, ID3D12Resource* fractalNormalTex)
	: _context(context),
	_albedoTex(albedoTex), _normalTex(normalTex),
	_fractalWorleyTex(fractalWorleyTex), _fractalNormalTex(fractalNormalTex) {
	createDescriptors();
	createRootSignature();
	createPipelineState(vsBytes, psBytes);
	createSamplers();
}

void FurRenderer::createDescriptors() {
	auto* device = _context.device();
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap)),
		"Failed to create fur descriptor heap");

	const auto writeSrv = [&](UINT idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += idx * inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = fmt;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = -1;
		device->CreateShaderResourceView(res, &srv, cpu);
	};
	writeSrv(kSrvAlbedo, _albedoTex, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	writeSrv(kSrvNormal, _normalTex, DXGI_FORMAT_R8G8B8A8_UNORM);
	writeSrv(kSrvWorley, _fractalWorleyTex, DXGI_FORMAT_R32_FLOAT);
	writeSrv(kSrvFractalNormal, _fractalNormalTex, DXGI_FORMAT_R8G8B8A8_SNORM);

	// b0/b1 上传 CBV 缓冲。
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC cbvDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_meshCbv)),
		"Failed to create fur mesh CBV");
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_globalCbv)),
		"Failed to create fur global CBV");
}

void FurRenderer::createRootSignature() {
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = kDescriptorCount; // t0-t3
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = kDescriptorCount; // s0-s3
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[4]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0 FurUniform
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1; // b1 FurGlobalUniform
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &srvRange;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &samplerRange;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 4;
	rsDesc.pParameters = params;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize fur root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create fur root signature");
}

void FurRenderer::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps) {
	auto* device = _context.device();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _rootSignature.Get();
	desc.VS = { vs.data(), vs.size() };
	desc.PS = { ps.data(), ps.size() };
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; // 参考 ShellFur.mat cullMode 2
	desc.RasterizerState.FrontCounterClockwise = TRUE;
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // LESS + write
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
		"Failed to create fur graphics PSO");
}

void FurRenderer::createSamplers() {
	auto* device = _context.device();
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create fur sampler heap");
	for (UINT i = 0; i < kDescriptorCount; ++i) {
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

void FurRenderer::render(const glm::mat4& view, const glm::mat4& proj, const glm::mat4& model,
	const glm::vec3& cameraPos, const glm::vec3& dirLightDir, const glm::vec4& dirLightColor,
	const Mesh& mesh, const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	const D3D12_CPU_DESCRIPTOR_HANDLE& dsv) {
	auto* cl = _context.commandList();

	// b0 FurUniform：布局与 ShellFur_VS/FS 一致（model/invTransModel/viewProjection/objectID/instanceCount/shellInterval/_pad）。
	{
		struct FurUniformCB {
			glm::mat4 model;          // 0
			glm::mat4 invTransModel;  // 64
			glm::mat4 viewProjection; // 128
			float objectID;           // 192
			float instanceCount;      // 196
			float shellInterval;      // 200
			float pad;                // 204
		};
		static_assert(sizeof(FurUniformCB) == 208, "FurUniformCB layout");
		FurUniformCB* cb = nullptr;
		_meshCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->model = model;
		cb->invTransModel = glm::inverseTranspose(model);
		cb->viewProjection = proj * view;
		cb->objectID = 0.0f;
		cb->instanceCount = static_cast<float>(kInstanceCount);
		cb->shellInterval = kShellInterval;
		cb->pad = 0.0f;
		_meshCbv->Unmap(0, nullptr);
	}
	// b1 FurGlobalUniform：y-up 世界（Ankha 模型经 rotate(-90X) 后 y-up，FS 的 worldPosition 是 y-up）。
	{
		struct FurGlobalUniformCB {
			glm::vec3 cameraPosition; float gpad0;
			glm::vec3 dirLightDirection; float gpad1;
			glm::vec4 dirLightColor;
		};
		FurGlobalUniformCB* cb = nullptr;
		_globalCbv->Map(0, nullptr, reinterpret_cast<void**>(&cb));
		cb->cameraPosition = cameraPos;
		cb->dirLightDirection = glm::normalize(dirLightDir);
		cb->dirLightColor = dirLightColor;
		_globalCbv->Unmap(0, nullptr);
	}

	D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
		static_cast<float>(_context.width()), static_cast<float>(_context.height()), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(_context.width()), static_cast<LONG>(_context.height()) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

	ID3D12DescriptorHeap* const heaps[] = { _heap.Get(), _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->SetGraphicsRootConstantBufferView(0, _meshCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootConstantBufferView(1, _globalCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(2, _heap->GetGPUDescriptorHandleForHeapStart());
	cl->SetGraphicsRootDescriptorTable(3, _samplerHeap->GetGPUDescriptorHandleForHeapStart());

	const D3D12_VERTEX_BUFFER_VIEW vbView = mesh.vbView();
	const D3D12_INDEX_BUFFER_VIEW ibView = mesh.ibView();
	cl->IASetVertexBuffers(0, 1, &vbView);
	cl->IASetIndexBuffer(&ibView);
	cl->DrawIndexedInstanced(mesh.indexCount(), kInstanceCount, 0, 0, 0);
}
