#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

class Mesh;

// ShellFur instancing（照搬参考项目 ShellFur.mat + AnkhaFur.mati）。
// AnkhaFur.fbx 用 instanceCount=8 壳层实例化：每实例沿顶点法线偏移 (instanceID+1)*shellInterval，
// FS 用 FractalWorleyNoise/FractalNormal 做毛发生长噪声，按 instanceID 裁剪（clip）出蓬松毛层。
// 变换与 Ankha 本体一致（y-up translate(0,0.5,-6) * rotate(-90X) * scale(0.02)）。
class FurRenderer {
public:
	FurRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes,
		ID3D12Resource* albedoTex, ID3D12Resource* normalTex,
		ID3D12Resource* fractalWorleyTex, ID3D12Resource* fractalNormalTex);
	~FurRenderer() = default;

	FurRenderer(const FurRenderer&) = delete;
	FurRenderer& operator=(const FurRenderer&) = delete;

	// 每帧实例化绘制 AnkhaFur（8 壳层）。model 为 y-up 世界变换（同 Ankha 本体）。
	void render(const glm::mat4& view, const glm::mat4& proj, const glm::mat4& model,
		const glm::vec3& cameraPos, const glm::vec3& dirLightDir, const glm::vec4& dirLightColor,
		const Mesh& mesh, const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& dsv);

private:
	void createRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps);
	void createDescriptors();
	void createSamplers();

	D3D12Context& _context;

	static constexpr UINT kInstanceCount = 5;   // 参考 AnkhaFur.mati instanceCount=8；本工程降到 5 让最外层壳(0.0030m)≈身体扩张面(0.0030m)，衣服边界不再有清晰在前的壳
	static constexpr float kShellInterval = 0.03f; // 参考 ShellFur.mat shellInterval

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;        // CBV_SRV_UAV 4 desc
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap; // 4 samplers
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;

	ID3D12Resource* _albedoTex = nullptr;
	ID3D12Resource* _normalTex = nullptr;
	ID3D12Resource* _fractalWorleyTex = nullptr;
	ID3D12Resource* _fractalNormalTex = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> _meshCbv;   // b0 FurUniform
	Microsoft::WRL::ComPtr<ID3D12Resource> _globalCbv; // b1 FurGlobalUniform
};
