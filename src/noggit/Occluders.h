#include <vector>
#include <glm/vec3.hpp>
#include <glm/ext/vector_float3.hpp>

namespace Noggit
{

  struct OccluderLocation
  {
    glm::vec3 position;
    bool initialized = false;
  };

  struct OccluderNode
  {
    int Id;
    // int OccluderID;
    int Sequence;
    int LocationID;

    OccluderLocation location;
  };

  struct Occluder
  {
    unsigned int id = 0;
    // unsigned int MapId;
    // type
    // splineType
    int Red = 0;
    int Green = 0;
    int Blue = 0;
    int Alpha = 255;
    // flags

    std::vector<OccluderNode> nodes;
  };

  class OccluderStorage
  {
  public:
    bool loaded = false;
    std::vector<Occluder> occludersWotlk;

    void loadFromCSV(int map_id);
  private:
    void loadOccludersFromCSV(int map_id);
    void loadOccluderNodesFromCSV(int map_id);
    void loadOccluderLocationsFromCSV(int map_id);

  };

}