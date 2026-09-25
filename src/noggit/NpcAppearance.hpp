#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Noggit
{
  struct NpcModelAppearance
  {
    std::string model_path;
    std::vector<bool> geosets;
    std::map<std::size_t, std::string> replacement_textures;
  };

  struct NpcAttachmentAppearance
  {
    unsigned attachment_id = 0;
    // Ranged equipment keeps attachment 12 as its slot identity while a
    // drawn model uses a hand socket.
    unsigned render_attachment_id = 0;
    NpcModelAppearance model;
  };

  struct NpcAppearance
  {
    NpcModelAppearance body;
    std::vector<NpcAttachmentAppearance> attachments;
  };
}
