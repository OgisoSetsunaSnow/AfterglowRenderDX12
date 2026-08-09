#pragma once

#include "D3D12Core.h"
#include "Texture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

class Mesh;
class ComputePass;
class FractalNoise;

// Grass instancing（照搬参考项目 GrassData.mat + GrassInstancing.mat）。
// 数据流：
//   GrassData(2048² R8G8B8A8_UNORM) 一次性（GrassData_CS：坡度/干旱度 → possibility/hash/scaling）
//   GrassInstancing_CS 每帧：相机前 128×128 区域 → 视锥剔除 + 坡度剔除 → InterlockedAdd 计数
//     → 写 InstanceBuffer(16384) + IndirectBuffer(D3D12_DRAW_INDEXED_ARGUMENTS)
//   ExecuteIndirect 间接绘制 InstancingGrass.fbx（GrassInstancing_VS/FS，风 + 地形混合 + DefaultShading）。
// z-up 语义（照搬参考）：视锥/相机/地形采样全 z-up；渲染 viewProjection 由 C++ 传 z-up 版。
class GrassRenderer {
public:
	GrassRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& grassDataCs,
		const std::vector<std::uint8_t>& instancingCs,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		ID3D12Resource* terrainHeightTex,
		ID3D12Resource* terrainNormalTex,
		ID3D12Resource* terrainSurfaceTex,
		ID3D12Resource* meteorographTex,
		const Texture& grassAlbedoTex,
		ID3D12Resource* fractalNormalTex,
		ID3D12Resource* ambientTex);
	~GrassRenderer() = default;

	GrassRenderer(const GrassRenderer&) = delete;
	GrassRenderer& operator=(const GrassRenderer&) = delete;

	// 一次性：GrassData 生成（独立 list + fence）。
	void initialize();

	// 每帧：GrassInstancing_CS → ExecuteIndirect 间接绘制。
	// cameraPos/frustum 传 y-up 世界；内部转 z-up（参考坐标系）。
	void render(const glm::mat4& view, const glm::mat4& proj,
		const glm::vec3& cameraPos, const glm::vec3& cameraFwd,
		float time, float deltaTime, const Mesh& mesh,
		const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& dsv);

private:
	void createBuffers();
	void createComputeRootSignature();
	void createGraphicsRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps);
	void createCommandSignature();
	void createSamplers();
	// 启动诊断：读回 IndirectBuffer instanceCount + 前 4 个实例矩阵（验证实例生成与矩阵合法性）。
	void debugPrintReadback();

	D3D12Context& _context;
	std::vector<std::uint8_t> _grassDataCs, _instancingCs;

	static constexpr UINT kMaxInstances = 16384;
	struct InstanceBufferElement {
		glm::mat4 model;         // 64B
		glm::mat4 invTransModel; // 64B
		glm::vec2 wind;          // 8B
	};
	static constexpr UINT kInstanceStride = sizeof(InstanceBufferElement); // 136B（HLSL 对齐后 144B，见 cpp）

	Microsoft::WRL::ComPtr<ID3D12Resource> _grassDataTex;    // 2048² R8G8B8A8_UNORM
	Microsoft::WRL::ComPtr<ID3D12Resource> _instanceBuffer;  // UAV/SRV
	Microsoft::WRL::ComPtr<ID3D12Resource> _indirectBuffer;  // D3D12_DRAW_INDEXED_ARGUMENTS
	bool _grassDataInit = false;                              // GrassData 首帧内联生成标志
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;      // CBV_SRV_UAV
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _computeRootSig;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _graphicsRootSig;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;      // graphics instanced
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> _cmdSig; // ExecuteIndirect

	std::unique_ptr<ComputePass> _grassDataPass, _instancingPass;
	std::unique_ptr<ComputePass> _dummyDataPass; // 占位（GrassData 用）

	ID3D12Resource* _terrainHeightTex = nullptr;
	ID3D12Resource* _terrainNormalTex = nullptr;
	ID3D12Resource* _terrainSurfaceTex = nullptr;
	ID3D12Resource* _meteorographTex = nullptr;
	ID3D12Resource* _fractalNormalTex = nullptr;
	ID3D12Resource* _ambientTex = nullptr;
	Texture _grassAlbedo;

	Microsoft::WRL::ComPtr<ID3D12Resource> _simCbv;   // b0（compute）
	Microsoft::WRL::ComPtr<ID3D12Resource> _vsCbv;    // b0（graphics VS）
	Microsoft::WRL::ComPtr<ID3D12Resource> _globalCbv; // b1（graphics FS）
	Microsoft::WRL::ComPtr<ID3D12Resource> _indirectUpload; // 上传堆：每帧写 D3D12_DRAW_INDEXED_ARGUMENTS
	Microsoft::WRL::ComPtr<ID3D12Resource> _debugReadbackIndirect; // 20B READBACK（启动诊断）
	Microsoft::WRL::ComPtr<ID3D12Resource> _debugReadbackInstance; // 4×144B READBACK（启动诊断）
	UINT _frameCounter = 0;      // 渲染帧计数
	bool _debugCopyPending = false; // 上一帧已 copy 到 READBACK，待本帧打印
};
