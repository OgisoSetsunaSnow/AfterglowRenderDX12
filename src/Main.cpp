#include "Camera.h"
#include "D3D12Core.h"
#include "FreeCamera.h"
#include "Mesh.h"
#include "MeshRenderer.h"
#include "Model.h"
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

		// ---- 创建网格 ----
		std::vector<Mesh> meshes(4);
		meshes[0].create(context, Mesh::makeCubeVertices(), Mesh::makeCubeIndices());
		meshes[1].create(context, Mesh::makeSphereVertices(16, 24), Mesh::makeSphereIndices(16, 24));
		meshes[2].create(context, Mesh::makePlaneVertices(10.0f), Mesh::makePlaneIndices());
		meshes[3].create(context, Mesh::makePrismVertices(), Mesh::makePrismIndices());

		MeshRenderer meshRenderer(context, vs, ps,
			{ &texture, &battleMageBody, &battleMageDecal }, 256);

		// ---- 加载参考项目模型 ----
		Model boxModel;
		boxModel.load(context, "../vsbuild/Assets/Shared/Models/Box.fbx");
		std::cout << "[AfterglowRender DX12] loaded Box.fbx: "
			<< boxModel.meshes().size() << " mesh(es)" << std::endl;

		Model battleMageModel;
		battleMageModel.load(context, "../vsbuild/Assets/Characters/BattleMage/BattleMage.fbx");
		std::cout << "[AfterglowRender DX12] loaded BattleMage.fbx: "
			<< battleMageModel.meshes().size() << " mesh(es)" << std::endl;
		for (std::size_t i = 0; i < battleMageModel.materialNames().size(); ++i) {
			std::cerr << "  mesh[" << i << "] material='"
				<< battleMageModel.materialNames()[i] << "'" << std::endl;
		}

		// ---- 场景物体（mesh + 位置 + 材质颜色） ----
		std::vector<DrawItem> items = {
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
		};

		// BattleMage：放场景后方（相机看向 -Z 方向），正面绕 X 转 -90° 朝 +Z 面向相机。
		const glm::mat4 battleMageBase = glm::rotate(
			glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)),
				glm::vec3(0.01f)),
			glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		// 按材质名绑定纹理：base→body、decal→decal、其余回退 body。
		for (std::size_t i = 0; i < battleMageModel.meshes().size(); ++i) {
			UINT srvIndex = 1; // 默认身体
			const std::string& matName = battleMageModel.materialNames()[i];
			if (matName.find("Decal") != std::string::npos || matName.find("decal") != std::string::npos) {
				srvIndex = 2;
			}
			items.push_back({ &battleMageModel.meshes()[i], battleMageBase,
				glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), srvIndex });
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
			items[0].model = glm::rotate(
				glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
				t * 0.8f, glm::vec3(0.4f, 1.0f, 0.2f));
			items[1].model = glm::rotate(
				glm::translate(glm::mat4(1.0f), glm::vec3(-3.5f, 1.0f, 0.0f)),
				t * 0.6f, glm::vec3(0.0f, 1.0f, 0.0f));

			const glm::mat4 proj = (cameraMode == CameraMode::Orbit)
				? orbitCamera.projMatrix(static_cast<float>(context.width()) / static_cast<float>(context.height()))
				: glm::perspective(glm::radians(60.0f),
					static_cast<float>(context.width()) / static_cast<float>(context.height()),
					0.01f, 1000.0f);

			context.transitionCurrentBackBuffer(D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float clearColor[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
			context.commandList()->ClearRenderTargetView(context.currentRtv(), clearColor, 0, nullptr);
			meshRenderer.render(view, proj, items);
			context.transitionCurrentBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT);

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
