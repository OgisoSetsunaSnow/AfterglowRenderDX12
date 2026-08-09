#include "FreeCamera.h"

#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

glm::vec3 FreeCamera::forward() const {
	const float yawRad = glm::radians(yaw);
	const float pitchRad = glm::radians(pitch);
	return glm::normalize(glm::vec3(
		std::cos(pitchRad) * std::cos(yawRad),
		std::sin(pitchRad),
		std::cos(pitchRad) * std::sin(yawRad)));
}

glm::mat4 FreeCamera::viewMatrix() const {
	return glm::lookAt(position, position + forward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

void FreeCamera::rotate(float dx, float dy) {
	yaw += dx * mouseSensitivity;
	pitch += dy * mouseSensitivity;
	pitch = glm::clamp(pitch, -89.0f, 89.0f);
}

void FreeCamera::move(float forwardAxis, float strafeAxis, float upAxis, float dt) {
	// 水平前向（忽略 pitch，避免飞行时斜向加速）。
	const glm::vec3 f = glm::normalize(glm::vec3(forward().x, 0.0f, forward().z));
	const glm::vec3 right = glm::normalize(glm::cross(f, glm::vec3(0.0f, 1.0f, 0.0f)));
	position += f * (forwardAxis * moveSpeed * dt);
	position += right * (strafeAxis * moveSpeed * dt);
	position += glm::vec3(0.0f, 1.0f, 0.0f) * (upAxis * moveSpeed * dt);
}
