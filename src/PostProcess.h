#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 阶段 10：全屏后处理 pass（对应参考项目 PostProcess pass + PostProcess_VS/FS）。
// 采样离屏 sceneColor（+ 后续 bloomCombinedTexture），输出到交换链后台缓冲。
class PostProcess {
public:
	PostProcess(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes);
	~PostProcess() = default;

	PostProcess(const PostProcess&) = delete;
	PostProcess& operator=(const PostProcess&) = delete;

	// 渲染全屏后处理（照搬参考 PostProcess_FS：ChromaticAberration → HeightFog(ambientTexture) →
	// Underwater(TerrainHeight) → Bloom + SkylightShaft → Dust → Vignette → Cursor → LUTMapping）。
	// sceneSrv..terrainHeightSrv 必须来自同一个 srvHeap 且按 t0..t5 物理连续（D3D12 一个 CBV_SRV_UAV 堆）。
	// effects 仅 bit3 = noise 调试显示（参考效果无条件执行）。
	// view/lightDir 供光轴（SkylightShaft）把方向光变换到相机空间；cursorPos 为屏幕像素坐标。
	void render(ID3D12DescriptorHeap* srvHeap,
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
		std::uint32_t effects = 0u);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps);
	void createParamCbv();

	D3D12Context& _context;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> _paramCbv; // b0：相机/雾参数
};
