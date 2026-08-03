#pragma once

#include <glm/glm.hpp>

// 自由相机（FPS 风格）：自身移动 + 转向，场景物体不动。
class FreeCamera {
public:
	glm::vec3 position{ 0.0f, 2.0f, 8.0f };
	float yaw = -90.0f;    // 度，初始朝向 -Z
	float pitch = 0.0f;
	float moveSpeed = 5.0f;
	float mouseSensitivity = 0.15f;

	glm::vec3 forward() const;
	glm::mat4 viewMatrix() const;

	// 右键拖拽旋转朝向。
	void rotate(float dx, float dy);

	// 沿前/侧/上方向移动，axis 取 -1..1。
	void move(float forwardAxis, float strafeAxis, float upAxis, float dt);
};
