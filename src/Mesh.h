#pragma once

#include "D3D12Core.h"

#include <glm/glm.hpp>

#include <wrl/client.h>
#include <cstdint>
#include <vector>

struct MeshVertex {
	glm::vec3 pos;    // offset 0
	glm::vec2 uv;     // offset 12
	glm::vec3 normal; // offset 20
	glm::vec4 color;  // offset 32
};

// 通用网格：顶点/索引数据 + GPU 缓冲。几何由工厂函数生成。
class Mesh {
public:
	void create(D3D12Context& context,
		const std::vector<MeshVertex>& vertices,
		const std::vector<std::uint16_t>& indices);

	D3D12_VERTEX_BUFFER_VIEW vbView() const { return _vbView; }
	D3D12_INDEX_BUFFER_VIEW ibView() const { return _ibView; }
	std::uint32_t indexCount() const { return _indexCount; }
	bool valid() const { return _vertexBuffer != nullptr; }

	// ---- 几何工厂 ----
	static std::vector<MeshVertex> makeCubeVertices();
	static std::vector<std::uint16_t> makeCubeIndices();

	static std::vector<MeshVertex> makeSphereVertices(int stacks, int slices);
	static std::vector<std::uint16_t> makeSphereIndices(int stacks, int slices);

	static std::vector<MeshVertex> makePlaneVertices(float size);
	static std::vector<std::uint16_t> makePlaneIndices();

	static std::vector<MeshVertex> makePrismVertices(); // 三棱柱
	static std::vector<std::uint16_t> makePrismIndices();

private:
	Microsoft::WRL::ComPtr<ID3D12Resource> _vertexBuffer;
	Microsoft::WRL::ComPtr<ID3D12Resource> _indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW _vbView{};
	D3D12_INDEX_BUFFER_VIEW _ibView{};
	std::uint32_t _indexCount = 0;
};
