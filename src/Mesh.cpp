#include "Mesh.h"

#include <d3dx12.h>

#include <array>
#include <cmath>
#include <stdexcept>

#define CHECK_MESH(hr, msg) do { if (FAILED(hr)) throw std::runtime_error(msg); } while (0)

namespace {

constexpr float kPi = 3.14159265358979323846f;

} // namespace

void Mesh::create(D3D12Context& context,
	const std::vector<MeshVertex>& vertices,
	const std::vector<std::uint32_t>& indices) {
	if (vertices.empty() || indices.empty()) {
		throw std::runtime_error("Mesh has empty geometry");
	}

	auto* device = context.device();
	auto* allocator = context.commandAllocator();
	auto* cl = context.commandList();

	const std::uint64_t vbSize = sizeof(MeshVertex) * vertices.size();
	const std::uint64_t ibSize = sizeof(std::uint32_t) * indices.size();

	const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
	const CD3DX12_RESOURCE_DESC vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
	const CD3DX12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);

	CHECK_MESH(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_vertexBuffer)),
		"Failed to create vertex buffer");
	CHECK_MESH(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_indexBuffer)),
		"Failed to create index buffer");

	CHECK_MESH(allocator->Reset(), "Failed to reset allocator");
	CHECK_MESH(cl->Reset(allocator, nullptr), "Failed to reset command list");

	const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
	const CD3DX12_RESOURCE_DESC uploadVbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
	const CD3DX12_RESOURCE_DESC uploadIbDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);

	Microsoft::WRL::ComPtr<ID3D12Resource> vbUpload;
	Microsoft::WRL::ComPtr<ID3D12Resource> ibUpload;
	CHECK_MESH(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadVbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbUpload)),
		"Failed to create VB upload");
	CHECK_MESH(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadIbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ibUpload)),
		"Failed to create IB upload");

	D3D12_SUBRESOURCE_DATA vbData{};
	vbData.pData = vertices.data();
	vbData.RowPitch = static_cast<LONG_PTR>(vbSize);
	vbData.SlicePitch = static_cast<LONG_PTR>(vbSize);
	UpdateSubresources(cl, _vertexBuffer.Get(), vbUpload.Get(), 0, 0, 1, &vbData);

	D3D12_SUBRESOURCE_DATA ibData{};
	ibData.pData = indices.data();
	ibData.RowPitch = static_cast<LONG_PTR>(ibSize);
	ibData.SlicePitch = static_cast<LONG_PTR>(ibSize);
	UpdateSubresources(cl, _indexBuffer.Get(), ibUpload.Get(), 0, 0, 1, &ibData);

	const D3D12_RESOURCE_BARRIER vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
		_vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	cl->ResourceBarrier(1, &vbBarrier);
	const D3D12_RESOURCE_BARRIER ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
		_indexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_INDEX_BUFFER);
	cl->ResourceBarrier(1, &ibBarrier);

	CHECK_MESH(cl->Close(), "Failed to close command list");
	context.submitAndWait();

	_vbView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
	_vbView.SizeInBytes = static_cast<UINT>(vbSize);
	_vbView.StrideInBytes = sizeof(MeshVertex);

	_ibView.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
	_ibView.SizeInBytes = static_cast<UINT>(ibSize);
	_ibView.Format = DXGI_FORMAT_R32_UINT; // 32 位索引，支持 >65535 顶点
	_indexCount = static_cast<std::uint32_t>(indices.size());
}

// ---- 几何工厂 ----

std::vector<MeshVertex> Mesh::makeCubeVertices() {
	// 每面 4 顶点，独立颜色；顶点序 CCW（从外看）。
	const std::array<std::array<std::array<float, 3>, 4>, 6> faces = { {
		{ { { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 } } },
		{ { { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 }, { -1, -1, -1 } } },
		{ { { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 } } },
		{ { { -1, -1, 1 }, { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 } } },
		{ { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } } },
		{ { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } } },
	} };
	// 面法线：与 faces 数组一一对应（+X / -X / +Y / -Y / +Z / -Z）。
	const std::array<glm::vec3, 6> faceNormals = {
		glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
		glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
		glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
	};
	const std::array<std::array<float, 2>, 4> uvs = {
		std::array<float, 2>{ 0, 0 }, std::array<float, 2>{ 1, 0 },
		std::array<float, 2>{ 1, 1 }, std::array<float, 2>{ 0, 1 },
	};
	// 面颜色（会被 baseColor 覆盖，这里留白）。
	const glm::vec4 white(1.0f);

	std::vector<MeshVertex> verts;
	verts.reserve(24);
	for (std::size_t f = 0; f < 6; ++f) {
		for (std::size_t v = 0; v < 4; ++v) {
			MeshVertex vertex;
			vertex.pos = glm::vec3(faces[f][v][0], faces[f][v][1], faces[f][v][2]);
			vertex.uv = glm::vec2(uvs[v][0], uvs[v][1]);
			vertex.normal = faceNormals[f];
			vertex.color = white;
			verts.push_back(vertex);
		}
	}
	return verts;
}

std::vector<std::uint32_t> Mesh::makeCubeIndices() {
	std::vector<std::uint32_t> indices;
	indices.reserve(36);
	for (std::uint32_t f = 0; f < 6; ++f) {
		const std::uint32_t b = f * 4;
		indices.push_back(b + 0);
		indices.push_back(b + 1);
		indices.push_back(b + 2);
		indices.push_back(b + 0);
		indices.push_back(b + 2);
		indices.push_back(b + 3);
	}
	return indices;
}

std::vector<MeshVertex> Mesh::makeSphereVertices(int stacks, int slices) {
	std::vector<MeshVertex> verts;
	verts.reserve(static_cast<std::size_t>((stacks + 1) * (slices + 1)));
	for (int s = 0; s <= stacks; ++s) {
		const float phi = kPi * static_cast<float>(s) / static_cast<float>(stacks);
		for (int sl = 0; sl <= slices; ++sl) {
			const float theta = 2.0f * kPi * static_cast<float>(sl) / static_cast<float>(slices);
			MeshVertex v;
			v.pos = glm::vec3(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
			v.uv = glm::vec2(static_cast<float>(sl) / static_cast<float>(slices),
				static_cast<float>(s) / static_cast<float>(stacks));
			v.normal = v.pos; // 单位球：法线 = 顶点位置
			v.color = glm::vec4(1.0f);
			verts.push_back(v);
		}
	}
	return verts;
}

std::vector<std::uint32_t> Mesh::makeSphereIndices(int stacks, int slices) {
	std::vector<std::uint32_t> indices;
	indices.reserve(static_cast<std::size_t>(stacks) * slices * 6);
	for (int s = 0; s < stacks; ++s) {
		for (int sl = 0; sl < slices; ++sl) {
			const std::uint32_t a = static_cast<std::uint32_t>(s * (slices + 1) + sl);
			const std::uint32_t b = static_cast<std::uint32_t>(a + slices + 1);
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(a + 1);
			indices.push_back(b);
			indices.push_back(b + 1);
			indices.push_back(a + 1);
		}
	}
	return indices;
}

std::vector<MeshVertex> Mesh::makePlaneVertices(float size) {
	const float h = size * 0.5f;
	std::vector<MeshVertex> verts;
	verts.reserve(4);
	// 俯视 CCW：(-h,-h,0) (h,-h,0) (h,h,0) (-h,h,0)
	const std::array<std::array<float, 3>, 4> positions = {
		std::array<float, 3>{ -h, 0.0f, -h },
		std::array<float, 3>{ h, 0.0f, -h },
		std::array<float, 3>{ h, 0.0f, h },
		std::array<float, 3>{ -h, 0.0f, h },
	};
	const std::array<std::array<float, 2>, 4> uvs = {
		std::array<float, 2>{ 0, 0 }, std::array<float, 2>{ 1, 0 },
		std::array<float, 2>{ 1, 1 }, std::array<float, 2>{ 0, 1 },
	};
	for (int i = 0; i < 4; ++i) {
		MeshVertex v;
		v.pos = glm::vec3(positions[i][0], positions[i][1], positions[i][2]);
		v.uv = glm::vec2(uvs[i][0], uvs[i][1]);
		v.normal = glm::vec3(0.0f, 1.0f, 0.0f); // 平面朝 +Y
		v.color = glm::vec4(1.0f);
		verts.push_back(v);
	}
	return verts;
}

std::vector<std::uint32_t> Mesh::makePlaneIndices() {
	return { 0, 1, 2, 0, 2, 3 };
}

std::vector<MeshVertex> Mesh::makePrismVertices() {
	// 三棱柱：6 个独立面（3 侧面 + 顶面 + 底面），每面 4 顶点独立 UV，避免共享顶点导致纹理压缩。
	const float r = 1.0f;
	const float yBottom = 0.0f;
	const float yTop = 1.0f;
	const std::array<glm::vec3, 3> bottomPts = {
		glm::vec3(r * std::cos(0.0f), yBottom, r * std::sin(0.0f)),
		glm::vec3(r * std::cos(2.0f * kPi / 3.0f), yBottom, r * std::sin(2.0f * kPi / 3.0f)),
		glm::vec3(r * std::cos(4.0f * kPi / 3.0f), yBottom, r * std::sin(4.0f * kPi / 3.0f)),
	};
	const std::array<glm::vec3, 3> topPts = {
		glm::vec3(bottomPts[0].x, yTop, bottomPts[0].z),
		glm::vec3(bottomPts[1].x, yTop, bottomPts[1].z),
		glm::vec3(bottomPts[2].x, yTop, bottomPts[2].z),
	};
	const std::array<glm::vec2, 4> faceUV = {
		glm::vec2(0, 0), glm::vec2(1, 0),
		glm::vec2(1, 1), glm::vec2(0, 1),
	};
	std::vector<MeshVertex> verts;
	verts.reserve(24);
	const glm::vec4 white(1.0f);
	auto addQuad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
		const glm::vec3& d, const glm::vec3& n) {
		const glm::vec3 pts[4] = { a, b, c, d };
		for (int i = 0; i < 4; ++i) {
			MeshVertex v;
			v.pos = pts[i];
			v.uv = faceUV[i];
			v.normal = n;
			v.color = white;
			verts.push_back(v);
		}
	};
	// 3 个侧面：从外看 CCW（底i → 底j → 顶j → 顶i），法线水平朝外。
	for (int i = 0; i < 3; ++i) {
		const int j = (i + 1) % 3;
		const glm::vec3 sideNormal = glm::normalize(glm::vec3(
			bottomPts[i].x + bottomPts[j].x, 0.0f, bottomPts[i].z + bottomPts[j].z));
		addQuad(bottomPts[i], bottomPts[j], topPts[j], topPts[i], sideNormal);
	}
	// 顶面：从 +Y 看 CCW（top[0]→top[1]→top[2]，最后一点重复构成 quad）。
	addQuad(topPts[0], topPts[1], topPts[2], topPts[2], glm::vec3(0, 1, 0));
	// 底面：从 -Y 看 CCW（bottom[0]→bottom[2]→bottom[1] 顺序反转，法线朝 -Y）。
	addQuad(bottomPts[0], bottomPts[2], bottomPts[1], bottomPts[1], glm::vec3(0, -1, 0));
	return verts;
}

std::vector<std::uint32_t> Mesh::makePrismIndices() {
	// 6 面 × 6 索引 = 36。
	std::vector<std::uint32_t> indices;
	indices.reserve(36);
	// 3 个侧面（顶点 0-11，每面 4 顶点 quad）。
	for (int f = 0; f < 3; ++f) {
		const std::uint32_t b = static_cast<std::uint32_t>(f * 4);
		indices.push_back(b + 0);
		indices.push_back(b + 1);
		indices.push_back(b + 2);
		indices.push_back(b + 0);
		indices.push_back(b + 2);
		indices.push_back(b + 3);
	}
	// 顶面（顶点 12-15）。
	indices.push_back(12);
	indices.push_back(13);
	indices.push_back(14);
	indices.push_back(12);
	indices.push_back(14);
	indices.push_back(15);
	// 底面（顶点 16-19，顶点序已反转，法线朝 -Y）。
	indices.push_back(16);
	indices.push_back(17);
	indices.push_back(18);
	indices.push_back(16);
	indices.push_back(18);
	indices.push_back(19);
	return indices;
}
