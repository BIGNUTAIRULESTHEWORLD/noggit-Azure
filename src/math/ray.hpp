// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <glm/mat4x4.hpp>
#include <optional>
#include <cmath>
#include <iostream>

namespace math
{
  struct ray
  {
    ray (glm::vec3 origin, glm::vec3 const& direction)
      : _origin (std::move (origin)), _direction (glm::normalize(direction))
    {
      if (std::isnan(_direction.x) || std::isnan(_direction.y) || std::isnan(_direction.z)) 
      {
        assert(false);
      }
      // pre compute ivnerted direction
      _inverted_direction.x = (_direction.x != 0.0f) ? 1.0f / _direction.x : std::numeric_limits<float>::max();
      _inverted_direction.y = (_direction.y != 0.0f) ? 1.0f / _direction.y : std::numeric_limits<float>::max();
      _inverted_direction.z = (_direction.z != 0.0f) ? 1.0f / _direction.z : std::numeric_limits<float>::max();
    }

    ray (glm::mat4x4 const& transform, ray const& other)
      : ray (
        glm::vec3(transform * glm::vec4(other._origin, 1.0f)),
        glm::vec3((glm::mat3(transform) * other._direction))
            )
    {}

    std::optional<float> intersect_bounds
      (glm::vec3 const& _min, glm::vec3 const& _max) const noexcept;
    std::optional<float> intersect_triangle
      (glm::vec3 const& _v0, glm::vec3 const& _v1, glm::vec3 const& _v2) const noexcept;

    glm::vec3 position (float distance) const
    {
      return _origin + _direction * distance;
    }

    glm::vec3 const origin() const
    {
      return _origin;
    }

  private:
     glm::vec3 const _origin;
     glm::vec3 const _direction;
     glm::vec3 _inverted_direction;
  };
}
