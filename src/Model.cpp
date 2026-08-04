#include "Model.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

bool Model::load(D3D12Context& context, const std::string& filePath) {
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(filePath,
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_GenNormals |
		aiProcess_GenUVCoords |
		aiProcess_OptimizeMeshes |
		aiProcess_JoinIdenticalVertices);
	if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
		const char* err = importer.GetErrorString();
		throw std::runtime_error("Failed to load model '" + filePath + "': "
			+ (err ? err : "unknown error"));
	}

	// 展平节点树，收集所有 mesh。
	std::vector<aiMesh*> meshList;
	std::function<void(aiNode*)> walk = [&](aiNode* node) {
		for (unsigned i = 0; i < node->mNumMeshes; ++i) {
			meshList.push_back(scene->mMeshes[node->mMeshes[i]]);
		}
		for (unsigned i = 0; i < node->mNumChildren; ++i) {
			walk(node->mChildren[i]);
		}
	};
	walk(scene->mRootNode);

	_meshes.clear();
	_materialNames.clear();
	_meshes.reserve(meshList.size());
	_materialNames.reserve(meshList.size());

	for (aiMesh* am : meshList) {
		// 记录每个 mesh 的材质名。
		std::string materialName;
		if (am->mMaterialIndex < scene->mNumMaterials) {
			aiString name;
			if (scene->mMaterials[am->mMaterialIndex]->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
				materialName = name.C_Str();
			}
		}
		_materialNames.push_back(materialName);
		std::cerr << "[Model] mesh material: '" << materialName << "'" << std::endl;

		std::vector<MeshVertex> vertices;
		vertices.reserve(am->mNumVertices);
		for (unsigned i = 0; i < am->mNumVertices; ++i) {
			MeshVertex v;
			v.pos = glm::vec3(am->mVertices[i].x, am->mVertices[i].y, am->mVertices[i].z);
			if (am->HasTextureCoords(0)) {
				v.uv = glm::vec2(am->mTextureCoords[0][i].x, am->mTextureCoords[0][i].y);
			} else {
				v.uv = glm::vec2(0.0f, 0.0f);
			}
			if (am->HasNormals()) { // aiProcess_GenNormals 已保证存在
				v.normal = glm::vec3(am->mNormals[i].x, am->mNormals[i].y, am->mNormals[i].z);
			} else {
				v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
			}
			v.color = glm::vec4(1.0f);
			vertices.push_back(v);
		}

		std::vector<std::uint16_t> indices;
		indices.reserve(am->mNumFaces * 3);
		for (unsigned f = 0; f < am->mNumFaces; ++f) {
			const aiFace& face = am->mFaces[f];
			for (unsigned j = 0; j < face.mNumIndices; ++j) {
				indices.push_back(static_cast<std::uint16_t>(face.mIndices[j]));
			}
		}

		Mesh mesh;
		mesh.create(context, vertices, indices);
		_meshes.push_back(std::move(mesh));
	}

	return !_meshes.empty();
}
