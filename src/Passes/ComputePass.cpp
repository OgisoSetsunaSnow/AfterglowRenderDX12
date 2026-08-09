#include "ComputePass.h"

#include <d3dx12.h>

#include <stdexcept>

#define CHECK_OK(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

ComputePass::ComputePass(D3D12Context& context, const std::vector<std::uint8_t>& csBytes,
	UINT numSrvRanges, UINT numUavRanges, bool hasCbv, UINT numSamplerRanges)
	: _context(context), _numSrvRanges(numSrvRanges), _numUavRanges(numUavRanges),
	_hasCbv(hasCbv), _numSamplerRanges(numSamplerRanges) {
	createRootSignature();
	createPipelineState(csBytes);
}

void ComputePass::createRootSignature() {
	// SRV 描述符表（t0..tN-1，各 1 描述符）+ UAV 描述符表（u0..uM-1）
	// + Sampler 描述符表（s0..sK-1，各 1 描述符）+ 可选 CBV b0。
	const UINT numParams = _numSrvRanges + _numUavRanges + _numSamplerRanges + (_hasCbv ? 1 : 0);
	std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges(_numSrvRanges);
	std::vector<D3D12_DESCRIPTOR_RANGE> uavRanges(_numUavRanges);
	std::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges(_numSamplerRanges);
	std::vector<D3D12_ROOT_PARAMETER> params(numParams);

	UINT pi = 0;
	for (UINT i = 0; i < _numSrvRanges; ++i) {
		srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRanges[i].NumDescriptors = 1;
		srvRanges[i].BaseShaderRegister = i;
		srvRanges[i].OffsetInDescriptorsFromTableStart = 0; // 每个 root parameter 是独立表，offset 必须 0
		srvRanges[i].RegisterSpace = 0;
		params[pi].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[pi].DescriptorTable.NumDescriptorRanges = 1;
		params[pi].DescriptorTable.pDescriptorRanges = &srvRanges[i];
		params[pi].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		++pi;
	}
	for (UINT i = 0; i < _numUavRanges; ++i) {
		uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRanges[i].NumDescriptors = 1;
		uavRanges[i].BaseShaderRegister = i;
		uavRanges[i].OffsetInDescriptorsFromTableStart = 0;
		uavRanges[i].RegisterSpace = 0;
		params[pi].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[pi].DescriptorTable.NumDescriptorRanges = 1;
		params[pi].DescriptorTable.pDescriptorRanges = &uavRanges[i];
		params[pi].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		++pi;
	}
	for (UINT i = 0; i < _numSamplerRanges; ++i) {
		samplerRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		samplerRanges[i].NumDescriptors = 1;
		samplerRanges[i].BaseShaderRegister = i;
		samplerRanges[i].OffsetInDescriptorsFromTableStart = 0; // 独立表，offset 必须 0
		samplerRanges[i].RegisterSpace = 0;
		params[pi].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[pi].DescriptorTable.NumDescriptorRanges = 1;
		params[pi].DescriptorTable.pDescriptorRanges = &samplerRanges[i];
		params[pi].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		++pi;
	}
	if (_hasCbv) {
		params[pi].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[pi].Descriptor.ShaderRegister = 0; // b0
		params[pi].Descriptor.RegisterSpace = 0;
		params[pi].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}

	D3D12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.NumParameters = numParams;
	rsDesc.pParameters = params.data();
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(std::string("Failed to serialize compute root signature: ") + msg);
	}
	CHECK_OK(_context.device()->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)),
		"Failed to create compute root signature");
}

void ComputePass::createPipelineState(const std::vector<std::uint8_t>& csBytes) {
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = _rootSignature.Get();
	desc.CS = { csBytes.data(), csBytes.size() };
	CHECK_OK(_context.device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_pso)),
		"Failed to create compute pipeline state");
}

void ComputePass::dispatch(ID3D12DescriptorHeap* heap,
	const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& srvs,
	const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& uavs,
	UINT threadGroupsX, UINT threadGroupsY, UINT threadGroupsZ,
	ID3D12GraphicsCommandList* cmdList,
	D3D12_GPU_VIRTUAL_ADDRESS cbvAddress,
	ID3D12DescriptorHeap* samplerHeap,
	const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& samplers) {
	auto* cl = cmdList ? cmdList : _context.commandList();
	cl->SetPipelineState(_pso.Get());
	cl->SetComputeRootSignature(_rootSignature.Get());
	if (_numSamplerRanges > 0) {
		ID3D12DescriptorHeap* const heaps[] = { heap, samplerHeap };
		cl->SetDescriptorHeaps(2, heaps);
	} else {
		cl->SetDescriptorHeaps(1, &heap);
	}

	UINT pi = 0;
	for (UINT i = 0; i < _numSrvRanges; ++i) {
		cl->SetComputeRootDescriptorTable(pi++, srvs[i]);
	}
	for (UINT i = 0; i < _numUavRanges; ++i) {
		cl->SetComputeRootDescriptorTable(pi++, uavs[i]);
	}
	for (UINT i = 0; i < _numSamplerRanges; ++i) {
		cl->SetComputeRootDescriptorTable(pi++, samplers[i]);
	}
	if (_hasCbv) {
		cl->SetComputeRootConstantBufferView(pi++, cbvAddress);
	}
	cl->Dispatch(threadGroupsX, threadGroupsY, threadGroupsZ);
}

void ComputePass::dispatch(ID3D12DescriptorHeap* heap,
	D3D12_GPU_DESCRIPTOR_HANDLE uav,
	UINT threadGroupsX, UINT threadGroupsY, UINT threadGroupsZ,
	D3D12_GPU_DESCRIPTOR_HANDLE srvBase,
	ID3D12GraphicsCommandList* cmdList) {
	std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> srvs;
	if (_numSrvRanges > 0) {
		for (UINT i = 0; i < _numSrvRanges; ++i) {
			srvs.push_back(srvBase);
			srvs.back().ptr += i * _context.device()->GetDescriptorHandleIncrementSize(
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
	}
	std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> uavs{ uav };
	dispatch(heap, srvs, uavs, threadGroupsX, threadGroupsY, threadGroupsZ, cmdList, 0);
}
