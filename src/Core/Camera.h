#pragma once

#include <glm/glm.hpp>

// 轨道相机：围绕目标点旋转/缩放/平移，用于自由观察场景。
class OrbitCamera {
public:
	void setTarget(const glm::vec3& target) { _target = target; }
	void setDistance(float distance) { _distance = glm::clamp(distance, 0.5f, 500.0f); }
	void setFov(float fovDegrees) { _fov = fovDegrees; }

	// 左键拖拽：dx/dy 为屏幕像素位移。
	void rotate(float dx, float dy) {
		_yaw += dx * 0.25f;
		_pitch += dy * 0.25f;
		_pitch = glm::clamp(_pitch, -89.0f, 89.0f);
	}

	// 滚轮缩放：delta 为正表示远离目标。
	void zoom(float wheelDelta) {
		_distance = glm::clamp(_distance * (1.0f + wheelDelta * 0.12f), 0.5f, 500.0f);
	}

	// 中键拖拽平移目标点（屏幕空间像素位移）。
	void pan(float dx, float dy) {
		const float scale = 0.005f * _distance;
		const glm::vec3 right = glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
		const glm::vec3 up = glm::normalize(glm::cross(right, forward()));
		_target += right * (-dx * scale) + up * (dy * scale);
	}

	glm::vec3 position() const;
	glm::mat4 viewMatrix() const;
	glm::mat4 projMatrix(float aspect) const;

private:
	glm::vec3 forward() const;

	glm::vec3 _target{ 0.0f, 0.0f, 0.0f };
	float _distance = 7.0f;
	float _yaw = 0.0f;    // 水平角（度），朝向 +Z（forward.z=cos(yaw)>0，看向 BattleMage/地形）
	float _pitch = -6.0f; // 俯仰角（度），近平视略俯视（参考 MainCamera z-up(0,2,1) target(0,0,0.8)：相机 y=1 在 target y=0.8 上方）
	float _fov = 45.0f;   // 对齐参考项目 AfterglowCameraComponent fovY = 45°
};
