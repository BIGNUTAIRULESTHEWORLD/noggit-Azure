// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Noggit::MissingObjectPlaceholder
{
  inline constexpr char m2_model_path[] = "spells/errorcube.m2";
  inline constexpr char wmo_model_path[] = "spells/unitcube.m2";

  inline constexpr float m2_display_scale = 10.0f;
  inline constexpr float wmo_display_scale = 15.0f;

  inline glm::mat4x4 transform(glm::vec3 const& position, glm::vec3 const& rotation, float display_scale)
  {
    glm::mat4x4 matrix{1.0f};
    matrix = glm::translate(matrix, position);
    matrix *= glm::eulerAngleYZX(
        glm::radians(rotation.y - 90.0f),
        glm::radians(-rotation.x),
        glm::radians(rotation.z));
    return glm::scale(matrix, glm::vec3(display_scale));
  }

  inline bool valid_bounds(glm::vec3 const& min, glm::vec3 const& max)
  {
    return min.x <= max.x && min.y <= max.y && min.z <= max.z;
  }
}
