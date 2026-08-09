#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

class D3D12Context;

// ImGui 渲染器（照搬参考项目 AfterglowGUI：imgui 1.92.3 + imgui_impl_glfw + imgui_impl_dx12）。
// 样式照搬参考 AfterglowGUI.cpp：StyleColorsLight + 圆角 16 + 白底半透明 + calibrib.ttf 14px。
// 输入照搬参考：imgui_impl_glfw InitForVulkan(install_callbacks=false) + 手动转发回调。
// D3D12 后端：独立 shader-visible SRV 堆（font texture），绘制在交换链后台缓冲（后处理之后）。
class GuiRenderer {
public:
	GuiRenderer(D3D12Context& context, GLFWwindow* window);
	~GuiRenderer();

	GuiRenderer(const GuiRenderer&) = delete;
	GuiRenderer& operator=(const GuiRenderer&) = delete;

	// 帧开始（ImGui_ImplDX12_NewFrame + Glfw_NewFrame + NewFrame）。之后调用者用 ImGui API 构建 UI，再调 render()。
	void beginFrame();
	// 帧结束：ImGui::Render + ImGui_ImplDX12_RenderDrawData（后台缓冲须为 RENDER_TARGET 状态）。
	void render();

	// ImGui 是否想捕获鼠标/键盘（GUI hover 或 InputText 聚焦时），相机/快捷键应跳过。
	bool wantsCaptureMouse() const;
	bool wantsCaptureKeyboard() const;

	// 滚轮累计偏移（imgui 回调接管 GLFW scroll 后，相机 zoom 从这里取）。
	double consumeScrollOffset();

	const std::string& deviceName() const { return _deviceName; }

	// Console 面板开关（`~` 键切换，参考 AfterglowGUI::showConsolePanel）。
	bool consoleOpen = true;

private:
	static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
	static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
	static void cursorPosCallback(GLFWwindow* w, double x, double y);
	static void scrollCallback(GLFWwindow* w, double xoff, double yoff);
	static void charCallback(GLFWwindow* w, unsigned int codepoint);

	D3D12Context& _context;
	GLFWwindow* _window = nullptr;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _srvHeap;
	double _scrollOffset = 0.0;
	std::string _deviceName;
};
