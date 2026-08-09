#pragma once

#include "D3D12Core.h"
#include "Mesh.h"

#include <string>
#include <vector>

// 用 assimp 加载模型文件（fbx/obj/gltf 等），拆分为多个 Mesh（每个 assimp mesh 一个）。
class Model {
public:
	bool load(D3D12Context& context, const std::string& filePath);

	const std::vector<Mesh>& meshes() const { return _meshes; }
	// 每个 mesh 关联的 assimp 材质名（长度与 meshes() 一致）。
	const std::vector<std::string>& materialNames() const { return _materialNames; }
	bool valid() const { return !_meshes.empty(); }

private:
	std::vector<Mesh> _meshes;
	std::vector<std::string> _materialNames;
};
