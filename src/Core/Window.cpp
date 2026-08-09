#include "Window.h"

#include <windows.h>

#include <cstdlib>
#include <mutex>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace {

std::once_flag g_glfwInitFlag;

void initGlfw() {
	if (glfwInit() != GLFW_TRUE) {
		throw WindowError("Failed to initialize GLFW.");
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

} // namespace

Window::Window(const std::wstring& title, uint32_t width, uint32_t height)
	: _width(width), _height(height) {
	try {
		std::call_once(g_glfwInitFlag, initGlfw);
	} catch (const std::exception& e) {
		throw WindowError(e.what());
	}

	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	// 将宽字符标题转换为 UTF-8（WideCharToMultiByte，支持非 ASCII）。
	std::string titleUtf8;
	if (!title.empty()) {
		const int len = WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (len > 0) {
			titleUtf8.resize(static_cast<std::size_t>(len) - 1); // 去掉结尾 NUL
			WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, titleUtf8.data(), len, nullptr, nullptr);
		}
	}
	_window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
		titleUtf8.empty() ? nullptr : titleUtf8.c_str(), nullptr, nullptr);
	if (!_window) {
		throw WindowError("Failed to create GLFW window.");
	}

	glfwSetWindowUserPointer(_window, this);
	glfwSetWindowSizeCallback(_window, resizeCallback);
	glfwSetScrollCallback(_window, scrollCallback);
}

Window::~Window() {
	if (_window) {
		glfwDestroyWindow(_window);
		_window = nullptr;
	}
}

void* Window::nativeHandle() const {
	return glfwGetWin32Window(_window);
}

void Window::resizeCallback(GLFWwindow* win, int /*w*/, int /*h*/) {
	auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
	if (self) {
		// 尺寸变化在渲染侧通过 getFramebufferSize 获取，这里只做标记。
		// swapchain resize 由渲染线程在下一帧检测。
	}
}

void Window::scrollCallback(GLFWwindow* win, double /*xoff*/, double yoff) {
	auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
	if (self) {
		self->_scrollOffset += yoff;
	}
}

bool Window::isKeyPressed(int key) const {
	return glfwGetKey(_window, key) == GLFW_PRESS;
}

bool Window::isMouseButtonPressed(int button) const {
	return glfwGetMouseButton(_window, button) == GLFW_PRESS;
}

void Window::getCursorPos(double& x, double& y) const {
	glfwGetCursorPos(_window, &x, &y);
}

double Window::consumeScrollOffset() {
	const double value = _scrollOffset;
	_scrollOffset = 0.0;
	return value;
}
