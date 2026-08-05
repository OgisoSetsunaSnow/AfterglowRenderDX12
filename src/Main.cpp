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
			  &yvonneBody, &yvonneCloth1, &yvonneCloth2, &yvonneFace, &yvonneIris, &yvonneHair },
			256, outlineVs, outlinePs,
			DXGI_FORMAT_R16G16B16A16_FLOAT, sharedSrvHeap.Get(), 4, skyVs, skyPs); // HDR 离屏 + depth SRV(10d) + SkySphere

		// 阶段 8：方向光（左上角照向场景）。
		meshRenderer.setLight(glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f)),
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

		// ---- 场景物体（mesh + 位置 + 材质颜色） ----
		std::vector<DrawItem> items = {
			// SkySphere 天空盒（遗留 #1 修复）：SkySphere.fbx 半径 50000，scale 0.01 → 世界半径 500
			// （参考项目 SkySphere scale 0.01）。SkySphere shader 使球心跟随相机（相机永远在球内）。
			// 必须画在 items 最前（0 号），否则 disable-depth 球后画会覆盖所有物体。
			{ &skySphereModel.meshes()[0], glm::scale(glm::mat4(1.0f), glm::vec3(0.01f)),
				glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), 3, false, false, 0.0f, true },
			{ &meshes[0], glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
				glm::vec4(1.0f, 0.35f, 0.35f, 1.0f) },   // 立方体（中）
			{ &meshes[1], glm::translate(glm::mat4(1.0f), glm::vec3(-3.5f, 1.0f, 0.0f)),
				glm::vec4(0.35f, 1.0f, 0.45f, 1.0f) },   // 球（左）
			{ &meshes[3], glm::translate(glm::mat4(1.0f), glm::vec3(2.5f, 0.5f, 0.0f)),
				glm::vec4(0.45f, 0.55f, 1.0f, 1.0f) },   // 三棱柱（中右）
			{ &boxModel.meshes()[0], glm::translate(glm::mat4(1.0f), glm::vec3(4.5f, 1.0f, 0.0f)),
				glm::vec4(0.9f, 0.5f, 0.2f, 1.0f) },     // Box.fbx（底部对齐地面）
			{ &meshes[2], glm::mat4(1.0f),
				glm::vec4(0.55f, 0.55f, 0.55f, 1.0f) },  // 地面
			// 参考项目 shared models（远距离，验证雾效果 10d-1）
			{ &sphereModel.meshes()[0], glm::translate(glm::mat4(1.0f), glm::vec3(8.0f, 1.0f, -8.0f)),
				glm::vec4(0.3f, 0.7f, 0.9f, 1.0f) },    // 远景 Sphere（蓝）
			{ &prismModel.meshes()[0], glm::translate(glm::mat4(1.0f), glm::vec3(-7.0f, 0.5f, -9.0f)),
				glm::vec4(0.7f, 0.4f, 0.6f, 1.0f) },    // 远景 Prism（紫）
		{ &planeModel.meshes()[0], glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, -14.0f)),
			glm::vec4(0.45f, 0.45f, 0.5f, 1.0f) },   // 远景 Plane（灰）
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

			// 物体动画：立方体旋转、球自转、棱柱静止、地面固定。
			// SkySphere(items[0]) 固定原点不更新。
			items[1].model = glm::rotate(
				glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
				t * 0.8f, glm::vec3(0.4f, 1.0f, 0.2f));   // 立方体（中）
			items[2].model = glm::rotate(
				glm::translate(glm::mat4(1.0f), glm::vec3(-3.5f, 1.0f, 0.0f)),
				t * 0.6f, glm::vec3(0.0f, 1.0f, 0.0f));   // 球（左）

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
				context.currentRtv(), cameraPos, nearZ, farZ);
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
