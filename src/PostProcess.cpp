#include "PostProcess.h"

#include <d3dx12.h>

#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

PostProcess::PostProcess(D3D12Context& context,
	const std::vector<std::uint8_t>& vsBytes,
	const std::vector<std::uint8_t>& psBytes)
	: _context(context) {
	createRootSignature();
	createPipelineState(vsBytes, psBytes);
	createParamCbv();

	// 采样器（线性 + CLAMP）。
	auto* device = context.device();
	D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
	samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerHeapDesc.NumDescriptors = 1;
	samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK_OK(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&_samplerHeap)),
		"Failed to create post-process sampler heap");

	D3D12_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.MinLOD = 0.0f;
	samplerDesc.MaxLOD = 0.0f;
	device->CreateSampler(&samplerDesc, _samplerHeap->GetCPUDescriptorHandleForHeapStart());
}

void PostProcess::createRootSignature() {
	// t0 sceneColor + t1 bloomCombined + t2 depth（都在共享 SRV 堆内）。
	D3D12_DESCRIPTOR_RANGE srvRanges[3]{};
	srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRanges[0].NumDescriptors = 1;
	srvRanges[0].BaseShaderRegister = 0;
	srvRanges[0].RegisterSpace = 0;

	srvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRanges[1].NumDescriptors = 1;
	srvRanges[1].BaseShaderRegister = 1;
	srvRanges[1].RegisterSpace = 0;

	srvRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRanges[2].NumDescriptors = 1;
	srvRanges[2].BaseShaderRegister = 2;
	srvRanges[2].RegisterSpace = 0;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 1;
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;

	D3D12_ROOT_PARAMETER params[3]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0; // b0：相机/雾参数
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 3;
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
	const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
	const glm::vec3& cameraPos, float nearZ, float farZ, float time,
	const glm::mat4& view, const glm::vec3& lightDir,
	std::uint32_t effects) {
	auto* cl = _context.commandList();
	const UINT w = _context.width();
	const UINT h = _context.height();
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
	cl->RSSetViewports(1, &viewport);
	cl->RSSetScissorRects(1, &scissor);
	cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	// sceneSrv/bloomSrv/depthSrv 都在同一个 srvHeap（D3D12 限制：一个 CBV_SRV_UAV 堆）。
	ID3D12DescriptorHeap* const heaps[] = { srvHeap, _samplerHeap.Get() };
	cl->SetDescriptorHeaps(2, heaps);
	// 必须先 SetGraphicsRootSignature，再 Set 各根参数（否则用上一个 PSO 的根签名）。
	cl->SetGraphicsRootSignature(_rootSignature.Get());
	cl->SetPipelineState(_pso.Get());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// b0：相机/雾/光轴参数。
	struct PostParams {
		glm::vec4 cameraPosition;
		glm::vec4 fogColor;
		glm::mat4 view;          // 光轴：方向光变换到相机空间
		glm::vec4 dirLightDirection; // 世界空间方向光（光轴用）
		float nearZ;
		float farZ;
		float fogDensity;
		float time;
		std::uint32_t effects;   // 位标志：bit0=色差 bit1=尘埃 bit2=暗角
		std::uint32_t pad;
		float invScreenWidth;
		float invScreenHeight;
		float pad2;
		float pad3;
	};
	PostParams* params = nullptr;
	_paramCbv->Map(0, nullptr, reinterpret_cast<void**>(&params));
	params->cameraPosition = glm::vec4(cameraPos, 1.0f);
	params->fogColor = glm::vec4(0.35f, 0.4f, 0.5f, 1.0f); // 雾色（冷灰蓝）
	params->view = view;
	params->dirLightDirection = glm::vec4(glm::normalize(lightDir), 0.0f);
	params->nearZ = nearZ;
	params->farZ = farZ;
	params->fogDensity = 0.08f;
	params->time = time;
	params->effects = effects;
	params->pad = 0;
	params->invScreenWidth = 1.0f / static_cast<float>(w);
	params->invScreenHeight = 1.0f / static_cast<float>(h);
	params->pad2 = 0.0f;
	params->pad3 = 0.0f;
	_paramCbv->Unmap(0, nullptr);
	cl->SetGraphicsRootConstantBufferView(0, _paramCbv->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
	cl->SetGraphicsRootDescriptorTable(2, _samplerHeap->GetGPUDescriptorHandleForHeapStart());
	cl->DrawInstanced(3, 1, 0, 0);
}
