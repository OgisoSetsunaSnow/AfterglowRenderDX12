#pragma once

#include "D3D12Core.h"
#include "Texture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Mesh;

// 程序化地形渲染器（完整照搬参考项目 Terrain 数据流）：
//   compute 数据纹理：TerrainHeight/Normal/Surface + Meteorograph；
//   表面贴图：grass (BC/N/AoRDp) + slope (BC/N/AoRDp) + ambientTexture(IBL)；
//   渲染：Terrain_VS（采样高度位移）+ Terrain_FS（完整 DefaultShading PBR）。
class TerrainRenderer {
public:
	struct TexturePaths {
		std::string grassAlbedo, grassNormal, grassProperty;
		std::string slopeAlbedo, slopeNormal, slopeProperty;
		std::string ambient;
	};

	TerrainRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		const TexturePaths& paths,
		UINT dataSideElements = 1024,
		DXGI_FORMAT rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT);
	~TerrainRenderer() = default;

	TerrainRenderer(const TerrainRenderer&) = delete;
	TerrainRenderer& operator=(const TerrainRenderer&) = delete;

	// 描述符句柄（供 compute 初始化/模拟 dispatch）。
	ID3D12DescriptorHeap* heap() const { return _heap.Get(); }
	ID3D12DescriptorHeap* samplerHeap() const { return _samplerHeap.Get(); }
	// compute 采样用的 sampler GPU 句柄（s0 terrain ANISO16/WRAP，双线性等效）。
	D3D12_GPU_DESCRIPTOR_HANDLE terrainSamplerGpu() const {
		return _samplerHeap->GetGPUDescriptorHandleForHeapStart();
	}
	D3D12_GPU_DESCRIPTOR_HANDLE heightUav() const { return _heightUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE normalUav() const { return _normalUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE surfaceUav() const { return _surfaceUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE meteorographUav() const { return _meteorographUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE pluviometerUav() const { return _pluviometerUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE waterFlowUav() const { return _waterFlowUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE waterSedimentUav() const { return _waterSedimentUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE waterVelocityUav() const { return _waterVelocityUavGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE dataSrvBase() const { return _heap->GetGPUDescriptorHandleForHeapStart(); }
	// Meteorograph 每帧模拟（Meteorograph_CS）用：t0=height, u0=meteorograph, u1=pluviometer。
	D3D12_GPU_DESCRIPTOR_HANDLE meteorographSrv() const { return _meteorographSrvGpu; }
	D3D12_GPU_DESCRIPTOR_HANDLE heightSrv() const { return _heap->GetGPUDescriptorHandleForHeapStart(); }
	// TerrainData_CS 用：t0=meteorograph, t1=pluviometer（采样）。
	D3D12_GPU_DESCRIPTOR_HANDLE pluviometerSrv() const { return _pluviometerSrvGpu; }

	ID3D12Resource* heightResource() const { return _heightTex.Get(); }
	ID3D12Resource* normalResource() const { return _normalTex.Get(); }
	ID3D12Resource* surfaceResource() const { return _surfaceTex.Get(); }
	ID3D12Resource* meteorographResource() const { return _meteorographTex.Get(); }
	ID3D12Resource* pluviometerResource() const { return _pluviometerTex.Get(); }
	ID3D12Resource* waterFlowResource() const { return _waterFlowTex.Get(); }
	ID3D12Resource* waterSedimentResource() const { return _waterSedimentTex.Get(); }
	ID3D12Resource* waterVelocityResource() const { return _waterVelocityTex.Get(); }

	// 绘制地形（调用方需已设 RT/DSV/深度已清；绘制写深度）。
	void render(const glm::mat4& viewProj, const glm::vec3& cameraPos, const Mesh& mesh,
		const glm::mat4& model, const glm::vec3& dirLightDir, const glm::vec4& dirLightColor);

private:
	void createDataTextures();
	void createSurfaceTextures(const TexturePaths& paths);
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps, DXGI_FORMAT rtvFormat);
	void createCbv();

	D3D12Context& _context;
	UINT _dataSide = 1024;
	DXGI_FORMAT _rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	// compute 数据纹理（index: SRV 0-11, UAV 12-19）。
	Microsoft::WRL::ComPtr<ID3D12Resource> _heightTex, _normalTex, _surfaceTex, _meteorographTex, _pluviometerTex;
	Microsoft::WRL::ComPtr<ID3D12Resource> _waterFlowTex, _waterSedimentTex, _waterVelocityTex;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap; // SHADER_VISIBLE, 20 desc
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;
	D3D12_GPU_DESCRIPTOR_HANDLE _heightUavGpu{}, _normalUavGpu{}, _surfaceUavGpu{}, _meteorographUavGpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE _pluviometerUavGpu{}, _waterFlowUavGpu{}, _waterSedimentUavGpu{}, _waterVelocityUavGpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE _meteorographSrvGpu{}, _pluviometerSrvGpu{};

	// 表面贴图（SRV index 4-10）。
	Texture _grassAlbedo, _grassNormal, _grassProperty;
	Texture _slopeAlbedo, _slopeNormal, _slopeProperty;
	Texture _ambient;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
	Microsoft::WRL::ComPtr<ID3D12Resource> _cbv;
};
