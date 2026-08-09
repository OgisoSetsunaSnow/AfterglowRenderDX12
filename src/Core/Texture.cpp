#include "Texture.h"
#include "ShaderCompiler.h"

#include <windows.h>
#include <wincodec.h>

#include <d3dx12.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <format>
#include <vector>

#define CHECK_TEX(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

namespace {

inline float SrgbToLinear(float c) {
	return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float LinearToSrgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

// 共享 GPU mip generator（vkCmdBlitImage 等价的 D3D12 实现）：compute 双线性 blit 逐级生成 mip。
struct MipGenerator {
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap; // 跨 Texture 共享，避免每个纹理建 sampler 堆
};

MipGenerator& GetMipGenerator(ID3D12Device* device) {
	static MipGenerator gen;
	if (!gen.pso) {
		shader::CompileOptions csOptions;
		csOptions.entryPoint = L"main";
		csOptions.targetProfile = L"cs_6_0";
		const auto cs = shader::compileFromFile("Shaders/GenerateMip_CS.hlsl", csOptions);

		// 3 参数 root signature：t0 SRV（MipSlice 传递 srcMip）+ u0 UAV + s0 sampler。
		// 4 参数（含 CBV/root constant）在本驱动 CreateRootSignature 返回 0x887A0005，故简化。
		D3D12_DESCRIPTOR_RANGE srvRange{};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = 1;
		srvRange.BaseShaderRegister = 0;
		D3D12_DESCRIPTOR_RANGE uavRange{};
		uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRange.NumDescriptors = 1;
		uavRange.BaseShaderRegister = 0;
		D3D12_DESCRIPTOR_RANGE sampRange{};
		sampRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		sampRange.NumDescriptors = 1;
		sampRange.BaseShaderRegister = 0;

		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable.NumDescriptorRanges = 1;
		params[0].DescriptorTable.pDescriptorRanges = &srvRange;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &uavRange;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &sampRange;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rsDesc{};
		rsDesc.NumParameters = 3;
		rsDesc.pParameters = params;
		Microsoft::WRL::ComPtr<ID3DBlob> sig;
		Microsoft::WRL::ComPtr<ID3DBlob> err;
		if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
			throw std::runtime_error("Failed to serialize mip generator root signature");
		}
		// 0x887A0005 = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE：驱动早期可能暂时不可用，重试几次。
		HRESULT hr = E_FAIL;
		for (int attempt = 0; attempt < 8 && FAILED(hr); ++attempt) {
			hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
				IID_PPV_ARGS(&gen.rootSig));
			if (FAILED(hr)) { Sleep(50); }
		}
		if (FAILED(hr)) {
			throw std::runtime_error("Failed to create mip generator root signature (HRESULT 0x" +
				std::format("{:08X}", static_cast<unsigned>(hr)) + ")");
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = gen.rootSig.Get();
		psoDesc.CS = { cs.data(), cs.size() };
		CHECK_TEX(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&gen.pso)),
			"Failed to create mip generator pipeline state");

		// 共享 sampler（linear + CLAMP，MaxLOD 覆盖 4K 13 级）。
		D3D12_DESCRIPTOR_HEAP_DESC sh{};
		sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		sh.NumDescriptors = 1;
		sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		CHECK_TEX(device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&gen.samplerHeap)),
			"Failed to create mip generator sampler heap");
		D3D12_SAMPLER_DESC sd{};
		sd.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sd.MaxAnisotropy = 1;
		sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sd.MinLOD = 0.0f;
		sd.MaxLOD = 13.0f; // 覆盖常见 4K 纹理 mipCount=13
		device->CreateSampler(&sd, gen.samplerHeap->GetCPUDescriptorHandleForHeapStart());
	}
	return gen;
}

// UTF-8 → UTF-16（供 WIC 文件路径使用）。
std::wstring toWide(const std::string& utf8) {
	if (utf8.empty()) {
		return {};
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring result(len > 0 ? static_cast<std::size_t>(len) - 1 : 0, L'\0');
	if (len > 0) {
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), len);
	}
	return result;
}

// 用 WIC 解码图片为 32bpp RGBA 像素。
std::vector<std::uint8_t> decodeImage(const std::string& filePath, UINT& width, UINT& height) {
	Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	CHECK_TEX(hr, "Failed to create WIC factory");

	Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
	const std::wstring wpath = toWide(filePath);
	hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, &decoder);
	CHECK_TEX(hr, "Failed to open image: " + filePath);

	Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	CHECK_TEX(hr, "Failed to get image frame");

	Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	CHECK_TEX(hr, "Failed to create format converter");
	hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	CHECK_TEX(hr, "Failed to convert pixel format");

	hr = converter->GetSize(&width, &height);
	CHECK_TEX(hr, "Failed to get image size");

	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	hr = converter->CopyPixels(nullptr, width * 4,
		static_cast<UINT>(pixels.size()), pixels.data());
	CHECK_TEX(hr, "Failed to copy pixels");

	return pixels;
}

} // namespace

void Texture::load(D3D12Context& context, const std::string& filePath,
	DXGI_FORMAT format) {
	// sRGB 格式不能有 UAV（D3D12 限制）→ 内部用 UNORM（linear），像素手动 sRGB→linear，
	// mip 在 linear 空间 GPU 生成，SRV 也用 UNORM。这是 API 差异必要适配。
	const bool wantsSrgb = (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	_format = wantsSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
	OutputDebugStringA(("[tex] loading " + filePath + "\n").c_str());
	// WIC 依赖 COM，进程内仅初始化一次。
	static std::once_flag comFlag;
	std::call_once(comFlag, [] {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	});

	UINT w = 0, h = 0;
	std::vector<std::uint8_t> pixels = decodeImage(filePath, w, h);
	if (pixels.empty()) {
		throw std::runtime_error("Decoded image is empty: " + filePath);
	}
	if (wantsSrgb) {
		for (std::size_t i = 0; i < pixels.size(); i += 4) {
			pixels[i] = static_cast<std::uint8_t>(std::clamp(SrgbToLinear(pixels[i] / 255.0f), 0.0f, 1.0f) * 255.0f + 0.5f);
			pixels[i + 1] = static_cast<std::uint8_t>(std::clamp(SrgbToLinear(pixels[i + 1] / 255.0f), 0.0f, 1.0f) * 255.0f + 0.5f);
			pixels[i + 2] = static_cast<std::uint8_t>(std::clamp(SrgbToLinear(pixels[i + 2] / 255.0f), 0.0f, 1.0f) * 255.0f + 0.5f);
		}
	}
	_width = w;
	_height = h;

	UINT big = (w > h) ? w : h;
	UINT mipCount = 1;
	while (big > 1) { big >>= 1; ++mipCount; }
	_mipLevels = mipCount;

	auto* device = context.device();

	// 创建纹理（完整 mip 链 + ALLOW_UNORDERED_ACCESS，供 GPU compute 生成 mip）。
	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(_format, w, h, 1, mipCount);
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	OutputDebugStringA(("[tex] mip count " + std::to_string(mipCount) + " " + std::to_string(w) + "x" + std::to_string(h) + "\n").c_str());
	CHECK_TEX(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_texture)),
		"Failed to create texture resource");

	// 上传 mip 0（源数据）。
	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
	const CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
	CHECK_TEX(device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)),
		"Failed to create texture upload buffer");

	auto* allocator = context.commandAllocator();
	auto* cl = context.commandList();
	CHECK_TEX(allocator->Reset(), "Failed to reset allocator");
	CHECK_TEX(cl->Reset(allocator, nullptr), "Failed to reset command list");

	D3D12_SUBRESOURCE_DATA sub{};
	sub.pData = pixels.data();
	sub.RowPitch = static_cast<LONG_PTR>(w * 4);
	sub.SlicePitch = static_cast<LONG_PTR>(sub.RowPitch * h);
	UpdateSubresources(cl, _texture.Get(), uploadBuffer.Get(), 0, 0, 1, &sub);

	// barrier mip0 COPY_DEST -> SRV（供后续 compute 采样）。
	{
		const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
			_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			0);
		cl->ResourceBarrier(1, &b);
	}

	// 描述符堆：index 0..N-2 = SRV per srcMip（MipSlice=0..N-2，MipLevels=1），
	//               index N-1..2N-3 = UAV per mip（MipSlice=1..N-1）。
	// mipCount==1 时（如 1x1 占位纹理）totalDesc=0，跳过整套 mip 堆/srv/uav 创建。
	const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	if (mipCount > 1) {
		const UINT totalDesc = (mipCount - 1) * 2;
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.NumDescriptors = totalDesc;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		CHECK_TEX(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_mipHeap)),
			"Failed to create mip descriptor heap");

		_mipSrvGpu = _mipHeap->GetGPUDescriptorHandleForHeapStart();
		_mipUavGpu.clear();
		for (UINT mip = 1; mip < mipCount; ++mip) {
			// SRV for srcMip = mip-1（MipSlice = mip-1, MipLevels = 1）。
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = _format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MostDetailedMip = mip - 1;
			srv.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = _mipHeap->GetCPUDescriptorHandleForHeapStart();
			srvCpu.ptr += (mip - 1) * inc;
			device->CreateShaderResourceView(_texture.Get(), &srv, srvCpu);

			// UAV for dstMip = mip（MipSlice = mip）。
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = _format;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D.MipSlice = mip;
			D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = _mipHeap->GetCPUDescriptorHandleForHeapStart();
			uavCpu.ptr += ((mip - 1) + (mipCount - 1)) * inc;
			device->CreateUnorderedAccessView(_texture.Get(), nullptr, &uav, uavCpu);
			_mipUavGpu.push_back(_mipHeap->GetGPUDescriptorHandleForHeapStart());
			_mipUavGpu.back().ptr += ((mip - 1) + (mipCount - 1)) * inc;
		}
	}

	// GPU 生成 mip 1..N-1（vkCmdBlitImage 等价：compute 双线性 blit）。
	if (mipCount > 1) {
		MipGenerator& gen = GetMipGenerator(device);
		ID3D12DescriptorHeap* const heaps[] = { _mipHeap.Get(), gen.samplerHeap.Get() };
		cl->SetDescriptorHeaps(2, heaps);
		cl->SetPipelineState(gen.pso.Get());
		cl->SetComputeRootSignature(gen.rootSig.Get());
		for (UINT mip = 1; mip < mipCount; ++mip) {
			// 当前级从 COPY_DEST -> UAV（写）。
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mip);
				cl->ResourceBarrier(1, &b);
			}
			// 读 srcMip = mip-1（SRV MostDetailedMip），写 dst mip（UAV）。
			const D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{ _mipSrvGpu.ptr + static_cast<SIZE_T>(mip - 1) * inc };
			cl->SetComputeRootDescriptorTable(0, srvHandle);
			cl->SetComputeRootDescriptorTable(1, _mipUavGpu[mip - 1]);
			cl->SetComputeRootDescriptorTable(2, gen.samplerHeap->GetGPUDescriptorHandleForHeapStart());
			const UINT dstW = (w >> mip) ? (w >> mip) : 1u;
			const UINT dstH = (h >> mip) ? (h >> mip) : 1u;
			cl->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
			// 当前级 UAV -> SRV（供下一级采样 / 最终渲染）。
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					_texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					mip);
				cl->ResourceBarrier(1, &b);
			}
		}
	}

	CHECK_TEX(cl->Close(), "Failed to close command list");
	context.submitAndWait();
	OutputDebugStringA(("[tex] loaded " + filePath + " (" + std::to_string(w) + "x" + std::to_string(h) + ", " + std::to_string(mipCount) + " mips, GPU)\n").c_str());
}

void Texture::warmUpMipGenerator(D3D12Context& context) {
	GetMipGenerator(context.device());
}

void Texture::createSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle) const {
	if (!_texture) {
		throw std::runtime_error("Texture is not loaded");
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = _format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = -1; // 全 mip，shader 自动 LOD 选样
	device->CreateShaderResourceView(_texture.Get(), &srvDesc, srvHandle);
}
