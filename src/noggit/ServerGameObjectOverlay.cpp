#include <noggit/ServerGameObjectOverlay.hpp>

#include <noggit/ModelInstance.h>
#include <noggit/WMOInstance.h>

#include <blizzard-archive-library/include/Listfile.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace Noggit
{
  ServerGameObjectOverlay::ServerGameObjectOverlay(
    std::uint64_t guid, unsigned entry, std::string const& model_path,
    glm::vec3 const& position, float yaw, float scale, NoggitRenderContext context)
    : _guid(guid)
    , _entry(entry)
    , _position(position)
  {
    std::string extension = model_path.substr(model_path.find_last_of('.') == std::string::npos
      ? model_path.size() : model_path.find_last_of('.'));
    std::transform(extension.begin(), extension.end(), extension.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    BlizzardArchive::Listfile::FileKey const file_key(model_path);
    float const safe_scale = std::isfinite(scale) && scale > 0.0f
      ? std::clamp(scale, 0.01f, SceneObject::max_scale()) : 1.0f;
    if (extension == ".wmo")
    {
      _wmo = std::make_unique<WMOInstance>(file_key, context);
      _wmo->pos = position;
      _wmo->dir = {0.0f, yaw, 0.0f};
      _wmo->scale = safe_scale;
      _wmo->chunk_mover_preview = true;
      _wmo->updateTransformMatrix();
    }
    else if (extension == ".m2")
    {
      _model = std::make_unique<ModelInstance>(file_key, context);
      _model->pos = position;
      _model->dir = {0.0f, yaw, 0.0f};
      _model->scale = safe_scale;
      _model->chunk_mover_preview = true;
      _model->updateTransformMatrix();
    }
    else
      throw std::runtime_error("unsupported server gameobject model format");
  }

  ServerGameObjectOverlay::~ServerGameObjectOverlay() = default;
}
