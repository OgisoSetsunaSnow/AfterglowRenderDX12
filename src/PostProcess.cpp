#include "PostProcess.h"

#include <d3dx12.h>

#include <cfloat>
#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

PostProcess::PostProcess(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes)
	: _context(context) {
	createRootSignature();
	createPipelineState(vsBytes, psBytes);
	createParamCbv();

	// 采样器：s0 scene（线性 CLAMP）、s1 ambient（线性 CLAMP）、s2 terrain（ANISO16 WRAP）、s3 ColorLUT（线性 CLAMP）。
	auto* device = context.device();
	D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
	samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerHeapDesc.NumDescriptors = 4;
	samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create post-process sampler heap");

	const UINT sInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	D3D12_CPU_DESCRIPTOR_HANDLE s0 = _samplerHeap->GetCPUDescriptorHandleForHeapStart();

	D3D12_SAMPLER_DESC sceneSamp{};
	sceneSamp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	sceneSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sceneSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sceneSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sceneSamp.MaxAnisotropy = 1;
	sceneSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sceneSamp.MinLOD = 0.0f;
	sceneSamp.MaxLOD = FLT_MAX;
	device->CreateSampler(&sceneSamp, s0);

	D3D12_SAMPLER_DESC ambSamp = sceneSamp; // ambient equirect（SampleLevel），线性 CLAMP。
	device->CreateSampler(&ambSamp, CD3DX12_CPU_DESCRIPTOR_HANDLE(s0, 1, sInc));

	D3D12_SAMPLER_DESC terrSamp{};
	terrSamp.Filter = D3D12_FILTER_ANISOTROPIC;
	terrSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	terrSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	terrSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	terrSamp.MaxAnisotropy = 16;
	terrSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	terrSamp.MinLOD = 0.0f;
	terrSamp.MaxLOD = FLT_MAX;
	device->CreateSampler(&terrSamp, CD3DX12_CPU_DESCRIPTOR_HANDLE(s0, 2, sInc));

	D3D12_SAMPLER_DESC lutSamp = sceneSamp; // 3D LUT（trilinear Sample），线性 CLAMP。
	device->CreateSampler(&lutSamp, CD3DX12_CPU_DESCRIPTOR_HANDLE(s0, 3, sInc));
}

void PostProcess::createRootSignature() {
	// t0 sceneColor + t1 bloomCombined + t2 depth + t3 noise + t4 ambient + t5 TerrainHeight + t6 ColorLUT（共享 SRV 堆连续）。
	// 关键：单个 range NumDescriptors=7（t0-t6 连续），表 base 指向共享堆 index 0-6。
	D3D12_DESCRIPTOR_RANGE srvRanges[1]{};
	srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRanges[0].NumDescriptors = 7; // t0-t6
	srvRanges[0].BaseShaderRegister = 0;
	srvRanges[0].RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 4; // s0 scene, s1 ambient, s2 terrain, s3 ColorLUT
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[3]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0：相机/雾参数
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = srvRanges;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &samplerRange;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = 3;
	rsDesc.pParameters = params;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize post-process root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create post-process root signature");
}

void PostProcess::createPipelineState(const std::vector<std::uint8_t>& vs,
	const std::vector<std::uint8_t>& ps) {
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
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // swapchain 后台缓冲格式
	desc.SampleDesc.Count = 1;

	// 全屏三角形：无顶点缓冲（VS 用 SV_VertexID 生成），InputLayout 为空。
	CHECK_OK(_context.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pso)),
		"Failed to create post-process pipeline state");
}

void PostProcess::createParamCbv() {
	auto* device = _context.device();
	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	CHECK_OK(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_paramCbv)),
		"Failed to create post-process param CBV");
}

void PostProcess::render(ID3D12DescriptorHeap* srvHeap,
	D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE bloomSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE noiseSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE ambientSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE terrainHeightSrv,
	const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	const glm::vec3& cameraPos, float nearZ, float farZ, float time,
	const glm::mat4& view, const glm::mat4& invViewProjection,
	const glm::vec3& lightDir,
	const glm::vec2& cursorPos,
	std::uint32_t effects) {
	auto* cl = _context.commandList();
	const UINT w = _context.width();
	const UINT h = _context.height();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	ID3D12DescriptorHeap* const heaps[] = { srvHeap, _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	struct PostParams {
		glm::vec4 cameraPosition;
		glm::vec4 envColor;        // 保留（参考从 ambientTexture 采样 envColor，不再用此字段）
		glm::mat4 view;
		glm::mat4 invViewProjection;
		glm::vec4 dirLightDirection;
		glm::vec4 params;          // nearZ, farZ, fogDensity, time
		glm::vec4 screenParams;    // invScreenWidth, invScreenHeight, cursorX, cursorY
		glm::vec2 aspectRatio;     // 宽高比, 1.0
		std::uint32_t effects;     // bit3 = noise 调试
		std::uint32_t pad;
	};
	PostParams* params = nullptr;
	_paramCbv->Map(0, nullptr, reinterpret_cast<void**>(&params));
	params->cameraPosition = glm::vec4(cameraPos, 1.0f);
	params->envColor = glm::vec4(0.7f, 0.75f, 0.85f, 1.0f); // 保留占位（参考从 ambientTexture 采样）
	params->view = view;
	params->invViewProjection = invViewProjection;
	params->dirLightDirection = glm::vec4(glm::normalize(lightDir), 0.0f);
	params->params = glm::vec4(nearZ, farZ, 0.08f, time);
	params->screenParams = glm::vec4(1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h),
		cursorPos.x, cursorPos.y);
	params->aspectRatio = glm::vec2(static_cast<float>(w) / static_cast<float>(h), 1.0f);
	params->effects = effects;
	params->pad = 0;
	_paramCbv->Unmap(0, nullptr);
	cl->SetGraphicsRootConstantBufferView(0, _paramCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
	cl->SetGraphicsRootDescriptorTable(2, _samplerHeap->GetGPUDescriptorHandleForHeapStart());
	cl->DrawInstanced(3, 1, 0, 0);
}
