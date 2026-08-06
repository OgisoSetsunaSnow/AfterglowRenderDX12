#include "Camera.h"
#include "D3D12Core.h"
#include "FreeCamera.h"
#include "FullScreenPass.h"
#include "Mesh.h"
#include "MeshRenderer.h"
#include "Model.h"
#include "OffscreenRT.h"
#include "PostProcess.h"
#include "ShaderCompiler.h"
#include "Texture.h"
#include "Window.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

int main() {
	try {
		Window window(L"AfterglowRender DX12", 1280, 720);
		D3D12Context context(window);

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

		// SkySphere 天空贴图（参考项目 Shared/Textures/DefaultSky.png）。
		Texture skyTexture;
		skyTexture.load(context, "../vsbuild/Assets/Shared/Textures/DefaultSky.png");
		std::cout << "[AfterglowRender DX12] loaded sky texture "
			<< skyTexture.width() << "x" << skyTexture.height() << std::endl;

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

		// Ankha（第三个角色）：albedo 纹理（参考项目 Ankha*.mati 的 albedoTex 引用）。
		Texture ankhaBody, ankhaDress, ankhaEye, ankhaHair;
		const char* ankTexDir = "../vsbuild/Assets/Characters/Ankha/Textures/";
		ankhaBody.load(context, std::string(ankTexDir) + "BODY TEXTURE.png");        // mesh 1/2/3/5/6/8/10
		ankhaDress.load(context, std::string(ankTexDir) + "DRESS TEXTURE.png");      // mesh 0
		ankhaEye.load(context, std::string(ankTexDir) + "EYE TEXTURE.png");          // mesh 4/7
		ankhaHair.load(context, std::string(ankTexDir) + "HAIR SKATER TEXTURE.png"); // mesh 9
		std::cout << "[AfterglowRender DX12] loaded Ankha textures "
			<< ankhaBody.width() << "x" << ankhaBody.height() << std::endl;

		// 共享 SRV 堆：D3D12 同一时刻只能设置一个 CBV_SRV_UAV 堆，所有 RT 的 SRV 放一起。
		// index 0=offscreen, 1=downSampled, 2=horizontalBlur, 3=combined, 4=depth(10d)。
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sharedSrvHeap;
		D3D12_DESCRIPTOR_HEAP_DESC sharedDesc{};
		sharedDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		sharedDesc.NumDescriptors = 5;
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
			DXGI_FORMAT_R16G16B16A16_FLOAT, sharedSrvHeap.Get(), 4, skyVs, skyPs); // HDR 离屏 + depth SRV(10d) + SkySphere

		// 阶段 8：方向光（左上角照向场景）。sceneLightDir 供光轴（SkylightShaft）用。
		const glm::vec3 sceneLightDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
		meshRenderer.setLight(sceneLightDir,
			glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(0.12f, 0.12f, 0.15f));

		// 阶段 10：离屏 sceneColor + 后处理。
		OffscreenRT offscreen;
		offscreen.create(context, 0, 0, sharedSrvHeap.Get(), 0, nullptr);
		PostProcess postProcess(context, postVs, postPs);

		// 阶段 10b：Bloom（1/2 分辨率降采样 RT + 三个 fullscreen passes）。
		const UINT halfW = context.width() / 2;
		const UINT halfH = context.height() / 2;
		const float bloomClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		OffscreenRT downSampled;
		downSampled.create(context, halfW, halfH, sharedSrvHeap.Get(), 1, bloomClear);
		OffscreenRT horizontalBlur;
		horizontalBlur.create(context, halfW, halfH, sharedSrvHeap.Get(), 2, bloomClear);
		OffscreenRT combined;
		combined.create(context, halfW, halfH, sharedSrvHeap.Get(), 3, bloomClear);

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
			// 地面。
			{ &meshes[2], glm::mat4(1.0f),
				glm::vec4(0.55f, 0.55f, 0.55f, 1.0f) },
		};

		// BattleMage：放场景后方（相机看向 -Z 方向），正面绕 X 转 -90° 朝 +Z 面向相机。
		const glm::mat4 battleMageBase = glm::rotate(
			glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)),
				glm::vec3(0.01f)),
			glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		// 按材质名绑定纹理：base→body、decal→decal、其余回退 body。
		// 对齐参考项目：outline 是材质级（BattleMage 有 BattleMageOutline 材质），仅角色模型开启描边。
		for (std::size_t i = 0; i < battleMageModel.meshes().size(); ++i) {
			UINT srvIndex = 1; // 默认身体
			const std::string& matName = battleMageModel.materialNames()[i];
			if (matName.find("Decal") != std::string::npos || matName.find("decal") != std::string::npos) {
				srvIndex = 2;
			}
			// BattleMageOutline.mati: outline on all meshes (hair/双马尾 in mesh2/3).
			// SkySphere LESS 修复后 outline 不再被天空盒覆盖.
			// width 5 (thinner), black.
			items.push_back({ &battleMageModel.meshes()[i], battleMageBase,
				glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), srvIndex, true, false, 5.0f,
				false, glm::vec3(0.0f, 0.0f, 0.0f) }); // all meshes outline, width 5, black
		}

		// Yvonne：按 mesh index 绑定 albedo 纹理（对齐参考项目 yvonneMesh.setMaterial 的 mesh 顺序）。
		// 纹理 srvIndex：4=body, 5=cloth1, 6=cloth2, 7=face, 8=iris, 9=hair。
		// mesh 6 EyeShadow / mesh 11 HairShadow 在参考项目无 albedo 纹理 → 用深色 tint 近似纯色。
		// 描边（对齐参考项目 pass1 材质）：mesh 0 Body / 2 Cloth / 3 Tail / 5 Cloth / 10 Hair 挂 outline。
		// outlineWidth=16, outlineColor：Body(0.12,0.02,0.04) / Cloth(0,0,0) / Hair(0.05,0,0.08)。mesh4 ThinFilm 参考项目用 Ignore 不描边。
		const glm::mat4 yvonneBase = glm::rotate(
			glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f, 0.0f, 5.0f)),
			glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		static const UINT yvSrv[12] = { 4, 4, 5, 6, 5, 5, 4, 7, 7, 8, 9, 9 };
		static const bool yvOutlineOn[12] = { true, false, true, true, false, true, false, false, false, false, true, false };
		static const glm::vec3 yvOutlineColor[12] = {
			glm::vec3(0.12f, 0.02f, 0.04f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f),
			glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f),
			glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.05f, 0.0f, 0.08f), glm::vec3(0.0f)
		};
		for (std::size_t i = 0; i < yvonneModel.meshes().size(); ++i) {
			const UINT srv = (i < 12) ? yvSrv[i] : 4;
			glm::vec4 tint(1.0f);
			if (i == 6) tint = glm::vec4(0.05f, 0.04f, 0.05f, 1.0f); // EyeShadow
			if (i == 11) tint = glm::vec4(0.02f, 0.002f, 0.0f, 1.0f); // HairShadow (shadowColor)
			const bool outlineOn = (i < 12) ? yvOutlineOn[i] : false;
			items.push_back({ &yvonneModel.meshes()[i], yvonneBase, tint, srv,
				outlineOn, false, 5.0f, false, yvOutlineColor[i] }); // width 5 (thinner)
		}

		// Ankha（第三个角色，参考项目 scale 0.02, translation {0,6,0.5}）。
		// mesh 材质映射：0 Dress, 1/2/3 Body, 4 Eye, 5/6 Body, 7 Eye, 8 Body, 9 Hair, 10 Body。
		// 参考项目 Ankha 无 outline 材质 pass → 不描边。AnkhaFur.fbx（毛茸茸，instancing）本工程暂不支持。
		const glm::mat4 ankhaBase = glm::rotate(
			glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, 5.0f)),
				glm::vec3(0.02f)),
			glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		static const UINT ankSrv[11] = { 11, 10, 10, 10, 12, 10, 10, 12, 10, 13, 10 };
		for (std::size_t i = 0; i < ankhaModel.meshes().size(); ++i) {
			const UINT srv = (i < 11) ? ankSrv[i] : 10;
			items.push_back({ &ankhaModel.meshes()[i], ankhaBase,
				glm::vec4(1.0f), srv, false, false, 0.0f });
		}

		// PaperAirplane（在天空飞，参考项目 Boids 是 compute 群集动画，本工程用 CPU 圆周轨迹近似）。
		// DebugBox.fbx 移除：测试用的 1x1x1 棋盘盒子，无实际用途。
		const std::size_t paperPlaneIndex = items.size();
		if (!paperPlaneModel.meshes().empty()) {
			items.push_back({ &paperPlaneModel.meshes()[0],
				glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(6.0f, 3.0f, -4.0f)),
					glm::vec3(0.2f)),
				glm::vec4(1.0f, 0.9f, 0.5f, 1.0f), 0, false, false, 0.0f }); // 米黄
		}

		// 相机：F 键在轨道/自由之间切换。
		enum class CameraMode { Orbit, Free };
		CameraMode cameraMode = CameraMode::Free;

		OrbitCamera orbitCamera;
		orbitCamera.setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
		orbitCamera.setDistance(7.0f);

		FreeCamera freeCamera;

		double lastMouseX = 0.0, lastMouseY = 0.0;
		window.getCursorPos(lastMouseX, lastMouseY);
		bool fKeyPrev = false;

		// 10d-2 调试：数字键切换后处理效果（1=色差 2=尘埃 3=暗角 4=全开 0=关）。
		std::uint32_t postEffects = 0x7u;
		bool fxKeyPrev[4] = { false, false, false, false }; // 1/2/3/0

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

			// 物体动画：纸飞机（环形轨迹）更新；SkySphere/地面/角色固定。

			// 纸飞机环形飞行（参考项目 Boids 是 compute 群集，本工程用 CPU 圆周轨迹近似）。
			// 绕 Y 旋转使机头朝向运动方向。
			if (paperPlaneIndex < items.size()) {
				const float ang = t * 0.6f;
				items[paperPlaneIndex].model = glm::rotate(
					glm::scale(glm::translate(glm::mat4(1.0f),
						glm::vec3(std::cos(ang) * 5.0f,
							2.5f + std::sin(t * 0.5f),
							std::sin(ang) * 5.0f - 3.0f)),
						glm::vec3(0.2f)),
					ang + glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			}

			const glm::mat4 proj = (cameraMode == CameraMode::Orbit)
				? orbitCamera.projMatrix(static_cast<float>(context.width()) / static_cast<float>(context.height()))
				: glm::perspective(glm::radians(60.0f),
					static_cast<float>(context.width()) / static_cast<float>(context.height()),
					0.01f, 1000.0f);

			// 阶段 10：场景渲染到离屏 sceneColor（HDR）。
			offscreen.transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float clearColor[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
			context.commandList()->ClearRenderTargetView(offscreen.rtvHandle(), clearColor, 0, nullptr);
			meshRenderer.render(view, proj, items, offscreen.rtvHandle());

			// 后处理：采样 sceneColor + depth → 输出到交换链后台缓冲。
			offscreen.transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			meshRenderer.transitionDepthTo(D3D12_RESOURCE_STATE_DEPTH_READ); // depth 供雾采样

			// 阶段 10b：Bloom（DownSampling → HorizontalBlur → VerticalBlur/Combined）。
			downSampled.transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.commandList()->ClearRenderTargetView(downSampled.rtvHandle(), bloomClear, 0, nullptr);
			downSamplingPass.render(sharedSrvHeap.Get(), offscreen.srvHandle(),
				downSampled.rtvHandle(), halfW, halfH);
			downSampled.transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			horizontalBlur.transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.commandList()->ClearRenderTargetView(horizontalBlur.rtvHandle(), bloomClear, 0, nullptr);
			horizontalBlurPass.render(sharedSrvHeap.Get(), downSampled.srvHandle(),
				horizontalBlur.rtvHandle(), halfW, halfH);
			horizontalBlur.transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			combined.transitionColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.commandList()->ClearRenderTargetView(combined.rtvHandle(), bloomClear, 0, nullptr);
			verticalBlurPass.render(sharedSrvHeap.Get(), horizontalBlur.srvHandle(),
				combined.rtvHandle(), halfW, halfH);
			combined.transitionColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			context.transitionCurrentBackBuffer(D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			const glm::mat4 invView = glm::inverse(view);
			const glm::vec3 cameraPos(invView[3]);
			// 从 proj 提取 near/far（glm RH 透视）。
			const float nearZ = proj[3][2] / (proj[2][2] - 1.0f);
			const float farZ = proj[3][2] / (proj[2][2] + 1.0f);
		postProcess.render(sharedSrvHeap.Get(), offscreen.srvHandle(),
			combined.srvHandle(), meshRenderer.depthSrvHandle(),
			context.currentRtv(), cameraPos, nearZ, farZ, t, view, sceneLightDir, postEffects); // t=动画时间, view/lightDir 供光轴
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
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
