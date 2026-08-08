#include "ACES.h"
#include "Camera.h"
#include "ComputePass.h"
#include "D3D12Core.h"
#include "CharacterRenderer.h"
#include "FreeCamera.h"
#include "FullScreenPass.h"
#include "FractalNoise.h"
#include "BoidsRenderer.h"
#include "GrassRenderer.h"
#include "FurRenderer.h"
#include "Mesh.h"
#include "MeshRenderer.h"
#include "Model.h"
#include "OffscreenRT.h"
#include "PostProcess.h"
#include "ShaderCompiler.h"
#include "TerrainRenderer.h"
#include "Texture.h"
#include "WaterRenderer.h"
#include "Window.h"

#include <d3dx12.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <tuple>

int main() {
	try {
		Window window(L"AfterglowRender DX12", 1280, 720);
		D3D12Context context(window);
		Texture::warmUpMipGenerator(context); // GPU mip generator 早期创建，避免驱动 0x887A0005

		std::cout << "[AfterglowRender DX12] width=" << context.width()
			<< " height=" << context.height() << std::endl;

		// 编译 shader（DXIL）。
		shader::CompileOptions vsOptions;
		vsOptions.entryPoint = L"VSMain";
		vsOptions.targetProfile = L"vs_6_0";
		shader::CompileOptions psOptions;
		psOptions.entryPoint = L"PSMain";
		psOptions.targetProfile = L"ps_6_0";

		const std::string shaderFile = "Shaders/Cube.hlsl";
		const auto vs = shader::compileFromFile(shaderFile, vsOptions);
		const auto ps = shader::compileFromFile(shaderFile, psOptions);
		std::cout << "[AfterglowRender DX12] compiled " << shaderFile
			<< " (vs " << vs.size() << "B, ps " << ps.size() << "B)" << std::endl;

		// 阶段 9：描边 shader。
		const std::string outlineFile = "Shaders/Outline.hlsl";
		const auto outlineVs = shader::compileFromFile(outlineFile, vsOptions);
		const auto outlinePs = shader::compileFromFile(outlineFile, psOptions);
		std::cout << "[AfterglowRender DX12] compiled " << outlineFile
			<< " (vs " << outlineVs.size() << "B, ps " << outlinePs.size() << "B)" << std::endl;

		// 遗留问题 #1：SkySphere 独立 Unlit shader（对齐参考项目 SkySphere.mat = SkySphere_VS + Unlit_FS）。
		const std::string skyFile = "Shaders/SkySphere.hlsl";
		const auto skyVs = shader::compileFromFile(skyFile, vsOptions);
		const auto skyPs = shader::compileFromFile(skyFile, psOptions);
		std::cout << "[AfterglowRender DX12] compiled " << skyFile
			<< " (vs " << skyVs.size() << "B, ps " << skyPs.size() << "B)" << std::endl;

		// 阶段 10：后处理 shader（全屏 pass）。
		const std::string postFile = "Shaders/PostProcess.hlsl";
		const auto postVs = shader::compileFromFile(postFile, vsOptions);
		const auto postPs = shader::compileFromFile(postFile, psOptions);
		std::cout << "[AfterglowRender DX12] compiled " << postFile
			<< " (vs " << postVs.size() << "B, ps " << postPs.size() << "B)" << std::endl;

		// 阶段 10b：Bloom 三 pass（DownSampling + HorizontalBlur + VerticalBlur）。
		struct BloomShaders {
			std::vector<std::uint8_t> vs;
			std::vector<std::uint8_t> ps;
		};
		auto compileFS = [&](const char* file) {
			shader::CompileOptions localVs = vsOptions;
			shader::CompileOptions localPs = psOptions;
			BloomShaders s;
			s.vs = shader::compileFromFile(file, localVs);
			s.ps = shader::compileFromFile(file, localPs);
			std::cout << "[AfterglowRender DX12] compiled " << file
				<< " (vs " << s.vs.size() << "B, ps " << s.ps.size() << "B)" << std::endl;
			return s;
		};
		BloomShaders downSh = compileFS("Shaders/DownSampling.hlsl");
		BloomShaders horzSh = compileFS("Shaders/HorizontalBlur.hlsl");
		BloomShaders vertSh = compileFS("Shaders/VerticalBlur.hlsl");

		// compute pipeline 基础设施验证：噪声生成 CS（cs_6_0）。
		shader::CompileOptions csOptions;
		csOptions.entryPoint = L"main";
		csOptions.targetProfile = L"cs_6_0";
		const std::string computeFile = "Shaders/ComputeNoise.hlsl";
		const auto noiseCs = shader::compileFromFile(computeFile, csOptions);
		std::cout << "[AfterglowRender DX12] compiled " << computeFile
			<< " (cs " << noiseCs.size() << "B)" << std::endl;

		// 程序化地形：高度生成 CS + 地形渲染 VS/FS。
		// 高度生成照搬参考项目 Terrain/TerrainHeightInitializer_CS.hlsl（fBm+Worley/Perlin/Simplex 噪声）。
		const std::string terrainFile = "Shaders/Terrain.hlsl";
		const auto terrainVs = shader::compileFromFile(terrainFile, vsOptions);
		const auto terrainPs = shader::compileFromFile(terrainFile, psOptions);
		std::cout << "[AfterglowRender DX12] compiled " << terrainFile
			<< " (vs " << terrainVs.size() << "B, ps " << terrainPs.size() << "B)" << std::endl;
		const std::string terrainHeightFile = "Shaders/TerrainHeight_CS.hlsl";
		const auto terrainHeightCs = shader::compileFromFile(terrainHeightFile, csOptions);
		std::cout << "[AfterglowRender DX12] compiled " << terrainHeightFile
			<< " (cs " << terrainHeightCs.size() << "B)" << std::endl;
		const std::string terrainNormalFile = "Shaders/TerrainNormal_CS.hlsl";
		const auto terrainNormalCs = shader::compileFromFile(terrainNormalFile, csOptions);
		std::cout << "[AfterglowRender DX12] compiled " << terrainNormalFile
			<< " (cs " << terrainNormalCs.size() << "B)" << std::endl;
		const std::string terrainSurfaceFile = "Shaders/TerrainSurface_CS.hlsl";
		const auto terrainSurfaceCs = shader::compileFromFile(terrainSurfaceFile, csOptions);
		std::cout << "[AfterglowRender DX12] compiled " << terrainSurfaceFile
			<< " (cs " << terrainSurfaceCs.size() << "B)" << std::endl;
	// Meteorograph：拆为 Init（一次性）+ Sim（每帧）+ Pluviometer Init（照搬参考 Meteorograph.mat computeTask）。
	const std::string meteorographInitFile = "Shaders/MeteorographInitializer_CS.hlsl";
	const auto meteorographInitCs = shader::compileFromFile(meteorographInitFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << meteorographInitFile
		<< " (cs " << meteorographInitCs.size() << "B)" << std::endl;
	const std::string meteorographFile = "Shaders/Meteorograph_CS.hlsl"; // 每帧风迭代/湿度/降雨
	const auto meteorographCs = shader::compileFromFile(meteorographFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << meteorographFile
		<< " (cs " << meteorographCs.size() << "B)" << std::endl;
	const std::string pluviometerInitFile = "Shaders/PluviometerInitializer_CS.hlsl";
	const auto pluviometerInitCs = shader::compileFromFile(pluviometerInitFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << pluviometerInitFile
		<< " (cs " << pluviometerInitCs.size() << "B)" << std::endl;
	// TerrainData 一次性水文模拟（浅水方程/侵蚀/蒸发，照搬参考 Terrain/TerrainData_CS.hlsl。
	// 参考 TerrainData.mat dispatchFrequency=Once：初始化后只跑一次，不每帧）。
	const std::string terrainDataFile = "Shaders/TerrainData_CS.hlsl";
	const auto terrainDataCs = shader::compileFromFile(terrainDataFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << terrainDataFile
		<< " (cs " << terrainDataCs.size() << "B)" << std::endl;
	// Chromatics：一次性烘焙 32^3 3D LUT（照搬参考 Chromatics.mat computeTask，ACES RRT+ODT）。
	const std::string chromaticsFile = "Shaders/Chromatics/Chromatics_CS.hlsl";
	const auto chromaticsCs = shader::compileFromFile(chromaticsFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << chromaticsFile
		<< " (cs " << chromaticsCs.size() << "B)" << std::endl;
	// 水面（照搬参考 WaterMesh.mat：Water_VS + Water_FS）。
	const std::string waterFile = "Shaders/Water_VS.hlsl";
	const std::string waterFsFile = "Shaders/Water_FS.hlsl";
	const auto waterVs = shader::compileFromFile(waterFile, vsOptions);
	const auto waterPs = shader::compileFromFile(waterFsFile, psOptions);
	std::cout << "[AfterglowRender DX12] compiled " << waterFile
		<< " (vs " << waterVs.size() << "B) + " << waterFsFile
		<< " (ps " << waterPs.size() << "B)" << std::endl;
	// FractalSimplexNoise 初始化（照搬参考 FractalNoise.mat FractalSimplexNoiseInitializer_CS）。
	const std::string fractalInitFile = "Shaders/FractalSimplexNoiseInitializer_CS.hlsl";
	const auto fractalInitCs = shader::compileFromFile(fractalInitFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << fractalInitFile
		<< " (cs " << fractalInitCs.size() << "B)" << std::endl;
	// FractalNoise 完整数据流（照搬参考 FractalNoise.mat）：Worley + Normal（水面只用 Simplex）。
	const std::string fractalWorleyFile = "Shaders/FractalWorleyNoiseInitializer_CS.hlsl";
	const auto fractalWorleyCs = shader::compileFromFile(fractalWorleyFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << fractalWorleyFile
		<< " (cs " << fractalWorleyCs.size() << "B)" << std::endl;
	const std::string fractalNoiseFile = "Shaders/FractalNoise_CS.hlsl";
	const auto fractalNoiseCs = shader::compileFromFile(fractalNoiseFile, csOptions);
	std::cout << "[AfterglowRender DX12] compiled " << fractalNoiseFile
		<< " (cs " << fractalNoiseCs.size() << "B)" << std::endl;
	// Boids（照搬参考 BoidInstancing.mat：attribute/instance init + 每帧 sim + instanced draw）。
	const std::string boidAttrInitFile = "Shaders/Boid/BoidAttributeInitializer_CS.hlsl";
	const std::string boidInstInitFile = "Shaders/Boid/BoidInstanceInitializer_CS.hlsl";
	const std::string boidSimFile = "Shaders/Boid/BoidInstancing_CS.hlsl";
	const std::string boidVsFile = "Shaders/Boid/BoidInstancing_VS.hlsl";
	const auto boidAttrInitCs = shader::compileFromFile(boidAttrInitFile, csOptions);
	const auto boidInstInitCs = shader::compileFromFile(boidInstInitFile, csOptions);
	const auto boidSimCs = shader::compileFromFile(boidSimFile, csOptions);
	const auto boidVs = shader::compileFromFile(boidVsFile, vsOptions);
	const auto boidPs = shader::compileFromFile("Shaders/DefaultLit_FS.hlsl", psOptions); // 参考 BoidInstancing.mat fragmentShaderPath
	std::cout << "[AfterglowRender DX12] compiled Boids: " << boidAttrInitFile << " ("
		<< boidAttrInitCs.size() << "B) " << boidInstInitFile << " (" << boidInstInitCs.size()
		<< "B) " << boidSimFile << " (" << boidSimCs.size() << "B) " << boidVsFile << " (vs "
		<< boidVs.size() << "B)" << std::endl;
	// Grass（照搬参考 GrassData.mat + GrassInstancing.mat：GrassData 纹理 + 每帧实例生成 + ExecuteIndirect）。
	const std::string grassDataFile = "Shaders/Grass/GrassData_CS.hlsl";
	const std::string grassInstFile = "Shaders/Grass/GrassInstancing_CS.hlsl";
	const std::string grassVsFile = "Shaders/Grass/GrassInstancing_VS.hlsl";
	const std::string grassFsFile = "Shaders/Grass/GrassInstancing_FS.hlsl";
	const auto grassDataCs = shader::compileFromFile(grassDataFile, csOptions);
	const auto grassInstCs = shader::compileFromFile(grassInstFile, csOptions);
	const auto grassVs = shader::compileFromFile(grassVsFile, vsOptions);
	const auto grassPs = shader::compileFromFile(grassFsFile, psOptions);
	std::cout << "[AfterglowRender DX12] compiled Grass: " << grassDataFile << " ("
		<< grassDataCs.size() << "B) " << grassInstFile << " (" << grassInstCs.size()
		<< "B) " << grassVsFile << " (vs " << grassVs.size() << "B) " << grassFsFile
		<< " (ps " << grassPs.size() << "B)" << std::endl;
	// ShellFur（照搬参考 ShellFur.mat + AnkhaFur.mati：8 壳层实例化毛发生成）。
	const std::string furVsFile = "Shaders/ShellFur/ShellFur_VS.hlsl";
	const std::string furFsFile = "Shaders/ShellFur/ShellFur_FS.hlsl";
	const auto furVs = shader::compileFromFile(furVsFile, vsOptions);
	const auto furPs = shader::compileFromFile(furFsFile, psOptions);
	std::cout << "[AfterglowRender DX12] compiled ShellFur: " << furVsFile << " (vs "
		<< furVs.size() << "B) " << furFsFile << " (ps " << furPs.size() << "B)" << std::endl;
	// ShellFurMask_VS（debug 版 Ankha.mat 的 mask 顶点着色器：Ankha 身体用）。
	const auto shellFurMaskVs = shader::compileFromFile("Shaders/ShellFur/ShellFurMask_VS.hlsl", vsOptions);
	std::cout << "[AfterglowRender DX12] compiled Shaders/ShellFur/ShellFurMask_VS.hlsl (vs "
		<< shellFurMaskVs.size() << "B)" << std::endl;

	// 角色材质 shader（照搬参考项目 DefaultLit_VS/Endfield*_VS + 各材质 FS + 描边）。
	// 材质专用 VS：EndfieldBody_VS（normalized position → color.xyz）、EndfieldIris_VS（color.x=tangent.x）、
	// EndfieldEyelash_VS（depth offset）、EndfieldHairShadow_VS（bias + depth bias）、UnlitVS（EyeShadow）。
	const std::string characterVsFile = "Shaders/CharacterVS.hlsl"; // DefaultLit_VS 移植
	const auto characterVs = shader::compileFromFile(characterVsFile, vsOptions);
	std::cout << "[AfterglowRender DX12] compiled " << characterVsFile
		<< " (vs " << characterVs.size() << "B)" << std::endl;
	const auto endfieldBodyVs = shader::compileFromFile("Shaders/EndfieldBody_VS.hlsl", vsOptions);
	const auto endfieldIrisVs = shader::compileFromFile("Shaders/EndfieldIris_VS.hlsl", vsOptions);
	const auto endfieldEyelashVs = shader::compileFromFile("Shaders/EndfieldEyelash_VS.hlsl", vsOptions);
	const auto endfieldHairShadowVs = shader::compileFromFile("Shaders/EndfieldHairShadow_VS.hlsl", vsOptions);
	const auto unlitVs = shader::compileFromFile("Shaders/UnlitVS.hlsl", vsOptions);
	// 每材质 VS（顺序 = CharacterMaterial enum）。
	const std::vector<std::vector<std::uint8_t>> characterVsList = {
		characterVs,        // ArcToon → DefaultLit_VS
		endfieldBodyVs,     // EndfieldBody → EndfieldBody_VS
		characterVs,        // EndfieldPBR → DefaultLit_VS
		characterVs,        // EndfieldThinFilm → DefaultLit_VS
		characterVs,        // EndfieldFace → DefaultLit_VS
		characterVs,        // EndfieldHair → DefaultLit_VS
		endfieldIrisVs,     // EndfieldIris → EndfieldIris_VS
		endfieldEyelashVs,  // EndfieldEyelash → EndfieldEyelash_VS
		unlitVs,            // EndfieldEyeShadow → Unlit_VS
		endfieldHairShadowVs, // EndfieldHairShadow → EndfieldHairShadow_VS
		characterVs,        // DefaultLit → DefaultLit_VS
		shellFurMaskVs,     // AnkhaMask → ShellFurMask_VS（debug 版 Ankha 身体 mask）
	};
	const std::vector<std::vector<std::uint8_t>> characterFsList = {
		shader::compileFromFile("Shaders/ArcToon_FS.hlsl", psOptions),           // 0 ArcToon
		shader::compileFromFile("Shaders/EndfieldBody_FS.hlsl", psOptions),      // 1 EndfieldBody
		shader::compileFromFile("Shaders/EndfieldPBR_FS.hlsl", psOptions),       // 2 EndfieldPBR
		shader::compileFromFile("Shaders/EndfieldPBR_FS.hlsl", psOptions),       // 3 EndfieldThinFilm（同 PBR_FS）
		shader::compileFromFile("Shaders/EndfieldFace_FS.hlsl", psOptions),      // 4 EndfieldFace
		shader::compileFromFile("Shaders/EndfieldHair_FS.hlsl", psOptions),      // 5 EndfieldHair
		shader::compileFromFile("Shaders/EndfieldIris_FS.hlsl", psOptions),      // 6 EndfieldIris
		shader::compileFromFile("Shaders/EndfieldEyelash_FS.hlsl", psOptions),   // 7 EndfieldEyelash
		shader::compileFromFile("Shaders/EndfieldEyeShadow_FS.hlsl", psOptions), // 8 EndfieldEyeShadow
		shader::compileFromFile("Shaders/EndfieldHairShadow_FS.hlsl", psOptions),// 9 EndfieldHairShadow
		shader::compileFromFile("Shaders/DefaultLit_FS.hlsl", psOptions),        // 10 DefaultLit
		shader::compileFromFile("Shaders/DefaultLit_FS.hlsl", psOptions),        // 11 AnkhaMask（同 DefaultLit_FS）
	};
	// 每材质 PSO 配置（参考项目 .mat：cullMode 0=None 2=Back；Transparency 域 blend + 不写深度）。
	const std::vector<CharacterMaterialDesc> characterMaterialDescs = {
		{ D3D12_CULL_MODE_BACK, true, false },  // ArcToon（ArcToon.mat cullMode 2）
		{ D3D12_CULL_MODE_BACK, true, false },  // EndfieldBody（cullMode 2）
		{ D3D12_CULL_MODE_NONE, true, false },  // EndfieldPBR（cullMode 0 双面）
		{ D3D12_CULL_MODE_BACK, false, true },  // EndfieldThinFilm（Transparency）
		{ D3D12_CULL_MODE_BACK, true, false },  // EndfieldFace（cullMode 2）
		{ D3D12_CULL_MODE_BACK, true, false },  // EndfieldHair（cullMode 2）
		{ D3D12_CULL_MODE_BACK, true, false },  // EndfieldIris（cullMode 2）
		{ D3D12_CULL_MODE_BACK, false, true },  // EndfieldEyelash（Transparency, depthWrite false）
		{ D3D12_CULL_MODE_BACK, false, true },  // EndfieldEyeShadow（Transparency, depthWrite false）
		{ D3D12_CULL_MODE_BACK, false, true },  // EndfieldHairShadow（Transparency, depthWrite false）
		{ D3D12_CULL_MODE_BACK, true, false },  // DefaultLit（cullMode 2）
		{ D3D12_CULL_MODE_NONE, true, false },  // AnkhaMask（debug Ankha.mat cullMode 0 双面）
	};
	const std::vector<std::vector<std::uint8_t>> outlineVsList = {
		shader::compileFromFile("Shaders/ArcOutline_VS.hlsl", vsOptions),
		shader::compileFromFile("Shaders/EndfieldOutline_VS.hlsl", vsOptions),
	};
	const std::vector<std::vector<std::uint8_t>> outlineFsList = {
		shader::compileFromFile("Shaders/ArcOutline_FS.hlsl", psOptions),
		shader::compileFromFile("Shaders/EndfieldOutline_FS.hlsl", psOptions),
	};
	for (std::size_t i = 0; i < characterFsList.size(); ++i) {
		std::cout << "[AfterglowRender DX12] character FS[" << i << "] " << characterFsList[i].size() << "B" << std::endl;
	}

		Texture texture;
		texture.load(context, "Textures/checker.png");
		std::cout << "[AfterglowRender DX12] loaded texture "
			<< texture.width() << "x" << texture.height() << std::endl;

		Texture battleMageBody;
		battleMageBody.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Color01/19A_Body_base.png");
		std::cout << "[AfterglowRender DX12] loaded BattleMage body texture "
			<< battleMageBody.width() << "x" << battleMageBody.height() << std::endl;

		Texture battleMageDecal;
		battleMageDecal.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Color01/19A_Body_Decal.png");
		std::cout << "[AfterglowRender DX12] loaded BattleMage decal texture "
			<< battleMageDecal.width() << "x" << battleMageDecal.height() << std::endl;

		// Box 用 ArcToon 武器材质（参考 BattleMageWeapon.mati，W00 系列纹理）。
		Texture boxAlbedo, boxSss, boxIlm;
		boxAlbedo.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Color01/19A_Body_W00_base.png");
		boxSss.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Color01/19A_Body_W00_Sss.png");
		boxIlm.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Base/19A_Body_W00_ilm.png");
		std::cout << "[AfterglowRender DX12] loaded Box weapon textures "
			<< boxAlbedo.width() << "x" << boxAlbedo.height() << std::endl;

		// SkySphere 天空贴图（参考项目 Shared/Textures/DefaultSky.png）。
		Texture skyTexture;
		skyTexture.load(context, "../vsbuild/Assets/Shared/Textures/DefaultSky.png");
		std::cout << "[AfterglowRender DX12] loaded sky texture "
			<< skyTexture.width() << "x" << skyTexture.height() << std::endl;

		// 角色材质共用：DefaultSky（IBL 环境贴图，参考项目 ambientTexture）。
		Texture defaultSkyTex;
		defaultSkyTex.load(context, "../vsbuild/Assets/Shared/Textures/DefaultSky.png");

		// 角色材质：BattleMage 额外纹理（参考 ArcToon.mat 需要 sss + ilm 3 纹理）。
		Texture battleMageSss, battleMageIlm;
		battleMageSss.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Color01/19A_Body_Sss.png");
		battleMageIlm.load(context, "../vsbuild/Assets/Characters/BattleMage/Textures/Base/19A_Body_ilm.png");
		std::cout << "[AfterglowRender DX12] loaded BattleMage sss/ilm for ArcToon material "
			<< battleMageSss.width() << "x" << battleMageSss.height() << std::endl;

		// 角色材质：占位纹理（缺纹理槽用黑色/白色避免非法 SRV）。
		Texture placeholderBlack, placeholderWhite;
		placeholderBlack.load(context, "Textures/black.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		placeholderWhite.load(context, "Textures/white.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		// Standard 材质默认法线（参考项目 Shared/Textures/DefaultNormal.png，4x4 中性法线）。
		Texture defaultNormalTex;
		defaultNormalTex.load(context, "Textures/DefaultNormal.png", DXGI_FORMAT_R8G8B8A8_UNORM);

		// Yvonne（第二个角色）：albedo 纹理（参考项目 Yvonne*.mati 的 albedoTex 引用）。
		Texture yvonneBody, yvonneCloth1, yvonneCloth2, yvonneFace, yvonneIris, yvonneHair;
		const char* yvTexDir = "../vsbuild/Assets/Characters/Yvonne/Textures/";
		yvonneBody.load(context, std::string(yvTexDir) + "T_actor_yvonne_body_01_D.png");   // mesh 0 Body / 1 Neck
		yvonneCloth1.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_01_D.png"); // mesh 2 Cloth / 4 ThinFilm / 5 Cloth
		yvonneCloth2.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_02_D.png"); // mesh 3 Tail
		yvonneFace.load(context, std::string(yvTexDir) + "T_actor_yvonne_face_01_D.png");    // mesh 7 Face / 8 Eyelash
		yvonneIris.load(context, std::string(yvTexDir) + "T_actor_yvonne_iris_01_D.png");    // mesh 9 Iris
		yvonneHair.load(context, std::string(yvTexDir) + "T_actor_yvonne_hair_01_D.png");    // mesh 10 Hair
		std::cout << "[AfterglowRender DX12] loaded Yvonne textures "
			<< yvonneBody.width() << "x" << yvonneBody.height()
			<< " / " << yvonneHair.width() << "x" << yvonneHair.height() << std::endl;

		// Yvonne 其余材质纹理（照搬 Yvonne*.mati 的 texture 引用；法线/property/st 等线性数据用 UNORM）。
		const char* yvComDir = "../vsbuild/Assets/Characters/Yvonne/Textures/Common/";
		Texture yvBodyN, yvCloth1N, yvCloth2N, yvCloth1P, yvCloth2P, yvCloth1M, yvCloth5RS;
		Texture yvHairHN, yvHairP, yvHairST;
		Texture yvBodyRD, yvFaceRD, yvFaceHlM, yvFaceSDF, yvFaceCmM, yvHairRD, yvMatcap05, yvMatcap07;
		yvBodyN.load(context, std::string(yvTexDir) + "T_actor_yvonne_body_01_N.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvCloth1N.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_01_N.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvCloth2N.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_02_N.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvCloth1P.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_01_P.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvCloth2P.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_02_P.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvCloth1M.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_01_M.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvCloth5RS.load(context, std::string(yvTexDir) + "T_actor_yvonne_cloth_05_RS.png");
		yvHairHN.load(context, std::string(yvTexDir) + "T_actor_yvonne_hair_01_HN.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvHairP.load(context, std::string(yvTexDir) + "T_actor_yvonne_hair_01_P.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvHairST.load(context, std::string(yvTexDir) + "T_actor_yvonne_hair_01_ST.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvBodyRD.load(context, std::string(yvComDir) + "T_actor_common_body_01_RD.png");
		yvFaceRD.load(context, std::string(yvComDir) + "T_actor_common_face_01_RD.png");
		yvFaceHlM.load(context, std::string(yvComDir) + "T_actor_common_face_01_hl_M.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvFaceSDF.load(context, std::string(yvComDir) + "T_actor_common_female_face_01_SDF.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvFaceCmM.load(context, std::string(yvComDir) + "T_actor_common_female_face_01_cm_M.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		yvHairRD.load(context, std::string(yvComDir) + "T_actor_common_hair_01_RD.png");
		yvMatcap05.load(context, std::string(yvComDir) + "T_actor_common_matcap_05_D.png");
		yvMatcap07.load(context, std::string(yvComDir) + "T_actor_common_matcap_07_D.png");
		std::cout << "[AfterglowRender DX12] loaded Yvonne material textures (N/P/ST/RD/SDF/M/MC)" << std::endl;

		// Ankha（第三个角色）：albedo 纹理（参考项目 Ankha*.mati 的 albedoTex 引用）。
		Texture ankhaBody, ankhaDress, ankhaEye, ankhaHair;
		const char* ankTexDir = "../vsbuild/Assets/Characters/Ankha/Textures/";
		ankhaBody.load(context, std::string(ankTexDir) + "BODY TEXTURE.png");        // mesh 1/2/3/5/6/8/10
		ankhaDress.load(context, std::string(ankTexDir) + "DRESS TEXTURE.png");      // mesh 0
		ankhaEye.load(context, std::string(ankTexDir) + "EYE TEXTURE.png");          // mesh 4/7
		ankhaHair.load(context, std::string(ankTexDir) + "HAIR SKATER TEXTURE.png"); // mesh 9
		std::cout << "[AfterglowRender DX12] loaded Ankha textures "
			<< ankhaBody.width() << "x" << ankhaBody.height() << std::endl;

		// Ankha 法线贴图（参考项目 DefaultLit 材质 normalTex；线性数据 UNORM）。
		Texture ankhaBodyN, ankhaDressN, ankhaHairN;
		ankhaBodyN.load(context, std::string(ankTexDir) + "BODY NORMALS.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		ankhaDressN.load(context, std::string(ankTexDir) + "DRESS NORMALS.png", DXGI_FORMAT_R8G8B8A8_UNORM);
		ankhaHairN.load(context, std::string(ankTexDir) + "HAIR NORMALS.png", DXGI_FORMAT_R8G8B8A8_UNORM);

		// 共享 SRV/UAV 堆：D3D12 同一时刻只能设置一个 CBV_SRV_UAV 堆，所有 RT 的 SRV/UAV 放一起。
		// PostProcess 根签名 t0-t6 绑 base 0..6（连续 7 个），所以 PostProcess 用的 7 个描述符必须物理连续。
		// index 0=offscreen(scene), 1=combined[0](最终 bloom), 2=depth, 3=noise SRV(compute),
		// 4=ambient(DefaultSky IBL), 5=TerrainHeight(PostProcess UnderwaterMask),
		// 6=ColorLUT(Chromatics 32^3 3D LUT, PostProcess LUTMapping 采样),
		// 7-12=down[0..5], 13-18=horz[0..5], 19-23=combined[1..5], 24=noise UAV(compute 写)。
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sharedSrvHeap;
		D3D12_DESCRIPTOR_HEAP_DESC sharedDesc{};
		sharedDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		sharedDesc.NumDescriptors = 25;
		sharedDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(context.device()->CreateDescriptorHeap(&sharedDesc, IID_PPV_ARGS(&sharedSrvHeap)))) {
			throw std::runtime_error("Failed to create shared SRV heap");
		}

		// ---- 创建网格 ----
		std::vector<Mesh> meshes(4);
		meshes[0].create(context, Mesh::makeCubeVertices(), Mesh::makeCubeIndices());
		meshes[1].create(context, Mesh::makeSphereVertices(16, 24), Mesh::makeSphereIndices(16, 24));
		meshes[2].create(context, Mesh::makePlaneVertices(10.0f), Mesh::makePlaneIndices());
		meshes[3].create(context, Mesh::makePrismVertices(), Mesh::makePrismIndices());

		MeshRenderer meshRenderer(context, vs, ps,
			{ &texture, &battleMageBody, &battleMageDecal, &skyTexture,
			  &yvonneBody, &yvonneCloth1, &yvonneCloth2, &yvonneFace, &yvonneIris, &yvonneHair,
			  &ankhaBody, &ankhaDress, &ankhaEye, &ankhaHair },
			256, outlineVs, outlinePs,
			DXGI_FORMAT_R16G16B16A16_FLOAT, sharedSrvHeap.Get(), 2, skyVs, skyPs); // HUD offscreen + depth SRV(index 2) + SkySphere

		// 阶段 8：参考项目方向光（z-up AfterglowRender）setGlobalEuler({45°, 45°, 30°})，
		// 颜色 0xFFEEDD00，强度 intPi（≈3.14159）。转换到 y-up：方向近似 (-0.5, 0.7, 0.4)（左侧上方）。
		const glm::vec3 dirLightColor = glm::vec3(1.0f, 0xEE / 255.0f, 0xDD / 255.0f); // 0xFFEEDD00
		const float dirLightIntensity = 3.14159265f; // π，对齐参考项目
		const glm::vec3 sceneLightDir = glm::normalize(glm::vec3(-0.5f, 0.7f, 0.4f));
		meshRenderer.setLight(sceneLightDir,
			dirLightColor * dirLightIntensity, // MeshRenderer 根签名 w=1.0 硬编码，强度烘到 color
			glm::vec3(0.12f, 0.12f, 0.15f));

		// 阶段 10：离屏 sceneColor + 后处理。
		OffscreenRT offscreen;
		offscreen.create(context, 0, 0, sharedSrvHeap.Get(), 0, nullptr);
		PostProcess postProcess(context, postVs, postPs);

		// 阶段 10b：Bloom 6 级金字塔（照搬参考 AfterglowBloomPassSet：6 降采样 1/2→1/64 + 6 水平/垂直组合）。
		// down[0]=1/2 ... down[5]=1/64；intensity 分级 {0.175,0.07,0.06,0.03,0.03,0.02}（最小级最强）。
		const UINT halfW = context.width() / 2;
		const UINT halfH = context.height() / 2;
		const float bloomClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		std::array<OffscreenRT, 6> down, horz, combined;
		for (UINT i = 0; i < 6; ++i) {
			const UINT w = (context.width() >> (i + 1)) ? (context.width() >> (i + 1)) : 1;
			const UINT h = (context.height() >> (i + 1)) ? (context.height() >> (i + 1)) : 1;
			down[i].create(context, w, h, sharedSrvHeap.Get(), 7 + i, bloomClear);
			horz[i].create(context, w, h, sharedSrvHeap.Get(), 13 + i, bloomClear);
		}
		// combined[0] = 最终 bloom（PostProcess t1 固定 index 1）；combined[1..5] 放 19-23
		// （避开 noise UAV index 24——否则 combined[5] SRV 会被 noise UAV 描述符覆盖，bloom 最小级采样到纯红噪声）。
		combined[0].create(context, context.width() >> 1, context.height() >> 1, sharedSrvHeap.Get(), 1, bloomClear);
		for (UINT i = 1; i < 6; ++i) {
			const UINT w = (context.width() >> (i + 1)) ? (context.width() >> (i + 1)) : 1;
			const UINT h = (context.height() >> (i + 1)) ? (context.height() >> (i + 1)) : 1;
			combined[i].create(context, w, h, sharedSrvHeap.Get(), 18 + i, bloomClear);
		}
		// 每级分辨率（供 viewport/clear）。
		const std::array<UINT, 6> bloomW = {
			context.width() >> 1, context.width() >> 2, context.width() >> 3,
			context.width() >> 4, context.width() >> 5, (context.width() >> 6) ? (context.width() >> 6) : 1 };
		const std::array<UINT, 6> bloomH = {
			context.height() >> 1, context.height() >> 2, context.height() >> 3,
			context.height() >> 4, context.height() >> 5, (context.height() >> 6) ? (context.height() >> 6) : 1 };
		// 每级 intensity：down[i]（1/2^(i+1)）对应参考 sequenceID=downSamplingCount-log2(1/scale)=5-i。
		// intensity[5-i]：down[0]=1/2→0.02, down[1]=1/4→0.03, ..., down[5]=1/64→0.175。
		const std::array<glm::vec4, 6> bloomIntensity = {
			glm::vec4(0.02f, 0.02f, 0.02f, 0.0f),   // down[0] 1/2
			glm::vec4(0.03f, 0.03f, 0.03f, 0.0f),   // down[1] 1/4
			glm::vec4(0.03f, 0.03f, 0.03f, 0.0f),   // down[2] 1/8
			glm::vec4(0.06f, 0.06f, 0.06f, 0.0f),   // down[3] 1/16
			glm::vec4(0.07f, 0.07f, 0.07f, 0.0f),   // down[4] 1/32
			glm::vec4(0.175f, 0.175f, 0.175f, 0.0f),// down[5] 1/64
		};

		// compute pipeline: noise UAV texture (256x256, SRV=3 / UAV=24) + ComputePass.
		OffscreenRT noiseUav;
		noiseUav.createUav(context, 256, 256, sharedSrvHeap.Get(), 3, 24);
		ComputePass noiseCompute(context, noiseCs);

		// PostProcess t4 = ambient（DefaultSky IBL，envColor 采样）。写到共享堆 index 4。
		{
			auto* device = context.device();
			const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += 4 * inc;
			skyTexture.createSrv(device, cpu);
		}

		// ---- Chromatics：ACES 表 + 一次性烘焙 32^3 3D LUT（照搬参考 Chromatics.mat computeTask dispatchFrequency=Once）----
		// ACES 表由 C++ 生成（参考 initMode=InternalFunction 的 aces::OutputTransformContext()）。
		const aces::GeneratedTables acesTables = aces::GenerateOutputTransformTables();
		std::cout << "[AfterglowRender DX12] ACES tables generated (params "
			<< sizeof(acesTables.params) << "B, tables " << acesTables.reachM.size() << ")" << std::endl;
		// sanity：打印关键参数 + NaN 检查（诊断 LUT 全黑）。
		{
			bool anyNaN = false;
			for (const auto& v : acesTables.reachM) if (std::isnan(v.x)) anyNaN = true;
			for (const auto& v : acesTables.upperHullGamma) if (std::isnan(v.x)) anyNaN = true;
			for (const auto& v : acesTables.gamutCusps) if (std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z)) anyNaN = true;
			std::cout << "[ACES] offsetof(inputJMh_rgbToCam16c)="
				<< offsetof(aces::OutputTransformParamsSSBO, inputJMh_rgbToCam16c)
				<< " sizeof=" << sizeof(acesTables.params) << std::endl;
			const auto& m0 = acesTables.params.inputJMh_rgbToCam16c;
			const auto& m1 = acesTables.params.inputJMh_coneResponseToAab;
			const auto& m2 = acesTables.params.limitJMh_cam16cToRGB;
			const auto& m3 = acesTables.params.limitJMh_aabToConeResponse;
			std::cout << "[ACES] peak=" << acesTables.params.peakLuminance
				<< " limitJMax=" << acesTables.params.limitJMax
				<< " invModelGamma=" << acesTables.params.invModelGamma
				<< " sat=" << acesTables.params.saturation
				<< " chromaComp=" << acesTables.params.chromaCompression
				<< " midJ=" << acesTables.params.midJ
				<< " | reachM[180]=" << acesTables.reachM[180].x
				<< " upper[180]=" << acesTables.upperHullGamma[180].x
				<< " cusp[180]=" << acesTables.gamutCusps[180].x << "," << acesTables.gamutCusps[180].y
				<< " | rgbToCAM16c[0]=" << m0[0] << "," << m0[1] << "," << m0[2]
				<< " [1]=" << m0[4] << "," << m0[5] << "," << m0[6]
				<< " [2]=" << m0[8] << "," << m0[9] << "," << m0[10]
				<< " | cam16cToRGB[0]=" << m2[0] << "," << m2[1] << "," << m2[2]
				<< " anyNaN=" << anyNaN << std::endl;
		}
		ComputePass chromaticsCompute(context, chromaticsCs, 4, 1, false); // 4 SRV(t0-t3 表) + 1 UAV(u0 3D LUT)

		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> chromaticsHeap;
		{
			D3D12_DESCRIPTOR_HEAP_DESC cDesc{};
			cDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			cDesc.NumDescriptors = 6; // 0-3=表 SRV, 4=LUT UAV
			cDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(context.device()->CreateDescriptorHeap(&cDesc, IID_PPV_ARGS(&chromaticsHeap)))) {
				throw std::runtime_error("Failed to create chromatics descriptor heap");
			}
		}
		Microsoft::WRL::ComPtr<ID3D12Resource> acesParamsBuf, acesUpperBuf, acesReachBuf, acesCuspsBuf;
		Microsoft::WRL::ComPtr<ID3D12Resource> colorLutTex;
		{
			auto* device = context.device();
			const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
			const auto makeUploadBuffer = [&](ID3D12Resource** out, const void* data, UINT64 size) {
				D3D12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(size);
				Microsoft::WRL::ComPtr<ID3D12Resource> res;
				if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)))) {
					throw std::runtime_error("Failed to create ACES table upload buffer");
				}
				void* mapped = nullptr;
				res->Map(0, nullptr, &mapped);
				std::memcpy(mapped, data, size);
				res->Unmap(0, nullptr);
				*out = res.Detach();
			};
			const UINT64 tableBytes = static_cast<UINT64>(acesTables.reachM.size()) * sizeof(glm::vec4);
			makeUploadBuffer(&acesParamsBuf, &acesTables.params, sizeof(acesTables.params));
			// DEBUG：dump buffer 关键 float 字节（确定 C++ 端写入值）
			{
				void* pm = nullptr;
				acesParamsBuf->Map(0, nullptr, &pm);
				const uint32_t* u = (const uint32_t*)pm;
				std::cout << std::hex;
				std::cout << "[buf] u[25]=0x" << u[25] << " (inputJMh_A_w_j @off100)"
					<< " u[28]=0x" << u[28] << " (rgbToCAM16c[0][0] @off112)"
					<< " u[29]=0x" << u[29] << " u[30]=0x" << u[30]
					<< " u[40]=0x" << u[40] << " (coneResp[0][0] @off160)" << std::dec << std::endl;
				acesParamsBuf->Unmap(0, nullptr);
			}
			makeUploadBuffer(&acesUpperBuf, acesTables.upperHullGamma.data(), tableBytes);
			makeUploadBuffer(&acesReachBuf, acesTables.reachM.data(), tableBytes);
			makeUploadBuffer(&acesCuspsBuf, acesTables.gamutCusps.data(), tableBytes);

			// 32^3 RGBA16F 3D LUT（UAV 写，dispatch 后 barrier 回 SRV 供 PostProcess t6）。
			const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_RESOURCE_DESC lutDesc = CD3DX12_RESOURCE_DESC::Tex3D(DXGI_FORMAT_R16G16B16A16_FLOAT, 32, 32, 32, 1);
			lutDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &lutDesc,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&colorLutTex)))) {
				throw std::runtime_error("Failed to create color LUT texture");
			}

			const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			D3D12_CPU_DESCRIPTOR_HANDLE c0 = chromaticsHeap->GetCPUDescriptorHandleForHeapStart();
			const auto bufferSrv = [&](D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource* res, UINT numElems, UINT stride) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_UNKNOWN;
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Buffer.FirstElement = 0;
				srv.Buffer.NumElements = numElems;
				srv.Buffer.StructureByteStride = stride;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				device->CreateShaderResourceView(res, &srv, cpu);
			};
			bufferSrv(c0, acesParamsBuf.Get(), 1, sizeof(acesTables.params));
			bufferSrv(CD3DX12_CPU_DESCRIPTOR_HANDLE(c0, 1, inc), acesUpperBuf.Get(), 362, 16);
			bufferSrv(CD3DX12_CPU_DESCRIPTOR_HANDLE(c0, 2, inc), acesReachBuf.Get(), 362, 16);
			bufferSrv(CD3DX12_CPU_DESCRIPTOR_HANDLE(c0, 3, inc), acesCuspsBuf.Get(), 362, 16);
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
			uav.Texture3D.WSize = 32;
			device->CreateUnorderedAccessView(colorLutTex.Get(), nullptr, &uav, CD3DX12_CPU_DESCRIPTOR_HANDLE(c0, 4, inc));
			// LUT SRV -> sharedSrvHeap index 6（PostProcess t6）。
			D3D12_CPU_DESCRIPTOR_HANDLE s6 = sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
			s6.ptr += 6 * inc;
			D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv{};
			lutSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			lutSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			lutSrv.Texture3D.MipLevels = 1;
			lutSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; // 必须设（0 是无效值 → ERROR #27）
			device->CreateShaderResourceView(colorLutTex.Get(), &lutSrv, s6);
		}
		// 一次性 dispatch（参考 dispatchFrequency=Once）+ barrier LUT UAV→kSampling + readback 验证 LUT 内容。
		Microsoft::WRL::ComPtr<ID3D12Resource> lutReadback;
		{
			auto* device = context.device();
			// READBACK 只支持 buffer：32^3 * 8B(RGBA16F)。
			{
				const CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
				D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(32 * 32 * 32 * 8);
				if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rd,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&lutReadback)))) {
					throw std::runtime_error("Failed to create LUT readback buffer");
				}
			}
			ComPtr<ID3D12CommandAllocator> cAlloc;
			ComPtr<ID3D12GraphicsCommandList> cList;
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cAlloc)))) {
				throw std::runtime_error("Failed to create chromatics allocator");
			}
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cAlloc.Get(),
				nullptr, IID_PPV_ARGS(&cList)))) {
				throw std::runtime_error("Failed to create chromatics command list");
			}
			const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			const D3D12_GPU_DESCRIPTOR_HANDLE h0 = chromaticsHeap->GetGPUDescriptorHandleForHeapStart();
			chromaticsCompute.dispatch(chromaticsHeap.Get(),
				{ h0, CD3DX12_GPU_DESCRIPTOR_HANDLE(h0, 1, inc),
				  CD3DX12_GPU_DESCRIPTOR_HANDLE(h0, 2, inc), CD3DX12_GPU_DESCRIPTOR_HANDLE(h0, 3, inc) },
				{ CD3DX12_GPU_DESCRIPTOR_HANDLE(h0, 4, inc) },
				4, 4, 4, cList.Get()); // 32/8=4
			const D3D12_RESOURCE_STATES kSampling =
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			// LUT: UAV -> COPY_SOURCE -> copy to readback -> COPY_SOURCE -> kSampling
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					colorLutTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
				cList->ResourceBarrier(1, &b);
			}
			{
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = lutReadback.Get();
				dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, 32, 32, 32, 32 * 8 };
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = colorLutTex.Get();
				src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src.SubresourceIndex = 0;
				cList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					colorLutTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kSampling);
				cList->ResourceBarrier(1, &b);
			}
			if (FAILED(cList->Close())) {
				throw std::runtime_error("Failed to close chromatics command list");
			}
			ID3D12CommandList* const lists[] = { cList.Get() };
			context.commandQueue()->ExecuteCommandLists(1, lists);
			ComPtr<ID3D12Fence> cFence;
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cFence)))) {
				throw std::runtime_error("Failed to create chromatics fence");
			}
			if (FAILED(context.commandQueue()->Signal(cFence.Get(), 1))) {
				throw std::runtime_error("Signal failed (chromatics)");
			}
			if (cFence->GetCompletedValue() < 1) {
				HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (FAILED(cFence->SetEventOnCompletion(1, ev))) {
					throw std::runtime_error("SetEventOnCompletion failed (chromatics)");
				}
				WaitForSingleObject(ev, INFINITE);
				CloseHandle(ev);
			}
		}
		// CPU 验证 LUT 内容（诊断 LUT 全黑）—— 正确解析 R16G16B16A16_FLOAT（每像素 8B = 4 个 uint16 half）。
		{
			void* p = nullptr;
			HRESULT hrMap = lutReadback->Map(0, nullptr, &p);
			if (SUCCEEDED(hrMap) && p) {
				const uint16_t* h = static_cast<const uint16_t*>(p);
				const auto h2f = [](uint16_t v) -> float {
					const uint32_t sign = (v >> 15) & 0x1;
					const uint32_t exp = (v >> 10) & 0x1f;
					const uint32_t mant = v & 0x3ff;
					if (exp == 0) {
						if (mant == 0) return sign ? -0.0f : 0.0f;
						const float val = static_cast<float>(mant) * (1.0f / 1024.0f) * (1.0f / 16384.0f);
						return sign ? -val : val;
					}
					if (exp == 31) {
						if (mant == 0) return sign ? -INFINITY : INFINITY;
						return std::numeric_limits<float>::quiet_NaN();
					}
					const float val = (1.0f + static_cast<float>(mant) / 1024.0f) * std::exp2(static_cast<float>(exp) - 15);
					return sign ? -val : val;
				};
				const auto at = [&](int idx) {
					return std::make_tuple(h2f(h[idx * 4]), h2f(h[idx * 4 + 1]), h2f(h[idx * 4 + 2]));
				};
				const auto p0 = at(0);
				const auto pc = at((16 * 32 + 16) * 32 + 16);
				const auto p31 = at((31 * 32 + 31) * 32 + 31);
				std::cout << "[LUT-half] (0,0,0)=" << std::get<0>(p0) << "," << std::get<1>(p0) << "," << std::get<2>(p0)
					<< " (16,16,16)=" << std::get<0>(pc) << "," << std::get<1>(pc) << "," << std::get<2>(pc)
					<< " (31,31,31)=" << std::get<0>(p31) << "," << std::get<1>(p31) << "," << std::get<2>(p31) << std::endl;
			}
			lutReadback->Unmap(0, nullptr);
		}

		// ---- 程序化地形（完整照搬参考项目数据流）----
		// 纹理：MossyGrass(草地)/ButtressRoot(坡地 albedo+property)/RockCliff(坡地 normal)，
		// ambient 用 DefaultSky（IBL）。资产在参考项目 release/debug 预编译目录。
		const std::string terrTex = "../../AfterglowRender_v0.1_x64_release/Assets/Shared/Textures/Terrain/";
		TerrainRenderer::TexturePaths terrainPaths{
			terrTex + "MossyGrass/T_MossyGrass_01_BC.PNG",
			terrTex + "MossyGrass/T_MossyGrass_01_N.PNG",
			terrTex + "MossyGrass/T_MossyGrass_01_AoRDp.PNG",
			terrTex + "ButtressRoot/T_ButtressRoot_01_BC.PNG",
			terrTex + "RockCliff/T_RockCliff_01_N.PNG",
			terrTex + "ButtressRoot/T_ButtressRoot_01_AoRDp.PNG",
			"../vsbuild/Assets/Shared/Textures/DefaultSky.png",
		};
		TerrainRenderer terrainRenderer(context, terrainVs, terrainPs, terrainPaths, 4096);
		// PostProcess t5 = TerrainHeight（UnderwaterMask 采样水面高度）。写到共享堆 index 5。
		{
			auto* device = context.device();
			const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = sharedSrvHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += 5 * inc;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32G32_FLOAT;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(terrainRenderer.heightResource(), &srv, cpu);
		}
		ComputePass terrainHeightCompute(context, terrainHeightCs);        // 0 SRV + 1 UAV
		ComputePass terrainNormalCompute(context, terrainNormalCs, 1);     // 1 SRV + 1 UAV
		ComputePass terrainSurfaceCompute(context, terrainSurfaceCs, 2);   // 2 SRV + 1 UAV
		ComputePass meteorographInitCompute(context, meteorographInitCs);      // 0 SRV + 1 UAV
		ComputePass pluviometerInitCompute(context, pluviometerInitCs);        // 0 SRV + 1 UAV
		ComputePass meteorographCompute(context, meteorographCs, 1, 2, true);  // 1 SRV(t0 height) + 2 UAV(u0 meteorograph u1 pluviometer) + b0 CBV
		// TerrainData 水文模拟：2 SRV(t0 meteorograph t1 pluviometer) + 6 UAV(u0 height u1 normal u2 waterFlow u3 waterSediment u4 waterVelocity u5 surface) + b0 CBV + 2 sampler(s0 meteorograph s1 pluviometer)
		ComputePass terrainDataCompute(context, terrainDataCs, 2, 6, true, 2);

		// Meteorograph/TerrainData 共用的 time/deltaTime CBV（上传堆）。
		// 生命周期必须覆盖到命令列表 Close/Execute（D3D12 root CBV 只记录 GPU 虚拟地址，
		// 不持有资源引用；资源在命令列表关闭/提交期间被删会触发 ERROR #921）。
		Microsoft::WRL::ComPtr<ID3D12Resource> meteorTimeCbv;
		{
			const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
			const CD3DX12_RESOURCE_DESC cbvDesc = CD3DX12_RESOURCE_DESC::Buffer(64);
			if (FAILED(context.device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbvDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&meteorTimeCbv)))) {
				throw std::runtime_error("Failed to create meteorograph time CBV");
			}
		}
		{
			auto* device = context.device();
			ComPtr<ID3D12CommandAllocator> tAlloc;
			ComPtr<ID3D12GraphicsCommandList> tList;
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tAlloc)))) {
				throw std::runtime_error("Failed to create terrain init allocator");
			}
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tAlloc.Get(),
				nullptr, IID_PPV_ARGS(&tList)))) {
				throw std::runtime_error("Failed to create terrain init command list");
			}
			const D3D12_RESOURCE_STATES kSampling =
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

			// 1. height（照搬 TerrainHeightInitializer_CS）。
			terrainHeightCompute.dispatch(terrainRenderer.heap(), terrainRenderer.heightUav(),
				128, 128, 1, {}, tList.Get()); // 4096/32=128
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					terrainRenderer.heightResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSampling);
				tList->ResourceBarrier(1, &b);
			}
			// 2. normal（照搬 TerrainNormalInitializer_CS，读 height）。
			terrainNormalCompute.dispatch(terrainRenderer.heap(), terrainRenderer.normalUav(),
				128, 128, 1, terrainRenderer.dataSrvBase(), tList.Get());
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					terrainRenderer.normalResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSampling);
				tList->ResourceBarrier(1, &b);
			}
			// 3. surface（照搬 TerrainData_CS terrainSurface 块，读 height+normal）。
			terrainSurfaceCompute.dispatch(terrainRenderer.heap(), terrainRenderer.surfaceUav(),
				128, 128, 1, terrainRenderer.dataSrvBase(), tList.Get());
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					terrainRenderer.surfaceResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSampling);
				tList->ResourceBarrier(1, &b);
			}
			// 4. meteorograph 初始化（照搬 MeteorographInitializer_CS，128x128）。
			meteorographInitCompute.dispatch(terrainRenderer.heap(), terrainRenderer.meteorographUav(),
				4, 4, 1, {}, tList.Get()); // 128/32=4
			{
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					terrainRenderer.meteorographResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSampling);
				tList->ResourceBarrier(1, &b);
			}
			// 5. pluviometer 初始化（照搬 PluviometerInitializer_CS；D3D12 需显式清零）。
			pluviometerInitCompute.dispatch(terrainRenderer.heap(), terrainRenderer.pluviometerUav(),
				4, 4, 1, {}, tList.Get()); // 128/32=4
			{
				// 关键：pluviometer 初始化后必须 barrier 回采样状态（否则首帧每时代码假设 kSampling
				// 转 UAV 时 before 实际为 UAV → RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH）。
				const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
					terrainRenderer.pluviometerResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSampling);
				tList->ResourceBarrier(1, &b);
			}
			// 6. TerrainData 一次性水文模拟（照搬参考 TerrainData.mat dispatchFrequency=Once：
			//    初始化后只跑一次，不每帧。每帧跑会让浅水/侵蚀/蒸发不收敛、地形持续剧烈变化 → P0 崩坏）。
			//    t0=Meteorograph(SRV 采样) t1=Pluviometer(SRV 采样)
			//    u0=TerrainHeight u1=TerrainNormal u2=WaterFlow u3=WaterSediment u4=WaterVelocity u5=TerrainSurface。
			{
				// 复用 meteorTimeCbv 设置一次性模拟的 dt=1/60（参考项目 Once dispatch 在第一帧跑）。
				struct MeteorTime { float time; float deltaTime; float _pad[2]; };
				MeteorTime* mt = nullptr;
				meteorTimeCbv->Map(0, nullptr, reinterpret_cast<void**>(&mt));
				mt->time = 0.0f;
				mt->deltaTime = 1.0f / 60.0f;
				meteorTimeCbv->Unmap(0, nullptr);
				const ID3D12Resource* dataTexs[] = {
					terrainRenderer.heightResource(), terrainRenderer.normalResource(), terrainRenderer.surfaceResource() };
				for (auto* res : dataTexs) {
					const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
						const_cast<ID3D12Resource*>(res), kSampling, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					tList->ResourceBarrier(1, &b);
				}
				terrainDataCompute.dispatch(terrainRenderer.heap(),
					{ terrainRenderer.meteorographSrv(), terrainRenderer.pluviometerSrv() },
					{ terrainRenderer.heightUav(), terrainRenderer.normalUav(),
					  terrainRenderer.waterFlowUav(), terrainRenderer.waterSedimentUav(),
					  terrainRenderer.waterVelocityUav(), terrainRenderer.surfaceUav() },
					128, 128, 1, tList.Get(), meteorTimeCbv->GetGPUVirtualAddress(),
					terrainRenderer.samplerHeap(),
					{ terrainRenderer.terrainSamplerGpu(), terrainRenderer.terrainSamplerGpu() }); // 4096/32=128
				for (auto* res : dataTexs) {
					const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
						const_cast<ID3D12Resource*>(res), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSampling);
					tList->ResourceBarrier(1, &b);
				}
			}

			if (FAILED(tList->Close())) {
				throw std::runtime_error("Failed to close terrain init command list");
			}
			ID3D12CommandList* const lists[] = { tList.Get() };
			context.commandQueue()->ExecuteCommandLists(1, lists);
			ComPtr<ID3D12Fence> tFence;
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tFence)))) {
				throw std::runtime_error("Failed to create terrain init fence");
			}
			if (FAILED(context.commandQueue()->Signal(tFence.Get(), 1))) {
				throw std::runtime_error("Signal failed (terrain init)");
			}
			if (tFence->GetCompletedValue() < 1) {
				HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (FAILED(tFence->SetEventOnCompletion(1, ev))) {
					throw std::runtime_error("SetEventOnCompletion failed (terrain init)");
				}
				WaitForSingleObject(ev, INFINITE);
				CloseHandle(ev);
			}
		}

		// 地形网格：256x256 平面网格（z=0），VS 采样高度纹理位移（照搬参考项目 GridVertex/GridIndexPack）。
		const auto terrainVerts = Mesh::makeTerrainVertices(256, 4.0f); // 严格对齐参考项目 TerrainVertexInitializer (256, 4.0)
		const auto terrainIndices = Mesh::makeTerrainIndices(256);
		std::cout << "[AfterglowRender DX12] terrain mesh: " << terrainVerts.size()
			<< " verts, " << terrainIndices.size() << " indices" << std::endl;
		Mesh terrainMesh;
		terrainMesh.create(context, terrainVerts, terrainIndices);

		// 水面（照搬参考 WaterMesh.mat：Water_VS 采样 TerrainHeight.y 水位 + Water_FS 折射/泡沫/深度淡出）。
		// 需要地形数据资源 + MeshRenderer depth + DefaultSky ambient。
		WaterRenderer waterRenderer(context, waterVs, waterPs, fractalInitCs,
			terrainRenderer.heightResource(), terrainRenderer.normalResource(),
			terrainRenderer.waterSedimentResource(), terrainRenderer.waterVelocityResource(),
			meshRenderer.depthResource(), defaultSkyTex, DXGI_FORMAT_R16G16B16A16_FLOAT);
		waterRenderer.initializeFractalNoise();
		std::cout << "[AfterglowRender DX12] created WaterRenderer (fractal simplex noise init done)" << std::endl;

		// FractalNoise 完整数据流（Simplex + Worley + Normal），供未来 Grass/ShellFur 采样。
		FractalNoise fractalNoise(context, fractalInitCs, fractalWorleyCs, fractalNoiseCs);
		fractalNoise.initialize();
		std::cout << "[AfterglowRender DX12] created FractalNoise (simplex/worley/normal init done)" << std::endl;

		// Grass（照搬参考 GrassData.mat + GrassInstancing.mat）。
		// grassAlbedo = MossyGrass BC（terrain 同款）；FractalNormal 来自 FractalNoise；数据纹理来自 terrain。
		Texture grassAlbedoTex;
		grassAlbedoTex.load(context,
			"../../AfterglowRender_v0.1_x64_release/Assets/Shared/Textures/Terrain/MossyGrass/T_MossyGrass_01_BC.PNG");
		GrassRenderer grassRenderer(context,
			grassDataCs, grassInstCs, grassVs, grassPs,
			terrainRenderer.heightResource(), terrainRenderer.normalResource(),
			terrainRenderer.surfaceResource(), terrainRenderer.meteorographResource(),
			grassAlbedoTex, fractalNoise.normalResource(), defaultSkyTex.resource());
		grassRenderer.initialize();
		std::cout << "[AfterglowRender DX12] created GrassRenderer (grass data init done)" << std::endl;

		// Boids（照搬参考 BoidInstancing.mat：attribute/instance init + 每帧 O(N²) sim + instanced draw）。
		// 替换纸飞机 CPU 环形轨迹为 GPU 群集模拟。纸飞机网格 + 地形高度（回避）+ 白/normal/ambient。
		BoidsRenderer boidsRenderer(context,
			boidAttrInitCs, boidInstInitCs, boidSimCs, boidVs, boidPs,
			terrainRenderer.heightResource(),
			placeholderWhite.resource(), defaultNormalTex.resource(), defaultSkyTex.resource());
		boidsRenderer.initialize();
		std::cout << "[AfterglowRender DX12] created BoidsRenderer (attribute/instance init done)" << std::endl;

		// ShellFur（AnkhaFur）：8 壳层实例化毛。albedo/normal = Ankha 身体纹理，FractalNoise 供毛噪声。
		FurRenderer furRenderer(context, furVs, furPs,
			ankhaBody.resource(), ankhaBodyN.resource(),
			fractalNoise.worleyResource(), fractalNoise.normalResource());
		std::cout << "[AfterglowRender DX12] created FurRenderer (ShellFur, 8 shells)" << std::endl;

		FullScreenPass downSamplingPass(context, downSh.vs, downSh.ps, DXGI_FORMAT_R16G16B16A16_FLOAT);
		FullScreenPass horizontalBlurPass(context, horzSh.vs, horzSh.ps, DXGI_FORMAT_R16G16B16A16_FLOAT);
		FullScreenPass verticalBlurPass(context, vertSh.vs, vertSh.ps, DXGI_FORMAT_R16G16B16A16_FLOAT);

		// ---- 加载参考项目模型 ----
		Model boxModel;
		boxModel.load(context, "../vsbuild/Assets/Shared/Models/Box.fbx");
		std::cout << "[AfterglowRender DX12] loaded Box.fbx: "
			<< boxModel.meshes().size() << " mesh(es)" << std::endl;

		// 参考项目 Shared/Models 简单模型（远距离放置，验证雾效果）。
		Model sphereModel, planeModel, prismModel;
		sphereModel.load(context, "../vsbuild/Assets/Shared/Models/Sphere.fbx");
		planeModel.load(context, "../vsbuild/Assets/Shared/Models/Plane.fbx");
		prismModel.load(context, "../vsbuild/Assets/Shared/Models/HexagonalPrism.fbx");
		std::cout << "[AfterglowRender DX12] loaded shared models (Sphere/Plane/Prism): "
			<< sphereModel.meshes().size() << "/" << planeModel.meshes().size() << "/"
			<< prismModel.meshes().size() << " mesh(es)" << std::endl;

		// SkySphere 天空盒（参考项目 Shared/Models/SkySphere.fbx，scale 大包住场景）。
		Model skySphereModel;
		skySphereModel.load(context, "../vsbuild/Assets/Shared/Models/SkySphere.fbx");
		std::cout << "[AfterglowRender DX12] loaded SkySphere: "
			<< skySphereModel.meshes().size() << " mesh(es), materials=[";
		for (std::size_t i = 0; i < skySphereModel.materialNames().size(); ++i) {
			std::cout << skySphereModel.materialNames()[i] << (i+1 < skySphereModel.materialNames().size() ? "," : "");
		}
		std::cout << "]" << std::endl;

		Model battleMageModel;
		battleMageModel.load(context, "../vsbuild/Assets/Characters/BattleMage/BattleMage.fbx");
		std::cout << "[AfterglowRender DX12] loaded BattleMage.fbx: "
			<< battleMageModel.meshes().size() << " mesh(es)" << std::endl;
		for (std::size_t i = 0; i < battleMageModel.materialNames().size(); ++i) {
			std::cerr << "  mesh[" << i << "] material='"
				<< battleMageModel.materialNames()[i] << "'" << std::endl;
		}

		// Yvonne（第二个角色，参考项目 Assets/Characters/Yvonne/Yvonne.fbx，无 scale）。
		Model yvonneModel;
		yvonneModel.load(context, "../vsbuild/Assets/Characters/Yvonne/Yvonne.fbx");
		std::cout << "[AfterglowRender DX12] loaded Yvonne.fbx: "
			<< yvonneModel.meshes().size() << " mesh(es)" << std::endl;

		// Ankha（第三个角色，参考项目 Assets/Characters/Ankha/Ankha.fbx，scale 0.02）。
		Model ankhaModel;
		ankhaModel.load(context, "../vsbuild/Assets/Characters/Ankha/Ankha.fbx");
		std::cout << "[AfterglowRender DX12] loaded Ankha.fbx: "
			<< ankhaModel.meshes().size() << " mesh(es)" << std::endl;

		// 装饰模型：PaperAirplane（参考项目 Boids，scale 0.2）+ DebugBox（参考项目 GlitchedMosaics，位置 (4,4,1)）。
		Model paperPlaneModel, debugBoxModel;
		paperPlaneModel.load(context, "../vsbuild/Assets/Shared/Models/PaperAirplane.fbx");
		debugBoxModel.load(context, "../vsbuild/Assets/Shared/Models/DebugBox.fbx");
		std::cout << "[AfterglowRender DX12] loaded PaperAirplane/DebugBox: "
			<< paperPlaneModel.meshes().size() << "/" << debugBoxModel.meshes().size() << " mesh(es)" << std::endl;

		// Grass 实例化网格（参考项目 InstancingGrass.fbx）。
		Model grassMeshModel;
		grassMeshModel.load(context, "../vsbuild/Assets/Shared/Models/InstancingGrass.fbx");
		std::cout << "[AfterglowRender DX12] loaded InstancingGrass.fbx: "
			<< grassMeshModel.meshes().size() << " mesh(es)" << std::endl;

		// AnkhaFur（参考项目 ShellFur 实例化毛：AnkhaFur.fbx + AnkhaFur.mati parent=ShellFur）。
		Model ankhaFurModel;
		ankhaFurModel.load(context, "../vsbuild/Assets/Characters/Ankha/AnkhaFur.fbx");
		std::cout << "[AfterglowRender DX12] loaded AnkhaFur.fbx: "
			<< ankhaFurModel.meshes().size() << " mesh(es)" << std::endl;

		// Terrain.fbx：参考项目 disabled（用 TerrainData/TerrainMesh/WaterMesh compute shader 程序化生成地形）。
		// 对齐参考项目：不加载 Terrain.fbx。compute 地形属后续大工程（需 compute pipeline）。

		// ---- 场景物体（mesh + 位置 + 材质颜色） ----
		// 已清理测试几何（棋盘纹理立方体/球/棱柱/远景模型），只保留天空盒 + 地面 + 角色 + 纸飞机。
		std::vector<DrawItem> items = {
			// SkySphere 天空盒（遗留 #1 修复）：SkySphere.fbx 半径 50000，scale 0.01 → 世界半径 500
			// （参考项目 SkySphere scale 0.01）。SkySphere shader 使球心跟随相机（相机永远在球内）。
			// 必须画在 items 最前（0 号），否则 disable-depth 球后画会覆盖所有物体。
			{ &skySphereModel.meshes()[0], glm::scale(glm::mat4(1.0f), glm::vec3(0.01f)),
				glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), 3, false, false, 0.0f, true },
			// 地面：由程序化地形（TerrainRenderer）替代，此处不再用程序化平面。
		};

	// BattleMage（ArcToon 材质，参考项目 ArcToon.mat 3 纹理 + 描边）。
	// 位置对齐参考项目：translation {0,0,0} scale 0.01（z-up→y-up 转 -90°X）。
	const glm::mat4 battleMageBase = glm::rotate(
		glm::scale(glm::mat4(1.0f), glm::vec3(0.01f)),
		glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	// 角色材质纹理组（每组 5 槽 t0-t4；组号 = 索引，与材质 enum 无关）。
	// t3 对 Endfield 系列 = mask/st 槽，对描边 = stTex。缺槽用占位（黑/白）避免非法 SRV。
	// 组 11 = 纯白组（EndfieldOutline 的 stTex 空路径 → 参考项目默认 White.png）。
	const std::vector<std::vector<Texture*>> materialGroups = {
		// 0 ArcToon：albedo+sss+ilm
		{ &battleMageBody, &battleMageSss, &battleMageIlm, &placeholderBlack, &placeholderBlack },
		// 1 EndfieldBody：albedo+normal+ramp（t3=white 供 Body 描边 stTex）
		{ &yvonneBody, &yvBodyN, &yvBodyRD, &placeholderWhite, &placeholderWhite },
		// 2 EndfieldPBR Cloth：albedo+normal+property（t3 mask=black）
		{ &yvonneCloth1, &yvCloth1N, &yvCloth1P, &placeholderBlack, &placeholderBlack },
		// 3 EndfieldPBR Tail：cloth_02 albedo+normal+property
		{ &yvonneCloth2, &yvCloth2N, &yvCloth2P, &placeholderBlack, &placeholderBlack },
		// 4 EndfieldThinFilm：albedo+normal+property+mask+ramp
		{ &yvonneCloth1, &yvCloth1N, &yvCloth1P, &yvCloth1M, &yvCloth5RS },
		// 5 EndfieldFace：albedo+sdf+ramp+mask+highlightMask
		{ &yvonneFace, &yvFaceSDF, &yvFaceRD, &yvFaceCmM, &yvFaceHlM },
		// 6 EndfieldHair：albedo+hn+property+st+hairRamp（t3=ST，Hair 描边 stTex 用）
		{ &yvonneHair, &yvHairHN, &yvHairP, &yvHairST, &yvHairRD },
		// 7 EndfieldIris：albedo（简化版只用 t0）
		{ &yvonneIris, &placeholderBlack, &placeholderBlack, &placeholderBlack, &placeholderBlack },
		// 8 EndfieldEyelash：albedo+ramp
		{ &yvonneFace, &yvFaceRD, &placeholderBlack, &placeholderBlack, &placeholderBlack },
		// 9 EndfieldEyeShadow：无纹理
		{ &placeholderWhite, &placeholderWhite, &placeholderWhite, &placeholderWhite, &placeholderWhite },
		// 10 EndfieldHairShadow：无纹理
		{ &placeholderWhite, &placeholderWhite, &placeholderWhite, &placeholderWhite, &placeholderWhite },
		// 11 纯白组（描边 stTex=White.png 默认）
		{ &placeholderWhite, &placeholderWhite, &placeholderWhite, &placeholderWhite, &placeholderWhite },
		// 12 AnkhaBody：albedo+normal（DefaultLit 不用 t3-t4）
		{ &ankhaBody, &ankhaBodyN, &placeholderBlack, &placeholderBlack, &placeholderBlack },
		// 13 AnkhaDress：albedo+normal
		{ &ankhaDress, &ankhaDressN, &placeholderBlack, &placeholderBlack, &placeholderBlack },
		// 14 AnkhaEye：albedo（无 normalTex）
		{ &ankhaEye, &placeholderBlack, &placeholderBlack, &placeholderBlack, &placeholderBlack },
	// 15 AnkhaHair：albedo+normal
	{ &ankhaHair, &ankhaHairN, &placeholderBlack, &placeholderBlack, &placeholderBlack },
	// 16 Ball（Standard/BallPBR）：albedo 白 + DefaultNormal 中性法线
	{ &placeholderWhite, &defaultNormalTex, &placeholderBlack, &placeholderBlack, &placeholderBlack },
	// 17 Box（ArcToon 武器材质 BattleMageWeapon.mati）：W00 albedo+sss+ilm
	{ &boxAlbedo, &boxSss, &boxIlm, &placeholderBlack, &placeholderBlack },
};
	std::vector<Texture*> materialTextures;
	for (const auto& g : materialGroups) {
		materialTextures.insert(materialTextures.end(), g.begin(), g.end());
	}

	// 角色材质渲染器：11 材质 PSO（ArcToon/Endfield*/DefaultLit）+ 2 描边 PSO。
	CharacterRenderer characterRenderer(context, characterVsList, characterFsList,
		characterMaterialDescs, outlineVsList, outlineFsList, DXGI_FORMAT_R16G16B16A16_FLOAT,
		materialTextures, defaultSkyTex);
	characterRenderer.setLight(sceneLightDir, dirLightColor, dirLightIntensity);

	// charItems：BattleMage 4 mesh 全 ArcToon + 全描边。
	// 描边参数对齐 BattleMageOutline.mati：outlineWidth=10, outlineDistanceFadeFactor=0.001，黑色。
	std::vector<CharacterDrawItem> charItems;
	for (std::size_t i = 0; i < battleMageModel.meshes().size(); ++i) {
		CharacterDrawItem item;
		item.mesh = &battleMageModel.meshes()[i];
		item.model = battleMageBase;
		item.material = CharacterMaterial::ArcToon;
		item.textureGroup = 0;
		item.outline = true;
		item.params.outlineColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		item.params.outlineParams = glm::vec4(10.0f, 0.001f, 0.0f, 0.0f);
		charItems.push_back(item);
	}

	// Yvonne 12 mesh → CharacterRenderer（参考项目 yvonneMesh.setMaterial 顺序）。
	// 材质映射（Yvonne*.mati）：0 Body=EndfieldBody 1 Neck=EndfieldBody 2/5 Cloth=EndfieldPBR
	//   3 Tail=EndfieldPBR 4 ThinFilm=EndfieldThinFilm(半透明) 6 EyeShadow 7 Face 8 Eyelash
	//   9 Iris 10 Hair 11 HairShadow。
	// 描边（drawIndex 1）：mesh 0/2/3/5/10 挂 EndfieldOutline（mesh 4 ThinFilm 用 Ignore 不描边）。
	// 描边参数 Yvonne*Outline.mati：outlineWidth=16, outlineDistanceFadeFactor=0.0002。
	const glm::mat4 yvonneBase = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.1f, -4.0f))
		* glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	// 对象轴（z-up Forward=(0,1,0) Up=(0,0,1) 经 -90°X → y-up）：Up=(0,1,0) Forward=(0,0,-1) Right=(1,0,0)。
	const glm::vec3 yvUp(0.0f, 1.0f, 0.0f), yvForward(0.0f, 0.0f, -1.0f), yvRight(1.0f, 0.0f, 0.0f);
	// 每 mesh 配置：材质/纹理组/描边/描边色/materialParams/描边纹理组。
	// materialParams(x=rimLightWidth y=rimLightIntensity z=subsurfaceMFP w=viewAttenuation)。
	struct YvMeshCfg {
		CharacterMaterial mat; UINT group; bool outline;
		glm::vec4 outlineColor; glm::vec4 params; UINT outlineGroup;
	};
	const YvMeshCfg yvCfg[12] = {
		{ CharacterMaterial::EndfieldBody,     1,  true,  glm::vec4(0.12f, 0.02f, 0.04f, 1.0f), glm::vec4(0.2f, 0.5f, 0.25f, 0.75f), 1 },  // 0 Body
		{ CharacterMaterial::EndfieldBody,     1,  false, glm::vec4(0.0f),                      glm::vec4(0.2f, 0.5f, 0.4f, 0.5f),  1 },  // 1 Neck
		{ CharacterMaterial::EndfieldPBR,      2,  true,  glm::vec4(0.0f),                      glm::vec4(0.15f),                  11 },  // 2 Cloth
		{ CharacterMaterial::EndfieldPBR,      3,  true,  glm::vec4(0.0f),                      glm::vec4(0.2f),                   11 },  // 3 Tail
		{ CharacterMaterial::EndfieldThinFilm, 4,  false, glm::vec4(0.0f),                      glm::vec4(0.1f),                   11 },  // 4 ThinFilm
		{ CharacterMaterial::EndfieldPBR,      2,  true,  glm::vec4(0.0f),                      glm::vec4(0.15f),                  11 },  // 5 Cloth
		{ CharacterMaterial::EndfieldEyeShadow, 9, false, glm::vec4(0.0f),                      glm::vec4(1.0f),                   9 },  // 6 EyeShadow
		{ CharacterMaterial::EndfieldFace,     5,  false, glm::vec4(0.0f),                      glm::vec4(1.0f, 0.5f, 0.4f, 1.0f),  5 },  // 7 Face
		{ CharacterMaterial::EndfieldEyelash,  8,  false, glm::vec4(0.0f),                      glm::vec4(1.0f, 1.0f, 0.5f, 1.0f),  8 },  // 8 Eyelash
		{ CharacterMaterial::EndfieldIris,     7,  false, glm::vec4(0.0f),                      glm::vec4(1.0f),                   7 },  // 9 Iris
		{ CharacterMaterial::EndfieldHair,     6,  true,  glm::vec4(0.05f, 0.0f, 0.08f, 1.0f),  glm::vec4(0.2f),                   6 },  // 10 Hair
		{ CharacterMaterial::EndfieldHairShadow, 10, false, glm::vec4(0.02f, 0.002f, 0.0f, 0.5f), glm::vec4(1.0f), 10 },                   // 11 HairShadow
	};
	// 不透明先画（0,1,2,3,5,7,9,10），半透明后画（4,6,8,11）。
	const int yvOpaqueOrder[8] = { 0, 1, 2, 3, 5, 7, 9, 10 };
	const int yvTransparentOrder[4] = { 4, 6, 8, 11 };
	const auto pushYvonne = [&](int meshIdx) {
		const YvMeshCfg& cfg = yvCfg[meshIdx];
		CharacterDrawItem item;
		item.mesh = &yvonneModel.meshes()[meshIdx];
		item.model = yvonneBase;
		item.material = cfg.mat;
		item.textureGroup = cfg.group;
		item.outlineTextureGroup = cfg.outlineGroup;
		item.outline = cfg.outline;
		item.objectUp = yvUp;
		item.objectForward = yvForward;
		item.objectRight = yvRight;
		item.params.materialParams = cfg.params;
		if (meshIdx == 9) {
			item.params.materialParams3.y = 0.2f; // Iris irisDepth（YvonneIris.mati）
		}
		if (cfg.outline) {
			item.params.outlineColor = cfg.outlineColor;
			item.params.outlineParams = glm::vec4(16.0f, 0.0002f, 0.0f, 0.0f);
		} else if (meshIdx == 11) {
			item.params.outlineColor = cfg.outlineColor; // HairShadow shadowColor（FS 读 outlineColor）
		}
		charItems.push_back(item);
	};
	for (int i : yvOpaqueOrder) {
		if (i < static_cast<int>(yvonneModel.meshes().size())) pushYvonne(i);
	}
	for (int i : yvTransparentOrder) {
		if (i < static_cast<int>(yvonneModel.meshes().size())) pushYvonne(i);
	}

	// Ankha（第三个角色，参考项目 scale 0.02, translation z-up {0,6,0.5} → y-up {0,0.5,-6}）。
	// 11 mesh 共享 DefaultLit 材质，4 个纹理组（Body/Dress/Eye/Hair）。
	// mesh 映射（参考 .fbx mesh 名含 Dress/Body/Eye/Hair）：0 Dress / 1-3,5,6,8,10 Body / 4,7 Eye / 9 Hair / 11 默认 Body。
	// 参考项目无 outline 材质 pass → 不描边。AnkhaFur.fbx（毛茸茸，instancing）本工程暂不支持。
	const glm::mat4 ankhaBase = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, -6.0f))
		* glm::rotate(glm::scale(glm::mat4(1.0f), glm::vec3(0.02f)),
			glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	// materialParams2（DefaultLit）：x=metallic y=specular z=roughness w=anisotropy
	struct AnkMeshCfg {
		UINT group; glm::vec4 params2;
	};
	const AnkMeshCfg ankCfg[4] = {
		{ 12, glm::vec4(0.2f, 0.5f, 0.5f, 0.0f) },  // Body (AnkhaBody.mati)
		{ 13, glm::vec4(0.2f, 0.5f, 0.5f, 0.0f) },  // Dress (AnkhaDress.mati)
		{ 14, glm::vec4(0.5f, 0.0f, 1.0f, 0.0f) },  // Eye (AnkhaEye.mati)
		{ 15, glm::vec4(0.75f, 0.5f, 0.4f, 0.0f) }, // Hair (AnkhaHair.mati)
	};
	const UINT ankGroup[12] = { 13, 12, 12, 12, 14, 12, 12, 14, 12, 15, 12, 12 };
	for (std::size_t i = 0; i < ankhaModel.meshes().size(); ++i) {
		CharacterDrawItem item;
		item.mesh = &ankhaModel.meshes()[i];
		item.model = ankhaBase;
		// debug 版 Ankha.mat：身体材质用 ShellFurMask_VS（mask 外扩 + 深度偏移），其余角色保持 release。
		item.material = CharacterMaterial::AnkhaMask;
		item.textureGroup = (i < 12) ? ankGroup[i] : 12;
		// 从 fbx mesh name 推断材质（Dress/Body/Eye/Hair），参考 Ankha*.mati 命名。
		UINT cfgIdx = 0; // 默认 Body
		if (i < ankhaModel.materialNames().size()) {
			const auto& matName = ankhaModel.materialNames()[i];
			if (matName.find("Dress") != std::string::npos) cfgIdx = 1;
			else if (matName.find("Eye") != std::string::npos) cfgIdx = 2;
			else if (matName.find("Hair") != std::string::npos) cfgIdx = 3;
		}
		const AnkMeshCfg& c = ankCfg[cfgIdx];
		item.params.materialParams2 = c.params2;
		// materialParams3：x=subsurfaceOpacity(1.0) y=irisDepth(0) z=sizeExpand(0.16) w=depthOffset(0.0002)
		// —— 对齐 debug 版 Ankha.mat 的 sizeExpand/depthOffset 标量。
		item.params.materialParams3 = glm::vec4(1.0f, 0.0f, 0.16f, 0.0002f);
		item.outline = false;
		charItems.push_back(item);
	}

	// Ball（参考项目 Sphere.fbx + BallPBR/Standard 材质，独立 entity）。
	// 参考 z-up translation {4,0,2} scale 1 → y-up (4,2,0)（rotate -90X 同角色）。
	// BallPBR.mati：metallic 0.5 / specular 0.5 / roughness 0.4 / anisotropy -0.5 / subsurfaceOpacity 1.0。
	if (!sphereModel.meshes().empty()) {
		CharacterDrawItem ballItem;
		ballItem.mesh = &sphereModel.meshes()[0];
		ballItem.model = glm::translate(glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			glm::vec3(4.0f, 2.0f, 0.0f));
		ballItem.material = CharacterMaterial::DefaultLit;
		ballItem.textureGroup = 16; // albedo 白 + DefaultNormal
		ballItem.params.materialParams2 = glm::vec4(0.5f, 0.5f, 0.4f, -0.5f);
		ballItem.params.materialParams3 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // subsurfaceOpacity 1.0
		ballItem.outline = false;
		charItems.push_back(ballItem);
	}

	// Box/BoxB（参考项目 Box.fbx，BattleMage 子物体，ArcToon 武器材质）。
	// 参考 z-up：Box 局部 scale(40,20,20) translation(80,-20,50) euler(0,15,45)；
	// 世界 = battleMageBase * BoxLocal(z-up 局部)。BoxB = Box 子物体 T(0,-40,0)（默认 Standard）。
	if (!boxModel.meshes().empty()) {
		CharacterDrawItem boxItem;
		boxItem.mesh = &boxModel.meshes()[0];
		const glm::mat4 boxLocal = glm::translate(glm::mat4(1.0f), glm::vec3(80.0f, -20.0f, 50.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(15.0f), glm::vec3(0.0f, 0.0f, 1.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::scale(glm::mat4(1.0f), glm::vec3(40.0f, 20.0f, 20.0f));
		boxItem.model = battleMageBase * boxLocal;
		boxItem.material = CharacterMaterial::ArcToon;
		boxItem.textureGroup = 17; // W00 武器材质（BattleMageWeapon.mati）
		boxItem.outline = false;
		charItems.push_back(boxItem);

		if (boxModel.meshes().size() > 1 || true) {
			CharacterDrawItem boxBItem;
			boxBItem.mesh = &boxModel.meshes()[0];
			boxBItem.model = boxItem.model * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -40.0f, 0.0f));
			boxBItem.material = CharacterMaterial::DefaultLit; // 参考 BoxB 无材质 → 默认 Standard
			boxBItem.textureGroup = 16; // albedo 白 + DefaultNormal
			boxBItem.params.materialParams2 = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);
			boxBItem.params.materialParams3 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
			boxBItem.outline = false;
			charItems.push_back(boxBItem);
		}
	}

		// 角色世界位置核对（base[3] = 模型原点世界平移，与参考项目 z-up 转 y-up 后一致）。
		std::cerr << "[scene] BattleMage world pos: " << battleMageBase[3].x << ", "
			<< battleMageBase[3].y << ", " << battleMageBase[3].z << std::endl;
		std::cerr << "[scene] Yvonne world pos: " << yvonneBase[3].x << ", "
			<< yvonneBase[3].y << ", " << yvonneBase[3].z << std::endl;
		std::cerr << "[scene] Ankha world pos: " << ankhaBase[3].x << ", "
			<< ankhaBase[3].y << ", " << ankhaBase[3].z << std::endl;

		// PaperAirplane：改由 BoidsRenderer（compute 群集）渲染，见渲染循环 boidsRenderer.render()。
		// DebugBox.fbx 移除：测试用的 1x1x1 棋盘盒子，无实际用途。

		// 相机：F 键在轨道/自由之间切换。默认轨道（对齐参考 MainCamera + SimpleController）。
		enum class CameraMode { Orbit, Free };
		CameraMode cameraMode = CameraMode::Orbit;

		OrbitCamera orbitCamera;
		// 对齐参考 MainCamera：target = BattleMage + (0,0,0.8) z-up → y-up (0,0.8,0)；距离≈2（参考相机在 (0,1,-2)）。
		orbitCamera.setTarget(glm::vec3(0.0f, 0.8f, 0.0f));
		orbitCamera.setDistance(2.5f);

		FreeCamera freeCamera;

		double lastMouseX = 0.0, lastMouseY = 0.0;
		window.getCursorPos(lastMouseX, lastMouseY);
		bool fKeyPrev = false;

		// 10d-2 调试：数字键切换后处理效果（1=色差 2=尘埃 3=暗角 4=全开 0=关 5=noise 6=sceneTexture 7=bloomTexture 8=ColorLUT）。
		std::uint32_t postEffects = 0x7u;
		bool fxKeyPrev[8] = { false, false, false, false, false, false, false, false }; // 1/2/3/0/5/6/7/8

		const auto start = std::chrono::steady_clock::now();
		auto lastFrame = std::chrono::steady_clock::now();

		while (!window.shouldClose()) {
			window.update();

			if (!context.beginFrame()) {
				continue;
			}

			// 帧时间。
			const auto now = std::chrono::steady_clock::now();
			const float dt = std::chrono::duration<float>(now - lastFrame).count();
			lastFrame = now;
			const float t = std::chrono::duration<float>(now - start).count();

			// F 键切换相机模式（边沿触发）。
			const bool fKeyNow = window.isKeyPressed(GLFW_KEY_F);
			if (fKeyNow && !fKeyPrev) {
				cameraMode = (cameraMode == CameraMode::Orbit) ? CameraMode::Free : CameraMode::Orbit;
				std::cout << "[AfterglowRender DX12] camera mode: "
					<< (cameraMode == CameraMode::Orbit ? "orbit" : "free") << std::endl;
			}
			fKeyPrev = fKeyNow;

			// 10d-2：数字键切换后处理效果（边沿触发）。
			auto toggleFx = [&](int key, std::uint32_t bit, int idx, const char* name) {
				const bool now = window.isKeyPressed(key);
				if (now && !fxKeyPrev[idx]) {
					postEffects ^= bit;
					std::cout << "[AfterglowRender DX12] fx " << name << (postEffects & bit ? " ON" : " OFF")
						<< " (0x" << std::hex << postEffects << std::dec << ")" << std::endl;
				}
				fxKeyPrev[idx] = now;
			};
			toggleFx(GLFW_KEY_1, 0x1u, 0, "chromatic aberration");
			toggleFx(GLFW_KEY_2, 0x2u, 1, "dust");
			toggleFx(GLFW_KEY_3, 0x4u, 2, "vignette");
			toggleFx(GLFW_KEY_5, 0x8u, 4, "compute noise"); // 5 = 全屏显示 compute 噪声
			toggleFx(GLFW_KEY_6, 0x10u, 5, "sceneTexture debug"); // 6 = 显示场景原始色（诊断全红根因）
			toggleFx(GLFW_KEY_7, 0x20u, 6, "bloomTexture debug"); // 7 = 显示 bloom 原始色（诊断 bloom 偏色）
			toggleFx(GLFW_KEY_8, 0x40u, 7, "ColorLUT debug"); // 8 = 显示 3D LUT 固定采样（诊断 LUT 内容）
			// 0 = 全关，4 = 全开（直接赋值）。
			const bool fxKey0 = window.isKeyPressed(GLFW_KEY_0);
			if (fxKey0 && !fxKeyPrev[3]) {
				postEffects = 0x0u;
				std::cout << "[AfterglowRender DX12] fx all OFF (0x0)" << std::endl;
			}
			fxKeyPrev[3] = fxKey0;
			if (window.isKeyPressed(GLFW_KEY_4)) {
				postEffects = 0x7u; // 4 = 全开
			}

			// ---- 相机输入 ----
			double mouseX = 0.0, mouseY = 0.0;
			window.getCursorPos(mouseX, mouseY);
			const double dx = mouseX - lastMouseX;
			const double dy = mouseY - lastMouseY;
			lastMouseX = mouseX;
			lastMouseY = mouseY;

			glm::mat4 view;
			if (cameraMode == CameraMode::Orbit) {
				if (window.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)) {
					orbitCamera.rotate(static_cast<float>(dx), static_cast<float>(dy));
				}
				if (window.isMouseButtonPressed(GLFW_MOUSE_BUTTON_MIDDLE)) {
					orbitCamera.pan(static_cast<float>(dx), static_cast<float>(dy));
				}
				const double scroll = window.consumeScrollOffset();
				if (scroll != 0.0) {
					orbitCamera.zoom(static_cast<float>(-scroll));
				}
				view = orbitCamera.viewMatrix();
			} else {
				if (window.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
					freeCamera.rotate(static_cast<float>(dx), static_cast<float>(dy));
				}
				const float forwardAxis = (window.isKeyPressed(GLFW_KEY_W) ? 1.0f : 0.0f)
					- (window.isKeyPressed(GLFW_KEY_S) ? 1.0f : 0.0f);
				const float strafeAxis = (window.isKeyPressed(GLFW_KEY_D) ? 1.0f : 0.0f)
					- (window.isKeyPressed(GLFW_KEY_A) ? 1.0f : 0.0f);
				const float upAxis = ((window.isKeyPressed(GLFW_KEY_E) || window.isKeyPressed(GLFW_KEY_SPACE)) ? 1.0f : 0.0f)
					- ((window.isKeyPressed(GLFW_KEY_Q) || window.isKeyPressed(GLFW_KEY_C)) ? 1.0f : 0.0f);
				freeCamera.move(forwardAxis, strafeAxis, upAxis, dt);
				view = freeCamera.viewMatrix();
			}

			// 物体动画：纸飞机已由 Boids compute 群集模拟（见下方 boidsRenderer.render）；SkySphere/地面/角色固定。

			const glm::mat4 proj = (cameraMode == CameraMode::Orbit)
				? orbitCamera.projMatrix(static_cast<float>(context.width()) / static_cast<float>(context.height()))
				: glm::perspective(glm::radians(45.0f), // 对齐参考 AfterglowCameraComponent fovY=45°
					static_cast<float>(context.width()) / static_cast<float>(context.height()),
					0.1f, 1000.0f); // near 0.1 对齐参考

			// compute pipeline：生成噪声纹理 → barrier 到 SRV（PostProcess 按键 5 显示验证）。
			noiseUav.transitionColor(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			noiseCompute.dispatch(sharedSrvHeap.Get(), noiseUav.uavHandle(), 32, 32, 1); // 256/8=32
			noiseUav.transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			// Meteorograph 每帧模拟（照搬参考 Meteorograph.mat computeTask dispatchFrequency 每帧）：
			//   t0=TerrainHeight(SRV 读)  u0=Meteorograph(UAV 读+写)  u1=Pluviometer(UAV 写降雨)。
			// 需先 barrier meteorograph 到 UAV（模拟写），跑完再回 SRV（TerrainData/地形采样）。
			{
				struct MeteorTime { float time; float deltaTime; float _pad[2]; };
				MeteorTime* mt = nullptr;
				meteorTimeCbv->Map(0, nullptr, reinterpret_cast<void**>(&mt));
				mt->time = t;
				mt->deltaTime = (std::min)(dt, 0.05f); // 防止大帧间隔让模拟爆炸（参考项目 TODO: Limit max deltaTime）
				meteorTimeCbv->Unmap(0, nullptr);

				const D3D12_RESOURCE_STATES kSampling =
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				const D3D12_RESOURCE_STATES kUavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				// meteorograph + pluviometer 转 UAV（Meteorograph_CS 写）。
				{
					const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
						terrainRenderer.meteorographResource(), kSampling, kUavState);
					context.commandList()->ResourceBarrier(1, &b);
				}
				{
					const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
						terrainRenderer.pluviometerResource(), kSampling, kUavState);
					context.commandList()->ResourceBarrier(1, &b);
				}
				meteorographCompute.dispatch(terrainRenderer.heap(),
					{ terrainRenderer.heightSrv() },
					{ terrainRenderer.meteorographUav(), terrainRenderer.pluviometerUav() },
					4, 4, 1, nullptr, meteorTimeCbv->GetGPUVirtualAddress()); // 128/32=4
				// meteorograph 回 SRV（地形渲染采样）。
				{
					const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
						terrainRenderer.meteorographResource(), kUavState, kSampling);
					context.commandList()->ResourceBarrier(1, &b);
				}
				// pluviometer 回 SRV。
				{
					const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
						terrainRenderer.pluviometerResource(), kUavState, kSampling);
					context.commandList()->ResourceBarrier(1, &b);
				}
				// TerrainData_CS 不在每帧跑（参考项目 dispatchFrequency=Once，只在初始化时跑一次，见上方初始化块）。
			}

			// 阶段 10：场景渲染到离屏 sceneColor（HDR）。
			offscreen.transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float clearColor[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
			context.commandList()->ClearRenderTargetView(offscreen.rtvHandle(), clearColor, 0, nullptr);
			// 程序化地形：先画并写深度（VS 采样高度纹理），物体/角色后画保留地形遮挡。
			const glm::mat4 invView = glm::inverse(view);
			const glm::vec3 cameraPos(invView[3]);
			const D3D12_CPU_DESCRIPTOR_HANDLE terrainRtv = offscreen.rtvHandle();
			const D3D12_CPU_DESCRIPTOR_HANDLE terrainDsv = meshRenderer.dsvHandle();
			context.commandList()->OMSetRenderTargets(1, &terrainRtv, FALSE, &terrainDsv);
			context.commandList()->ClearDepthStencilView(terrainDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			// z-up 地形网格转 y-up（同角色 rotate -90 X）。采样高度用本地坐标不受影响。
			const glm::mat4 terrainModel = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f),
				glm::vec3(1.0f, 0.0f, 0.0f));
			terrainRenderer.render(proj * view, cameraPos, terrainMesh, terrainModel,
				sceneLightDir, glm::vec4(dirLightColor, dirLightIntensity)); // 0xFFEEDD00 + π
			meshRenderer.render(view, proj, items, terrainRtv, false);
			// 角色（CharacterRenderer）：outline 先画 + main pass，用同一离屏 RT + 深度。
			characterRenderer.render(view, proj, charItems, terrainRtv, meshRenderer.dsvHandle());

			// Boids（照搬参考 BoidInstancing.mat）：每帧 sim + 实例化绘制纸飞机群集。
			// depth 仍为 DEPTH_WRITE（水 pass 前），Boids 写深度被地形/物体遮挡。
			if (!paperPlaneModel.meshes().empty()) {
				boidsRenderer.render(view, proj, cameraPos, t, dt,
					paperPlaneModel.meshes()[0], terrainRtv, meshRenderer.dsvHandle());
			}

			// Grass（照搬参考 GrassInstancing.mat）：每帧实例生成 + ExecuteIndirect 间接绘制。
			// 相机前向必须从 view 矩阵第三行提取（GLM view[2] 是第三列，仅 +Z 朝向时 ≈ 前向，
			// 绕 X 旋转后（看 ±X）会偏 → 草网格被放到身后被剔除）。
			if (!grassMeshModel.meshes().empty()) {
				const glm::vec3 cameraFwdGrass = glm::vec3(-view[0][2], -view[1][2], -view[2][2]);
				grassRenderer.render(view, proj, cameraPos, cameraFwdGrass, t, dt,
					grassMeshModel.meshes()[0], terrainRtv, meshRenderer.dsvHandle());
			}

			// AnkhaFur（ShellFur 8 壳层毛）：全部角色之后绘制（fur 覆盖衣服——几何上限，未拆衣服）。
			if (!ankhaFurModel.meshes().empty()) {
				furRenderer.render(view, proj, ankhaBase, cameraPos,
					sceneLightDir, glm::vec4(dirLightColor, dirLightIntensity),
					ankhaFurModel.meshes()[0], terrainRtv, meshRenderer.dsvHandle());
			}

			// 水面（照搬参考 WaterMesh.mat）：渲染到同一离屏 RT。
			// 折射需要"当前场景色"→ 先把 offscreen 拷贝到 waterSceneCopy（offscreen RENDER_TARGET→COPY_SOURCE）。
			offscreen.transitionColor(D3D12_RESOURCE_STATE_COPY_SOURCE);
			waterRenderer.copySceneColor(offscreen.resource());
			offscreen.transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET); // 水 pass 写回离屏
			// 深度转 DEPTH_READ：水 pass 只读深度测试（被地形遮挡）+ 采样 depth（折射淡出）。
			meshRenderer.transitionDepthTo(D3D12_RESOURCE_STATE_DEPTH_READ);
		// near/far（glm RH 透视），同后处理段。
		const float waterNearZ = proj[3][2] / (proj[2][2] - 1.0f);
		const float waterFarZ = proj[3][2] / (proj[2][2] + 1.0f);
		// 相机 view 方向（同 Grass：第三行取反；-view[2] 第三列在 ±X 朝向时不是前向）。
		const glm::vec3 cameraFwd = glm::vec3(-view[0][2], -view[1][2], -view[2][2]);
			waterRenderer.render(terrainRtv, meshRenderer.dsvHandle(),
				proj * view, terrainModel, cameraPos, cameraFwd,
				sceneLightDir, glm::vec4(dirLightColor, dirLightIntensity),
				context.width(), context.height(), waterNearZ, waterFarZ, t);

			// 后处理：采样 sceneColor + depth → 输出到交换链后台缓冲。
			offscreen.transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			// depth 已 DEPTH_READ（水 pass 已转换），供雾采样。

			// 阶段 10b：Bloom 6 级金字塔（照搬参考 AfterglowBloomPassSet）。
			// 1) 6 级降采样：down[0]=1/2 ← offscreen, down[i] ← down[i-1]。
			for (UINT i = 0; i < 6; ++i) {
				const D3D12_GPU_DESCRIPTOR_HANDLE inSrv =
					(i == 0) ? offscreen.srvHandle() : down[i - 1].srvHandle();
				down[i].transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.commandList()->ClearRenderTargetView(down[i].rtvHandle(), bloomClear, 0, nullptr);
				downSamplingPass.render(sharedSrvHeap.Get(), inSrv, down[i].rtvHandle(),
					bloomW[i], bloomH[i]);
				down[i].transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			// 2) 从最细级（down[5]=1/64）向上组合：HorizontalBlur（乘 intensity）→ VerticalBlur + 叠加上一级 combined。
			//    每级 BloomParams cbuffer：{ bloomIntensity, useCombinedTexture, _pad }（16B 对齐）。
			struct BloomParams {
				glm::vec4 intensity;   // bloomIntensity
				float useCombined;     // 最小级 0，其余 1
				float _pad[3];
			};
			for (int i = 5; i >= 0; --i) {
				const UINT idx = static_cast<UINT>(i);
				// HorizontalBlur
				BloomParams hp{ bloomIntensity[idx], 0.0f, { 0.0f, 0.0f, 0.0f } };
				horz[idx].transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.commandList()->ClearRenderTargetView(horz[idx].rtvHandle(), bloomClear, 0, nullptr);
				horizontalBlurPass.render(sharedSrvHeap.Get(), down[idx].srvHandle(),
					horz[idx].rtvHandle(), bloomW[idx], bloomH[idx], {}, &hp, sizeof(hp));
				horz[idx].transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				// VerticalBlur + Combined（最小级 useCombined=0 无上一级；其余叠加 combined[idx+1]）
				BloomParams vp{ glm::vec4(1.0f), (idx == 5) ? 0.0f : 1.0f, { 0.0f, 0.0f, 0.0f } };
				combined[idx].transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.commandList()->ClearRenderTargetView(combined[idx].rtvHandle(), bloomClear, 0, nullptr);
				const D3D12_GPU_DESCRIPTOR_HANDLE prevCombined =
					(idx == 5) ? D3D12_GPU_DESCRIPTOR_HANDLE{} : combined[idx + 1].srvHandle();
				verticalBlurPass.render(sharedSrvHeap.Get(), horz[idx].srvHandle(),
					combined[idx].rtvHandle(), bloomW[idx], bloomH[idx], prevCombined, &vp, sizeof(vp));
				combined[idx].transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}

			context.transitionCurrentBackBuffer(D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			// 从 proj 提取 near/far（glm RH 透视）。cameraPos 已在场景渲染段计算。
			const float nearZ = proj[3][2] / (proj[2][2] - 1.0f);
			const float farZ = proj[3][2] / (proj[2][2] + 1.0f);
		{
			auto* device = context.device();
			const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			D3D12_GPU_DESCRIPTOR_HANDLE amb = sharedSrvHeap->GetGPUDescriptorHandleForHeapStart();
			amb.ptr += 4 * inc; // t4 ambient
			D3D12_GPU_DESCRIPTOR_HANDLE terr = sharedSrvHeap->GetGPUDescriptorHandleForHeapStart();
			terr.ptr += 5 * inc; // t5 TerrainHeight
			postProcess.render(sharedSrvHeap.Get(), offscreen.srvHandle(),
				combined[0].srvHandle(), meshRenderer.depthSrvHandle(), noiseUav.srvHandle(),
				amb, terr,
				context.currentRtv(), cameraPos, nearZ, farZ, t, view, glm::inverse(proj * view),
				sceneLightDir,
				glm::vec2(static_cast<float>(mouseX), static_cast<float>(mouseY)), // cursor test（参考 PostProcess_FS）
				postEffects);
		}
			context.transitionCurrentBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT);

			// depth 恢复为渲染写入状态，供下一帧场景深度测试。
			meshRenderer.transitionDepthTo(D3D12_RESOURCE_STATE_DEPTH_WRITE);

			context.endFrame();
		}

		context.waitForGpu();
		std::cout << "[AfterglowRender DX12] exited." << std::endl;
	} catch (const std::exception& e) {
		std::cerr << "[AfterglowRender DX12] fatal error: " << e.what() << std::endl;
		std::cout << "[AfterglowRender DX12] fatal error: " << e.what() << std::endl;
		OutputDebugStringA(("[AfterglowRender DX12] fatal error: " + std::string(e.what()) + "\n").c_str()); // VS 输出窗口可见
		{ // 额外写日志文件（trunc：每次运行覆盖，fatal.log 只保留本次错误，避免累积干扰诊断）
			std::ofstream log("fatal.log", std::ios::trunc);
			log << "[AfterglowRender DX12] fatal error: " << e.what() << std::endl;
		}
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
