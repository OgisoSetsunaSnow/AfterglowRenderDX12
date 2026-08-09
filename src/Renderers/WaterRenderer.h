#pragma once

#include "D3D12Core.h"
#include "Texture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

class Mesh;

// 水面渲染器（照搬参考项目 WaterMesh：Water_VS + Water_FS）。
// 关键设计：
//   - 水网格 256x256 interval 4.0（复用 Mesh::makeTerrainVertices），VS 采样 TerrainHeight.y 水位高度，
//     globalOffset 跟随相机（与地形一致的 LOD 视差）。
//   - 折射 sceneColor 需要"当前离屏 RT 内容" → D3D12 不允许同一资源 RTV+SRV 同 pass。
//     方案：每帧把 offscreen 拷贝到 waterSceneCopy，水 pass 采样 waterSceneCopy（调用方负责拷贝+barrier）。
//   - 根签名 t0-t7 连续 8 SRV（sceneColorCopy/depth/TerrainHeight/TerrainNormal/WaterSediment/WaterVelocity/FractalSimplex/ambient）
//     + s0-s1 sampler + b0 CBV。独立 waterHeap，避免与其他堆冲突。
//   - FractalSimplexNoise（256x256 R32Float）由 initializer CS 一次性生成（参考 FractalNoise.mat）。
class WaterRenderer {
public:
	WaterRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		const std::vector<std::uint8_t>& fractalInitCsBytes,
		ID3D12Resource* heightTex,   // TerrainHeight R32G32_FLOAT
		ID3D12Resource* normalTex,   // TerrainNormal R8G8B8A8_SNORM
		ID3D12Resource* sedimentTex, // WaterSediment R32_FLOAT
		ID3D12Resource* velocityTex, // WaterVelocity R16G16_FLOAT
		ID3D12Resource* depthTex,    // 场景深度 D32_FLOAT（MeshRenderer）
		const Texture& ambientTex,   // DefaultSky IBL
		DXGI_FORMAT rtvFormat);
	~WaterRenderer() = default;

	WaterRenderer(const WaterRenderer&) = delete;
	WaterRenderer& operator=(const WaterRenderer&) = delete;

	// 一次性 dispatch FractalSimplexNoise initializer（独立 command list + fence 等待）。
	void initializeFractalNoise();

	// 拷贝当前离屏场景色到 waterSceneCopy（调用方需先把 offscreen barrier 到 COPY_SOURCE）。
	// 本方法内部：waterSceneCopy COPY_SOURCE→COPY_DEST→PIXEL_SHADER_RESOURCE（状态自跟踪）。
	void copySceneColor(ID3D12Resource* sceneResource);

	// 绘制水面到 rtv（= offscreen RTV）。调用方需保证：
	//   - waterSceneCopy 已拷贝且处于 PIXEL_SHADER_RESOURCE（copySceneColor 完成）
	//   - depth 处于 DEPTH_READ（水 pass 深度只读测试 + 采样）
	//   - offscreen 处于 RENDER_TARGET
	void render(const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& dsv,
		const glm::mat4& viewProj, const glm::mat4& model,
		const glm::vec3& cameraPos, const glm::vec3& cameraFwd,
		const glm::vec3& lightDir, const glm::vec4& lightColor,
		UINT width, UINT height, float nearZ, float farZ, float time);

	// 访问器（供 Main 描述符堆布局协调）。
	ID3D12DescriptorHeap* heap() const { return _heap.Get(); }
	ID3D12DescriptorHeap* samplerHeap() const { return _samplerHeap.Get(); }
	ID3D12Resource* sceneCopyResource() const { return _sceneCopy.Get(); }
	D3D12_RESOURCE_STATES sceneCopyState() const { return _sceneCopyState; }

private:
	void createTextures();
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat);
	void createCbv();
	void createSamplers();

	D3D12Context& _context;
	std::vector<std::uint8_t> _fractalInitCs;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	std::unique_ptr<Mesh> _waterMesh; // 256x256 interval 4.0 水网格（构造时创建）

	// 数据纹理（外部资源引用，仅 SRV 描述符写本 heap）。
	ID3D12Resource* _heightTex = nullptr;
	ID3D12Resource* _normalTex = nullptr;
	ID3D12Resource* _sedimentTex = nullptr;
	ID3D12Resource* _velocityTex = nullptr;
	ID3D12Resource* _depthTex = nullptr;

	// 自建资源：waterSceneCopy（离屏拷贝）、FractalSimplexNoise（compute 生成）。
	Microsoft::WRL::ComPtr<ID3D12Resource> _sceneCopy;
	Microsoft::WRL::ComPtr<ID3D12Resource> _fractalTex;
	D3D12_RESOURCE_STATES _sceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap; // CBV_SRV_UAV, 9 desc（0-7 SRV + 8 fractal UAV）
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
	D3D12_GPU_DESCRIPTOR_HANDLE _fractalSrvGpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE _fractalUavGpu{};

	Microsoft::WRL::ComPtr<ID3D12Resource> _cbv; // b0 上传堆
	Texture _ambient; // 引用 ambient 资源描述（仅存 SRV 格式信息）
	DXGI_FORMAT _ambientFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
};
