#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdint>
#include <stdexcept>
#include <string>

// 基于 glfw 的窗口封装，兼容后续 D3D12 swapchain 与 imgui 集成。
class Window {
public:
	Window(const std::wstring& title, uint32_t width, uint32_t height);
	~Window();

	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	bool shouldClose() const { return glfwWindowShouldClose(_window); }
	void update() {
		glfwPollEvents();
		glfwSwapBuffers(_window);
	}

	void requestClose() { glfwSetWindowShouldClose(_window, GLFW_TRUE); }

	GLFWwindow* handle() const { return _window; }
	uint32_t width() const { return _width; }
	uint32_t height() const { return _height; }

	// 当前帧缓冲尺寸（考虑 DPI 缩放），供 swapchain 使用。
	void getFramebufferSize(uint32_t& w, uint32_t& h) const {
		int ww, hh;
		glfwGetFramebufferSize(_window, &ww, &hh);
		w = static_cast<uint32_t>(ww);
		h = static_cast<uint32_t>(hh);
	}

	// 原生 HWND，供 DXGI swapchain 创建使用。
	void* nativeHandle() const;

	// ---- 输入 ----
	bool isKeyPressed(int key) const;
	bool isMouseButtonPressed(int button) const;
	void getCursorPos(double& x, double& y) const;
	// 返回自上次调用以来的滚轮累计偏移（正值向上滚）。
	double consumeScrollOffset();

private:
	static void resizeCallback(GLFWwindow* win, int w, int h);
	static void scrollCallback(GLFWwindow* win, double xoff, double yoff);

	GLFWwindow* _window = nullptr;
	uint32_t _width = 0;
	uint32_t _height = 0;
	double _scrollOffset = 0.0;
};

class WindowError : public std::runtime_error {
public:
	explicit WindowError(const std::string& what) : std::runtime_error(what) {}
};
