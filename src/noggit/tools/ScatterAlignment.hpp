#pragma once

#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>

namespace Noggit::ScatterAlignment
{
  inline glm::vec3 rotation(glm::vec3 normal, float yaw)
  {
    float const length = glm::length(normal);
    if (!std::isfinite(length) || length < 1e-6f) return {0, yaw, 0};
    normal /= length;
    if (normal.y < 0) normal = -normal;
    if (glm::length(normal - glm::vec3(0, 1, 0)) < 1e-6f) return {0, yaw, 0};

    // Apply tilt after the existing heading, preserving rotation around the
    // surface normal. Match SceneObject's YZX order and its 90-degree offset.
    auto tilt = glm::rotation(glm::vec3(0, 1, 0), normal);
    auto matrix = glm::toMat4(tilt) * glm::eulerAngleYZX(glm::radians(yaw - 90.0f), 0.0f, 0.0f);
    float y, z, x;
    glm::extractEulerAngleYZX(matrix, y, z, x);
    return {-glm::degrees(z), glm::degrees(y) + 90.0f, glm::degrees(x)};
  }
}
