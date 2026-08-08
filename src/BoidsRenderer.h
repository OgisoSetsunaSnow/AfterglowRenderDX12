#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

class Mesh;
class ComputePass;

// Boid 群集（照搬参考 BoidInstancing.mat：attribute init + instance init + 每帧 O(N²) sim + instanced draw）。
// 数据流：
//   BoidBuffer(1024) + InstanceBuffer(1024) 两个 StructuredBuffer（UAV 供 compute 写，SRV 供 sim/VS 读）。
//   attribute init（一次性）→ instance init（一次性）→ 每帧 sim（读取旧状态 → 计算 flocking → 写新状态）。
//   渲染：BoidInstancing_VS（SV_InstanceID 读 InstanceBuffer）+ DefaultLit_FS（albedo 白 + DefaultNormal）。
// 根签名（graphics）：b0 MeshUniform + b1 GlobalUniform + VS t0=InstanceBuffer + PS t0-t5 纹理 + s0-s5 sampler。
// z-up 模拟（照搬参考），渲染 model 含 rotate(-90X) 转 y-up。
class BoidsRenderer {
public:
	BoidsRenderer(D3D12Context& context,
		const std::vector<std::uint8_t>& attributeInitCs,
		const std::vector<std::uint8_t>& instanceInitCs,
		const std::vector<std::uint8_t>& simCs,
		const std::vector<std::uint8_t>& vsBytes,
		const std::vector<std::uint8_t>& psBytes, // DefaultLit_FS
		ID3D12Resource* terrainHeightTex,
		ID3D12Resource* albedoTex,   // 白
		ID3D12Resource* normalTex,   // DefaultNormal
		ID3D12Resource* ambientTex); // DefaultSky
	~BoidsRenderer() = default;

	BoidsRenderer(const BoidsRenderer&) = delete;
	BoidsRenderer& operator=(const BoidsRenderer&) = delete;

	// 一次性初始化（attribute + instance）。
	void initialize();

	// 每帧：dispatch sim（写 BoidBuffer/InstanceBuffer UAV）→ 实例化绘制。
	void render(const glm::mat4& view, const glm::mat4& proj,
		const glm::vec3& cameraPos, float time, float deltaTime,
		const Mesh& mesh, const D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
		const D3D12_CPU_DESCRIPTOR_HANDLE& dsv);

private:
	void createBuffers();
	void createComputeRootSignature();
	void createGraphicsRootSignature();
	void createPipelineState(const std::vector<std::uint8_t>& vs,
		const std::vector<std::uint8_t>& ps);
	void createSamplers();
	void updateUniforms(const glm::mat4& view, const glm::mat4& proj,
		const glm::vec3& cameraPos, float time, float deltaTime);

	D3D12Context& _context;
	std::vector<std::uint8_t> _attributeInitCs, _instanceInitCs, _simCs;

	// StructuredBuffer（1024 实例）。
	static constexpr UINT kInstanceCount = 1024;
	struct InstanceBufferElement {
		glm::mat4 instanceModel; // 192B（3 × mat4）
		glm::mat4 model;
		glm::mat4 invTransModel;
	};
	// BoidBuffer：float3+float 交替，HLSL float3 16B 对齐 → 每字段 16B，共 8×16=128B。
	struct BoidBufferElement {
		glm::vec4 acceleration;     // 0-15
		glm::vec4 influenceRadius;  // 16-31
		glm::vec4 maxAcceleration;  // 32-47
		glm::vec4 turnSpeed;        // 48-63
		glm::vec4 velocity;         // 64-79
		glm::vec4 curiosity;        // 80-95
		glm::vec4 maxVelocity;      // 96-111
		glm::vec4 perception;       // 112-127
	};

	Microsoft::WRL::ComPtr<ID3D12Resource> _instanceBuffer;
	Microsoft::WRL::ComPtr<ID3D12Resource> _boidBuffer;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap; // 11 desc（见 createBuffers 注释）
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _samplerHeap;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _computeRootSig;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _graphicsRootSig;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _simPso;   // compute（attribute/instance/sim 共用 root sig，PSO 各一）
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;      // graphics instanced

	ID3D12Resource* _terrainHeightTex = nullptr;
	ID3D12Resource* _albedoTex = nullptr;
	ID3D12Resource* _normalTex = nullptr;
	ID3D12Resource* _ambientTex = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> _simCbv;    // b0（compute：time/deltaTime/model/invTransModel/objectID）
	Microsoft::WRL::ComPtr<ID3D12Resource> _meshCbv;   // b0（graphics：MeshUniform）
	Microsoft::WRL::ComPtr<ID3D12Resource> _frameCbv;  // b1（graphics：GlobalUniform）

	// ComputePass 成员（避免局部变量出作用域时 PSO 被释放导致 #921：list 仍持有引用）。
	std::unique_ptr<ComputePass> _attrPass, _instPass, _simPass;
};
