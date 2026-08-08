#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <wrl/client.h>
#include <cstdint>
#include <vector>

struct MeshVertex {
	glm::vec3 pos;     // offset 0
	glm::vec2 uv;      // offset 12
	glm::vec3 normal;  // offset 20
	glm::vec4 color;   // offset 32
	glm::vec3 tangent; // offset 48（参考项目 StandardFSInput.worldTangent）
};

// 通用网格：顶点/索引数据 + GPU 缓冲。几何由工厂函数生成。
// 索引 32 位（R32_UINT）：支持 >65535 顶点的大模型（Terrain.fbx 26 万顶点等）。
class Mesh {
public:
	void create(D3D12Context& context,
		const std::vector<MeshVertex>& vertices,
		const std::vector<std::uint32_t>& indices);

	D3D12_VERTEX_BUFFER_VIEW vbView() const { return _vbView; }
	D3D12_INDEX_BUFFER_VIEW ibView() const { return _ibView; }
	std::uint32_t indexCount() const { return _indexCount; }
	bool valid() const { return _vertexBuffer != nullptr; }

	// 模型空间 AABB（参考项目 MeshUniform.minAABB/maxAABB，EndfieldBody_VS 用）。
	glm::vec3 aabbMin() const { return _aabbMin; }
	glm::vec3 aabbMax() const { return _aabbMax; }

	// ---- 几何工厂 ----
	static std::vector<MeshVertex> makeCubeVertices();
	static std::vector<std::uint32_t> makeCubeIndices();

	static std::vector<MeshVertex> makeSphereVertices(int stacks, int slices);
	static std::vector<std::uint32_t> makeSphereIndices(int stacks, int slices);

	static std::vector<MeshVertex> makePlaneVertices(float size);
	static std::vector<std::uint32_t> makePlaneIndices();

	static std::vector<MeshVertex> makePrismVertices(); // 三棱柱
	static std::vector<std::uint32_t> makePrismIndices();

	// 程序化地形网格：平面 xy 网格（z=0），渲染时 VS 采样高度纹理位移 z。
	// 照搬参考项目 Terrain/TerrainVertexInitializer_CS（GridVertex）+ TerrainIndexInitializer_CS（GridIndexPack）。
	static std::vector<MeshVertex> makeTerrainVertices(std::uint32_t sideElements = 256, float interval = 4.0f);
	static std::vector<std::uint32_t> makeTerrainIndices(std::uint32_t sideElements = 256);

private:
	Microsoft::WRL::ComPtr<ID3D12Resource> _vertexBuffer;
	Microsoft::WRL::ComPtr<ID3D12Resource> _indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW _vbView{};
	D3D12_INDEX_BUFFER_VIEW _ibView{};
	std::uint32_t _indexCount = 0;
	glm::vec3 _aabbMin{ 0.0f };
	glm::vec3 _aabbMax{ 0.0f };
};
