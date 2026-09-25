#pragma once

#include <noggit/ContextObject.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>

class ModelInstance;
class WMOInstance;

namespace Noggit
{
  // Viewport-only representation of an acore_world.gameobject spawn. It is
  // deliberately not inserted into map tiles or the ADT object storage.
  class ServerGameObjectOverlay
  {
  public:
    ServerGameObjectOverlay(std::uint64_t guid, unsigned entry,
                            std::string const& model_path, glm::vec3 const& position,
                            float yaw, float scale, NoggitRenderContext context);
    ~ServerGameObjectOverlay();

    std::uint64_t guid() const { return _guid; }
    unsigned entry() const { return _entry; }
    glm::vec3 const& position() const { return _position; }
    ModelInstance* model() const { return _model.get(); }
    WMOInstance* wmo() const { return _wmo.get(); }

  private:
    std::uint64_t _guid;
    unsigned _entry;
    glm::vec3 _position;
    std::unique_ptr<ModelInstance> _model;
    std::unique_ptr<WMOInstance> _wmo;
  };
}
