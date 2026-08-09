#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

class ComputePass;

// ParticleExample（照搬参考 ParticleExample.mat：4096 粒子 compute 模拟 + 点精灵渲染）。
// 数据流：
//   ParticleSSBO(4096×48B) 初始化（ParticleExampleInitializer_CS，Once，u0/u1）
//   每帧 ParticleExample_CS（b0 CBV + t0 TerrainHeight + t1 Meteorograph + s0 + u0 ParticleSSBO in-place）
//   渲染：POINT_LIST 点精灵，VS 用 SV_VertexID 读 ParticleSSBO + SV_PointSize；
//   FS 圆形 alpha + sceneColor（offscreen 拷贝）折射混合，Transparency（alpha blend、depthWrite=false）。
// 粒子模拟 z-up（照搬参考）；渲染 viewProjection = proj*view*rotate(-90X)。
class ParticleRenderer {
public:
	ParticleRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& initCs,
		const std::vector<std::uint8_t>& simCs,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& gsBytes,
		const std::vector<std::uint8_t>& psBytes,
		ID3D12Resource* terrainHeightTex, ID3D12Resource* meteorographTex,
		DXGI_FORMAT rtvFormat);
	~ParticleRenderer() = default;

	ParticleRenderer(const ParticleRenderer&) = delete;
	ParticleRenderer& operator=(const ParticleRenderer&) = delete;

	// 一次性初始化（独立 command list + fence）。
	void initialize();

	// 每帧：sim dispatch → offscreen 拷贝到 sceneCopy（折射采样）→ 点精灵绘制。
	// sceneColorResource = offscreen（当前 RENDER_TARGET），render 内部做 COPY_SOURCE 拷贝后还原。
	void render(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
		float time, float deltaTime, ID3D12Resource* sceneColorResource,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv, const D3D12_CPU_DESCRIPTOR_HANDLE& dsv);

private:
	void createBuffers();
	void createGraphicsRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& gs,
		const std::vector<std::uint8_t>& ps);
	void createSamplers();

	D3D12Context& _context;
	DXGI_FORMAT _rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	static constexpr UINT kParticleCount = 4096;
	static constexpr UINT kParticleStride = 48; // float3+float+float3+float+float4

	Microsoft::WRL::ComPtr<ID3D12Resource> _particleBuffer; // UAV(init/sim) + SRV(render)
	Microsoft::WRL::ComPtr<ID3D12Resource> _testTexture;    // 128×32 R8G8B8A8_UNORM（init 写，FS Load(0,0)）
	Microsoft::WRL::ComPtr<ID3D12Resource> _sceneCopy;      // offscreen 拷贝（折射采样源）
	D3D12_RESOURCE_STATES _sceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;        // 7 desc
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap; // 1 sampler（s0 场景色）
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _graphicsRootSig;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;

	std::unique_ptr<ComputePass> _initPass, _simPass;

	ID3D12Resource* _terrainHeightTex = nullptr;
	ID3D12Resource* _meteorographTex = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> _simCbv;    // b0（compute：cameraPosition/prevCameraPosition/time/deltaTime）
	Microsoft::WRL::ComPtr<ID3D12Resource> _renderCbv; // b0（graphics：viewProjection/screenRes/cameraFov）
	glm::vec3 _prevCameraPosYup = glm::vec3(0.0f, 0.0f, 0.0f); // 上一帧相机位置（z-up，粒子跟随）
	bool _prevCameraValid = false; // 首帧 prevCamera = 当前（camDelta=0）
};
