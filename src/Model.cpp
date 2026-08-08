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
		aiProcess_CalcTangentSpace | // 参考项目 BattleMage addImportFlags(GenerateTangent)
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

		// 诊断：顶点数/UV/包围盒（排查 SkySphere 等模型数据问题）。
		{
			std::cerr << "[Model]   " << am->mNumVertices << " verts, "
				<< am->mNumFaces << " faces, "
				<< (am->HasTextureCoords(0) ? "hasUV" : "NO_UV") << std::endl;
			if (am->HasTextureCoords(0) && am->mNumVertices > 0) {
				glm::vec2 uvMin(1e9f), uvMax(-1e9f);
				glm::vec3 posMin(1e9f), posMax(-1e9f);
				for (unsigned i = 0; i < am->mNumVertices; ++i) {
					const glm::vec2 uv(am->mTextureCoords[0][i].x, am->mTextureCoords[0][i].y);
					uvMin = glm::min(uvMin, uv);
					uvMax = glm::max(uvMax, uv);
					const glm::vec3 p(am->mVertices[i].x, am->mVertices[i].y, am->mVertices[i].z);
					posMin = glm::min(posMin, p);
					posMax = glm::max(posMax, p);
				}
				std::cerr << "[Model]   uv[" << uvMin.x << "," << uvMin.y << "]~["
					<< uvMax.x << "," << uvMax.y << "] bbox[" << posMin.x << "," << posMin.y << ","
					<< posMin.z << "]~[" << posMax.x << "," << posMax.y << "," << posMax.z << "]"
					<< std::endl;
			}
		}

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
		// 切线（参考项目 GenerateTangent；assimp CalcTangentSpace 已生成）。
		if (am->HasTangentsAndBitangents()) {
			v.tangent = glm::vec3(am->mTangents[i].x, am->mTangents[i].y, am->mTangents[i].z);
		} else {
			glm::vec3 t = glm::cross(v.normal, glm::vec3(0.0f, 1.0f, 0.0f));
			if (glm::length(t) < 1e-6f) {
				t = glm::cross(v.normal, glm::vec3(1.0f, 0.0f, 0.0f));
			}
			v.tangent = glm::normalize(t);
		}
		// 顶点色（参考项目 ArcOutline_VS 用 color.b/a 做双因子外扩；EndfieldBody 用 color.x 翻转 bitangent）。
		if (am->HasVertexColors(0)) {
			const aiColor4D& c = am->mColors[0][i];
			v.color = glm::vec4(c.r, c.g, c.b, c.a);
		} else {
			v.color = glm::vec4(1.0f);
		}
		vertices.push_back(v);
		}

		std::vector<std::uint32_t> indices; // 32 位索引：支持 >65535 顶点（Terrain.fbx 26 万顶点）
		indices.reserve(am->mNumFaces * 3);
		for (unsigned f = 0; f < am->mNumFaces; ++f) {
			const aiFace& face = am->mFaces[f];
			for (unsigned j = 0; j < face.mNumIndices; ++j) {
				indices.push_back(face.mIndices[j]);
			}
		}

		Mesh mesh;
		mesh.create(context, vertices, indices);
		_meshes.push_back(std::move(mesh));
	}

	return !_meshes.empty();
}
