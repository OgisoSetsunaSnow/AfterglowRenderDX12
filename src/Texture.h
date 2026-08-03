#pragma once

#include "D3D12Core.h"

#include <string>

// 阶段 3：纹理资源。使用 WIC 加载 PNG/JPG 等图片，创建 D3D12 纹理并上传。
// 描述符（SRV）由上层管理，通过 createSrv 写入指定堆位置。
class Texture {
public:
	Texture() = default;
	~Texture() = default;

	// 从图片文件加载并创建 GPU 纹理（数据已上传，状态为 PIXEL_SHADER_RESOURCE）。
	void load(D3D12Context& context, const std::string& filePath);

	// 在当前纹理上创建 SRV 写入指定描述符位置。
	void createSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle) const;

	ID3D12Resource* resource() const { return _texture.Get(); }
	bool valid() const { return _texture != nullptr; }
	UINT width() const { return _width; }
	UINT height() const { return _height; }

private:
	Microsoft::WRL::ComPtr<ID3D12Resource> _texture;
	UINT _width = 0;
	UINT _height = 0;
	DXGI_FORMAT _format = DXGI_FORMAT_R8G8B8A8_UNORM;
};
