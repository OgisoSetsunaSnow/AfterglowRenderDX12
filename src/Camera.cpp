#include "Camera.h"

#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

glm::vec3 OrbitCamera::forward() const {
	const float yawRad = glm::radians(_yaw);
	const float pitchRad = glm::radians(_pitch);
	return glm::normalize(glm::vec3(
		std::cos(pitchRad) * std::sin(yawRad),
		std::sin(pitchRad),
		std::cos(pitchRad) * std::cos(yawRad)));
}

glm::vec3 OrbitCamera::position() const {
	return _target - forward() * _distance;
}

glm::mat4 OrbitCamera::viewMatrix() const {
	return glm::lookAt(position(), _target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projMatrix(float aspect) const {
	// near/far 对齐参考 AfterglowCameraComponent（0.1 / 1000）。
	return glm::perspective(glm::radians(_fov), aspect, 0.1f, 1000.0f);
}
