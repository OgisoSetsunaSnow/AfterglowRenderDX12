#pragma once

#include <glm/glm.hpp>

// 自由相机（FPS 风格）：自身移动 + 转向，场景物体不动。
class FreeCamera {
public:
	// 对齐参考项目 MainCamera（z-up (0,2,1) → y-up (0,1,-2)），target BattleMage+(0,0,0.8) → (0,0.8,0)。
	glm::vec3 position{ 0.0f, 1.0f, -2.0f };
	float yaw = 90.0f;     // 度，朝向 +Z（看向 BattleMage）
	float pitch = -6.0f;   // 度，略俯视 target (0,0.8,0)
	float moveSpeed = 5.0f;
	float mouseSensitivity = 0.15f;

	glm::vec3 forward() const;
	glm::mat4 viewMatrix() const;

	// 右键拖拽旋转朝向。
	void rotate(float dx, float dy);

	// 沿前/侧/上方向移动，axis 取 -1..1。
	void move(float forwardAxis, float strafeAxis, float upAxis, float dt);
};
