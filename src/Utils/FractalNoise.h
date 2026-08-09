#pragma once

#include "D3D12Core.h"

#include <cstdint>
#include <vector>

// FractalNoise 数据流（完整照搬参考 FractalNoise.mat）：
//   FractalSimplexNoise(256² R32F) + FractalWorleyNoise(256² R32F) 各自 initializer 一次性生成，
//   然后 FractalNoise_CS（Once）读两者相邻 texel 差分 → FractalNormal(256² R8G8B8A8_SNORM)。
// 供未来 Grass/ShellFur/AnkhaFur 采样（与水面 WaterRenderer 内部 Simplex 独立，避免耦合）。
class FractalNoise {
public:
	FractalNoise(D3D12Context& context,
		const std::vector<std::uint8_t>& simplexInitCs,
		const std::vector<std::uint8_t>& worleyInitCs,
		const std::vector<std::uint8_t>& fractalNoiseCs);
	~FractalNoise() = default;

	FractalNoise(const FractalNoise&) = delete;
	FractalNoise& operator=(const FractalNoise&) = delete;

	// 一次性初始化（独立 command list + fence 等待）：simplex → worley → normal（顺序依赖）。
	void initialize();

	ID3D12DescriptorHeap* heap() const { return _heap.Get(); }
	D3D12_GPU_DESCRIPTOR_HANDLE simplexSrv() const;
	D3D12_GPU_DESCRIPTOR_HANDLE worleySrv() const;
	D3D12_GPU_DESCRIPTOR_HANDLE normalSrv() const;
	ID3D12Resource* simplexResource() const { return _simplexTex.Get(); }
	ID3D12Resource* worleyResource() const { return _worleyTex.Get(); }
	ID3D12Resource* normalResource() const { return _normalTex.Get(); }

private:
	void createTextures();
	void createDescriptors();
	void createSamplerHeap();

	D3D12Context& _context;
	std::vector<std::uint8_t> _simplexInitCs;
	std::vector<std::uint8_t> _worleyInitCs;
	std::vector<std::uint8_t> _fractalNoiseCs;

	Microsoft::WRL::ComPtr<ID3D12Resource> _simplexTex, _worleyTex, _normalTex;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;        // CBV_SRV_UAV, 6 desc（3 SRV + 3 UAV shader-visible）
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap; // 采样器（future Grass 用）
	D3D12_GPU_DESCRIPTOR_HANDLE _simplexSrvGpu{}, _worleySrvGpu{}, _normalSrvGpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE _simplexUavGpu{}, _worleyUavGpu{}, _normalUavGpu{};
};
