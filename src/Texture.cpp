#include "Texture.h"

#include <windows.h>
#include <wincodec.h>

#include <d3dx12.h>

#include <mutex>
#include <stdexcept>
#include <vector>

#define CHECK_TEX(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

namespace {

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

void Texture::load(D3D12Context& context, const std::string& filePath) {
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
	_width = w;
	_height = h;

	auto* device = context.device();
	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	const CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(_format, w, h, 1, 1);

	HRESULT hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_texture));
	CHECK_TEX(hr, "Failed to create texture resource");

	// 计算上传需要的临时缓冲大小。
	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

	const CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
	hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
	CHECK_TEX(hr, "Failed to create texture upload buffer");

	// 录制上传命令。
	auto* allocator = context.commandAllocator();
	auto* cl = context.commandList();
	CHECK_TEX(allocator->Reset(), "Failed to reset allocator");
	CHECK_TEX(cl->Reset(allocator, nullptr), "Failed to reset command list");

	D3D12_SUBRESOURCE_DATA texData{};
	texData.pData = pixels.data();
	texData.RowPitch = static_cast<LONG_PTR>(_width * 4);
	texData.SlicePitch = static_cast<LONG_PTR>(texData.RowPitch * _height);
	UpdateSubresources(cl, _texture.Get(), uploadBuffer.Get(), 0, 0, 1, &texData);

	const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cl->ResourceBarrier(1, &barrier);

	CHECK_TEX(cl->Close(), "Failed to close command list");
	context.submitAndWait();
}

void Texture::createSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle) const {
	if (!_texture) {
		throw std::runtime_error("Texture is not loaded");
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = _format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(_texture.Get(), &srvDesc, srvHandle);
}
