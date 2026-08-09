#include "GuiRenderer.h"
#include "D3D12Core.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_dx12.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>

GuiRenderer::GuiRenderer(D3D12Context& context, GLFWwindow* window)
	: _context(context), _window(window) {
	auto* device = context.device();

	// 设备名（RenderStatus 面板显示）。
	{
		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))
			&& SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
			DXGI_ADAPTER_DESC desc{};
			if (SUCCEEDED(adapter->GetDesc(&desc))) {
				char buf[128]{};
				WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
				_deviceName = buf;
			}
		}
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	// 样式照搬参考项目 AfterglowGUI.cpp：浅色圆角半透明。
	ImGuiStyle& style = ImGui::GetStyle();
	ImGui::StyleColorsLight();
	style.ScaleAllSizes(1.2f);
	style.Colors[ImGuiCol_WindowBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.8f);
	style.WindowPadding = ImVec2(16.0f, 16.0f);
	style.WindowRounding = 16.0f;
	style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	style.ChildRounding = 12.0f;
	style.PopupRounding = 16.0f;
	style.FramePadding = ImVec2(8.0f, 5.0f);
	style.FrameRounding = 16.0f;
	style.GrabRounding = 12.0f;

	// 字体（参考 font::defaultFontPath = Assets/Shared/Fonts/calibrib.ttf，14px）。
	ImFontConfig fontConfig;
	ImFont* font = io.Fonts->AddFontFromFileTTF("vsbuild/Assets/Shared/Fonts/calibrib.ttf", 14.0f, &fontConfig);
	if (font) {
		io.FontDefault = font;
	}

	// 合并中文字体（Windows 自带微软雅黑/黑体，覆盖中文歌名等显示）。
	// calibrib.ttf 无中文 glyph → 不合并时中文显示豆腐。系统字体路径探测，不存在则跳过。
	{
		const char* cnCandidates[] = {
			"C:\\Windows\\Fonts\\msyh.ttc",   // 微软雅黑（TTC 集合，imgui 取第一个 face=Regular）
			"C:\\Windows\\Fonts\\simhei.ttf", // 黑体（单 TTF，最稳）
			"C:\\Windows\\Fonts\\simsun.ttc", // 宋体
		};
		for (const char* cnPath : cnCandidates) {
			if (!std::filesystem::exists(cnPath)) {
				continue;
			}
			ImFontConfig mergeConfig;
			mergeConfig.MergeMode = true;
			mergeConfig.OversampleH = mergeConfig.OversampleV = 1; // 中文字体大，省内存
			if (io.Fonts->AddFontFromFileTTF(cnPath, 14.0f, &mergeConfig,
					io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
				std::cout << "[AfterglowRender DX12] imgui: merged CN font " << std::filesystem::path(cnPath).filename().string() << std::endl;
				break;
			}
		}
	}

	// 输入：参考项目 AfterglowGUI 用 InitForVulkan(false) + 手动转发回调（不覆盖引擎自身回调）。
	// GLFW 每事件类型仅一个回调，本类统一接管并转发给 imgui_impl_glfw。
	ImGui_ImplGlfw_InitForVulkan(_window, false);
	glfwSetWindowUserPointer(_window, this);
	glfwSetKeyCallback(_window, keyCallback);
	glfwSetMouseButtonCallback(_window, mouseButtonCallback);
	glfwSetCursorPosCallback(_window, cursorPosCallback);
	glfwSetScrollCallback(_window, scrollCallback);
	glfwSetCharCallback(_window, charCallback);

	// D3D12 后端：独立 shader-visible SRV 堆（font texture atlas）。num_frames_in_flight = 交换链后台缓冲数 3。
	D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = 1;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&_srvHeap)))) {
		throw std::runtime_error("Failed to create imgui SRV heap");
	}
	ImGui_ImplDX12_InitInfo initInfo{};
	initInfo.Device = device;
	initInfo.CommandQueue = context.commandQueue();
	initInfo.NumFramesInFlight = 3;
	initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	initInfo.SrvDescriptorHeap = _srvHeap.Get();
	// 1.92 迁移接口：单 SRV 描述符（font texture）走 Legacy 字段。
	initInfo.LegacySingleSrvCpuDescriptor = _srvHeap->GetCPUDescriptorHandleForHeapStart();
	initInfo.LegacySingleSrvGpuDescriptor = _srvHeap->GetGPUDescriptorHandleForHeapStart();
	if (!ImGui_ImplDX12_Init(&initInfo)) {
		throw std::runtime_error("Failed to init imgui DX12 backend");
	}
}

GuiRenderer::~GuiRenderer() {
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
}

void GuiRenderer::beginFrame() {
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	// `~` 键切换 Console 面板（参考 AfterglowGUI::showConsolePanel）。
	if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
		consoleOpen = !consoleOpen;
	}
}

void GuiRenderer::render() {
	ImGui::Render();
	ImDrawData* drawData = ImGui::GetDrawData();
	if (drawData) {
		auto* cl = _context.commandList();
		// imgui_impl_dx12 的 RenderDrawData 不自行设置 descriptor heap / render target
		//（参考其官方示例：调用前需 SetDescriptorHeaps + OMSetRenderTargets）。
		// 否则 SetGraphicsRootDescriptorTable 用本类 SRV 堆的 handle 会与当前绑定的堆不一致 → ERROR #708。
		ID3D12DescriptorHeap* const heaps[] = { _srvHeap.Get() };
		cl->SetDescriptorHeaps(1, heaps);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _context.currentRtv();
		cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		ImGui_ImplDX12_RenderDrawData(drawData, cl);
	}
}

bool GuiRenderer::wantsCaptureMouse() const {
	return ImGui::GetIO().WantCaptureMouse;
}

bool GuiRenderer::wantsCaptureKeyboard() const {
	return ImGui::GetIO().WantCaptureKeyboard;
}

double GuiRenderer::consumeScrollOffset() {
	const double value = _scrollOffset;
	_scrollOffset = 0.0;
	return value;
}

void GuiRenderer::keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods) {
	ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
}

void GuiRenderer::mouseButtonCallback(GLFWwindow* w, int button, int action, int mods) {
	ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
}

void GuiRenderer::cursorPosCallback(GLFWwindow* w, double x, double y) {
	ImGui_ImplGlfw_CursorPosCallback(w, x, y);
}

void GuiRenderer::scrollCallback(GLFWwindow* w, double xoff, double yoff) {
	// 保留相机 zoom 用的滚轮累计（Window 的 scrollCallback 已被本类接管）。
	auto* self = static_cast<GuiRenderer*>(glfwGetWindowUserPointer(w));
	if (self) {
		self->_scrollOffset += yoff;
	}
	ImGui_ImplGlfw_ScrollCallback(w, xoff, yoff);
}

void GuiRenderer::charCallback(GLFWwindow* w, unsigned int codepoint) {
	ImGui_ImplGlfw_CharCallback(w, codepoint);
}
