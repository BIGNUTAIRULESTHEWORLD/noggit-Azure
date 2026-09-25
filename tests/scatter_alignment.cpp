#include "../src/noggit/tools/ScatterAlignment.hpp"
#include <iostream>
#include <stdexcept>

int main()
{
  try
  {
    for (auto normal : {glm::vec3(0, 1, 0), glm::vec3(1, 2, 0), glm::vec3(0, 2, -1),
                        glm::vec3(-2, 1, 3), glm::vec3(1, 0.001f, 0)})
      for (float yaw : {0.0f, 45.0f, 90.0f, 180.0f, 270.0f, 359.0f})
      {
        normal = glm::normalize(normal);
        auto const dir = Noggit::ScatterAlignment::rotation(normal, yaw);
        // Reconstruct the actual SceneObject transform, including axis signs.
        auto const actual = glm::eulerAngleYZX(glm::radians(dir.y - 90.0f),
          glm::radians(-dir.x), glm::radians(dir.z));
        auto up = glm::vec3(actual * glm::vec4(0, 1, 0, 0));
        if (glm::length(up - normal) > 0.0001f) throw std::runtime_error("Model up does not match terrain normal");
        auto expected = glm::toMat4(glm::rotation(glm::vec3(0, 1, 0), normal))
          * glm::eulerAngleYZX(glm::radians(yaw - 90.0f), 0.0f, 0.0f);
        for (int column = 0; column < 3; ++column)
          if (glm::length(actual[column] - expected[column]) > 0.0001f)
            throw std::runtime_error("Aligned rotation lost heading or changed handedness");
      }
    auto flat = Noggit::ScatterAlignment::rotation({0, 1, 0}, 123);
    auto zero = Noggit::ScatterAlignment::rotation({0, 0, 0}, 123);
    if (flat != glm::vec3(0, 123, 0) || zero != flat) throw std::runtime_error("Upright fallback changed heading");
    std::cout << "Scatter alignment: flat, compound slopes, steep slopes, headings and fallback passed.\n";
    return 0;
  }
  catch (std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
}
