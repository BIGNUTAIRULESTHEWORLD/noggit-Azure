// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/trig.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Brush.h> // brush
#include <noggit/ChunkWater.hpp>
#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/object_paste_params.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/TextureManager.h>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/WMOInstance.h> // WMOInstance
#include <noggit/World.h>
#include <noggit/World.inl>

#include <math/bounding_box.hpp>
#include <math/ray.hpp>

#include <blizzard-database-library/include/structures/FileStructures.h>

#include <external/tracy/Tracy.hpp>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
  struct LiquidVertexKey
  {
    std::int64_t x;
    std::int64_t z;

    bool operator==(LiquidVertexKey const& other) const
    {
      return x == other.x && z == other.z;
    }
  };

  struct LiquidVertexKeyHash
  {
    std::size_t operator()(LiquidVertexKey const& key) const
    {
      auto const x_hash = std::hash<std::int64_t>{}(key.x);
      auto const z_hash = std::hash<std::int64_t>{}(key.z);
      return x_hash ^ (z_hash + 0x9e3779b9 + (x_hash << 6) + (x_hash >> 2));
    }
  };

  struct LiquidVertexGroup
  {
    float x = 0.f;
    float z = 0.f;
    float height_sum = 0.f;
    std::size_t height_count = 0;
    std::vector<glm::vec3*> positions;
    std::unordered_set<liquid_layer*> layers;
    std::unordered_set<MapChunk*> chunks;

    float originalHeight() const
    {
      return height_sum / static_cast<float>(height_count);
    }
  };

  using LiquidVertexGroups = std::unordered_map<LiquidVertexKey, LiquidVertexGroup,
                                                 LiquidVertexKeyHash>;

  LiquidVertexKey liquidVertexKey(float x, float z)
  {
    // Liquid grid positions are shared in world space. Quantizing below a
    // millimetre makes separately stored chunk-edge copies one logical vertex.
    return {std::llround(static_cast<double>(x) * 10000.0),
            std::llround(static_cast<double>(z) * 10000.0)};
  }

  liquid_layer* selectedLiquidLayer(MapChunk* chunk, int target_layer,
                                    std::uint64_t surface_token)
  {
    auto* layers = chunk->liquid_chunk()->getLayers();
    if (surface_token)
    {
      auto const found = std::find_if(layers->begin(), layers->end(),
        [surface_token](liquid_layer const& layer)
        {
          return layer.surfaceToken() == surface_token;
        });
      return found == layers->end() ? nullptr : &*found;
    }

    return target_layer >= 0 && target_layer < static_cast<int>(layers->size())
      ? &(*layers)[target_layer] : nullptr;
  }

  LiquidVertexGroups collectLiquidVertices(std::vector<MapChunk*> const& chunks,
                                           int target_layer, std::uint64_t surface_token)
  {
    LiquidVertexGroups groups;
    for (MapChunk* chunk : chunks)
    {
      liquid_layer* layer = selectedLiquidLayer(chunk, target_layer, surface_token);
      if (!layer)
        continue;

      auto& vertices = layer->getVertices();
      for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x)
        {
          if (!layer->usesVertex(x, z))
            continue;

          glm::vec3& position = vertices[z * 9 + x].position;
          auto& group = groups[liquidVertexKey(position.x, position.z)];
          group.x = position.x;
          group.z = position.z;
          group.height_sum += position.y;
          ++group.height_count;
          group.positions.push_back(&position);
          group.layers.emplace(layer);
          group.chunks.emplace(chunk);
        }
    }
    return groups;
  }

  float liquidBrushWeight(float distance, float radius, float inner_radius, int falloff)
  {
    if (distance >= radius)
      return 0.f;
    if (falloff == eFlattenType_Flat)
      return 1.f;

    float const inner = std::clamp(inner_radius, 0.f, 1.f) * radius;
    if (distance <= inner)
      return 1.f;

    float const span = std::max(radius - inner, 0.0001f);
    float const linear = std::clamp(1.f - (distance - inner) / span, 0.f, 1.f);
    return falloff == eFlattenType_Smooth
      ? linear * linear * (3.f - 2.f * linear)
      : linear;
  }

  template<typename HeightFunction>
  void editLiquidVertexGroups(LiquidVertexGroups& groups, glm::vec3 const& cursor,
                              float radius, float inner_radius, int falloff,
                              HeightFunction&& height_function)
  {
    struct Update
    {
      LiquidVertexGroup* group;
      float height;
    };

    std::vector<Update> updates;
    std::unordered_set<MapChunk*> changed_chunks;
    std::unordered_set<liquid_layer*> changed_layers;

    for (auto& [key, group] : groups)
    {
      float const dx = group.x - cursor.x;
      float const dz = group.z - cursor.z;
      float const distance = std::sqrt(dx * dx + dz * dz);
      float const weight = liquidBrushWeight(distance, radius, inner_radius, falloff);
      if (weight <= 0.f)
        continue;

      std::optional<float> const target = height_function(group, weight);
      if (!target || misc::float_equals(*target, group.originalHeight()))
        continue;

      updates.push_back({&group, *target});
      changed_chunks.insert(group.chunks.begin(), group.chunks.end());
      changed_layers.insert(group.layers.begin(), group.layers.end());
    }

    // Undo must snapshot every physical owner before any duplicate is changed.
    for (MapChunk* chunk : changed_chunks)
      NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);

    for (Update const& update : updates)
      for (glm::vec3* position : update.group->positions)
        position->y = update.height;

    for (liquid_layer* layer : changed_layers)
      layer->refresh();
    for (MapChunk* chunk : changed_chunks)
      chunk->liquid_chunk()->update_layers();
  }
}


bool World::IsEditableWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record)
{
  ZoneScoped;
  std::string lMapName = record.Columns["Directory"].Value;

  std::stringstream ssfilename;
  ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(ssfilename.str()))
  {
    LogDebug << "World " << record.RecordId << ": " << lMapName << " has no WDT file!" << std::endl;
    return false;
  }

  BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

  //sometimes, wdts don't open, so ignore them...
  if (mf.isEof())
    return false;

  const char * lPointer = reinterpret_cast<const char*>(mf.getPointer());

  // Not using the libWDT here doubles performance. You might want to look at your lib again and improve it.
  const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));

  // check for global wmo flag
  if (lFlags & FLAG_GLOBAL_OBJECT)
    return true; // filter them later

  // check if map has tiles
  const int * lData = reinterpret_cast<const int*>(lPointer + 8 + 4 + 8 + 0x20 + 8);
  for (int i = 0; i < 8192; i += 2)
  {
    if (lData[i] & 1)
      return true;
  }

  // change : still load world even if it has no tile to allow user to edit it
  return true;
}

bool World::IsWMOWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record)
{
    ZoneScoped;
    std::string lMapName = record.Columns["Directory"].Value;

    std::stringstream ssfilename;
    ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

    BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

    const char* lPointer = reinterpret_cast<const char*>(mf.getPointer());

    const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));
    if (lFlags & 1)
        return true;

    return false;
}

World::World(const std::string& name, int map_id, Noggit::NoggitRenderContext context, bool create_empty)
    : _renderer(Noggit::Rendering::WorldRender(this))
    , _model_instance_storage(this)
    , _tile_update_queue(this)
    , mapIndex(name, map_id, this, context, create_empty)
    , horizon(name, this)
    , mWmoFilename(mapIndex.globalWMOName)
    , mWmoEntry(mapIndex.wmoEntry)
    , animtime(0)
    , time(1450)
    , basename(name)
    , _current_selection()
    , _settings(new QSettings())
    , _context(context)
    , occluders()
{
  LogDebug << "Loading world \"" << name << "\"." << std::endl;
  _loaded_tiles_buffer[0] = std::make_pair<std::pair<int, int>, MapTile*>(std::make_pair(0, 0), nullptr);

  // initialize wdl models here
  if (horizon.wmos.size() < horizon.lWMOInstances.size())
  {
    for (int i = 0; i < horizon.mWMOFilenames.size(); ++i)
    {
      // auto instance = horizon.lWMOInstances[i];
      auto& filepath = horizon.mWMOFilenames[i];
      horizon.wmos.push_back(scoped_wmo_reference(filepath, _context));
    }
  }

  occluders.loadFromCSV(map_id);
}

void World::LoadSavedSelectionGroups()
{
  _selection_groups.clear();

  auto& saved_map_groups = Noggit::Project::CurrentProject::get()->ObjectSelectionGroups;
  for (auto& map_group : saved_map_groups)
  {
      if (map_group.MapId == mapIndex._map_id)
      {
          for (auto& group : map_group.SelectionGroups)
          {
              selection_group selectionGroup(group, this);
              _selection_groups.push_back(selectionGroup);
          }
          return;
      }
  }
}

void World::saveSelectionGroups()
{
    auto proj_selection_map_group = Noggit::Project::NoggitProjectSelectionGroups();
    proj_selection_map_group.MapId = mapIndex._map_id;
    for (auto& selection_group : _selection_groups)
    {
        proj_selection_map_group.SelectionGroups.push_back(selection_group.getMembers());
    }

    Noggit::Project::CurrentProject::get()->saveObjectSelectionGroups(proj_selection_map_group);
}

Noggit::Rendering::WorldRender* World::renderer()
{
  return &_renderer;
}

void World::notifyTextureChange(int global_chunk_x, int global_chunk_z)
{
  constexpr int chunks_per_map_axis = 64 * 16;
  if (global_chunk_x < 0 || global_chunk_x >= chunks_per_map_axis
      || global_chunk_z < 0 || global_chunk_z >= chunks_per_map_axis)
  {
    return;
  }

  std::uint32_t const chunk_key = static_cast<std::uint32_t>(
    global_chunk_z * chunks_per_map_axis + global_chunk_x);
  std::lock_guard<std::mutex> const lock(_texture_change_mutex);
  _texture_changed_chunks.insert(chunk_key);
}

std::vector<std::uint32_t> World::takeTextureChanges()
{
  std::lock_guard<std::mutex> const lock(_texture_change_mutex);
  std::vector<std::uint32_t> changes;
  changes.reserve(_texture_changed_chunks.size());
  changes.assign(_texture_changed_chunks.begin(), _texture_changed_chunks.end());
  _texture_changed_chunks.clear();
  return changes;
}

void World::update_selection_pivot()
{
  ZoneScoped;
  if (has_multiple_model_selected())
  {
    glm::vec3 pivot = glm::vec3(0);
    int model_count = 0;

    for (auto const& entry : _current_selection)
    {
      if (entry.index() == eEntry_Object)
      {
        pivot += std::get<selected_object_type>(entry)->pos;
        model_count++;
      }
    }

    _multi_select_pivot = pivot / static_cast<float>(model_count);
  }
  else
  {
    _multi_select_pivot = std::nullopt;
  }
}

std::optional<glm::vec3> const& World::multi_select_pivot() const
{
  return _multi_select_pivot;
}

bool World::is_selected(selection_type selection)
{
  ZoneScoped;
  if (selection.index() != eEntry_Object)
    return false;

  /*
  auto which = std::get<selected_object_type>(selection)->which();

  if (which == eMODEL)
  {
    uint uid = static_cast<ModelInstance*>(std::get<selected_object_type>(selection))->uid;
    auto const& it = std::find_if(_current_selection.begin()
                                  , _current_selection.end()
                                  , [uid] (selection_type type)
    {
      return var_type(type) == typeid(selected_object_type)
        && std::get<selected_object_type>(type)->which() == eMODEL
        && static_cast<ModelInstance*>(std::get<selected_object_type>(type))->uid == uid;
    }
    );

    if (it != _current_selection.end())
    {
      return true;
    }
  }
  else if (which == eWMO)
  {
    uint uid = static_cast<WMOInstance*>(std::get<selected_object_type>(selection))->uid;
    auto const& it = std::find_if(_current_selection.begin()
                            , _current_selection.end()
                            , [uid] (selection_type type)
    {
      return var_type(type) == typeid(selected_object_type)
        && std::get<selected_object_type>(type)->which() == eWMO
        && static_cast<WMOInstance*>(std::get<selected_object_type>(type))->uid == uid;
    }
    );
    if (it != _current_selection.end())
    {
      return true;
    }
  }


  return false;
*/

  auto selected_object = std::get<selected_object_type>(selection);
  unsigned int uid = selected_object->uid;

  bool found = selected_uids.contains(uid);
  if (!found)
    return false;

  // verify object type
  // probably should only be done when adding or removing objects.
  /*
  auto instance = getObjectInstance(uid);
  if (instance == nullptr || var_type(instance) != typeid(selected_object_type))
    return false;

  if (selected_object->which() != instance->which())
  {
    return false;
  }
  */
  return true;
}

bool World::is_selected(std::uint32_t uid) const
{
  return selected_uids.contains(uid);
}

std::vector<selection_type> const& World::current_selection() const
{
  return _current_selection;
}

std::optional<selection_type> World::get_last_selected_model() const
{
  ZoneScoped;
  if (_current_selection.empty())
      return std::nullopt;

  auto const it
    ( std::find_if ( _current_selection.rbegin()
                   , _current_selection.rend()
                   , [&] (selection_type const& entry)
                     {
                       return entry.index() != eEntry_MapChunk;
                     }
                   )
    );

  return it == _current_selection.rend()
    ? std::optional<selection_type>() : std::optional<selection_type> (*it);
}

bool World::has_selection() const
{
  return !_current_selection.empty();
}

bool World::has_multiple_model_selected() const
{
  return _selected_model_count > 1;
}

int World::get_selected_model_count() const
{
  return _selected_model_count;
}

std::vector<selected_object_type> const World::get_selected_objects() const
{
    // std::vector<selected_object_type> objects(_selected_model_count);
    std::vector<selected_object_type> objects;
    objects.reserve(_selected_model_count);

    ZoneScoped;
    for (auto& entry : _current_selection)
    {
        if (entry.index() == eEntry_Object)
        {
            auto obj = std::get<selected_object_type>(entry);
            objects.push_back(obj);
        }
    }

    return objects;
}

glm::vec3 getBarycentricCoordinatesAt(
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c,
    const glm::vec3& point,
    const glm::vec3& normal)
{
  glm::vec3 bary;
  // The area of a triangle is

  glm::vec3 aMb = (b - a);
  glm::vec3 cMa = (c - a);
  glm::vec3 bMpoint = (b - point);
  glm::vec3 cMpont = (c - point);
  glm::vec3 aMpoint = (a - point);

  glm::vec3 ABC = glm::cross(aMb ,cMa);
  glm::vec3 PBC = glm::cross(bMpoint, cMpont);
  glm::vec3 PCA = glm::cross(cMpont, aMpoint);

  double areaABC = glm::dot(normal , ABC);
  double areaPBC = glm::dot(normal , PBC);
  double areaPCA = glm::dot(normal , PCA);

  bary.x = areaPBC / areaABC; // alpha
  bary.y = areaPCA / areaABC; // beta
  bary.z = 1.0f - bary.x - bary.y; // gamma

  return bary;
}

void World::rotate_selected_models_randomly(float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
{
  ZoneScoped;
  bool has_multi_select = has_multiple_model_selected();

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    math::degrees::vec3& dir = obj->dir;

    float rx = misc::randfloat(minX, maxX);
    float ry = misc::randfloat(minY, maxY);
    float rz = misc::randfloat(minZ, maxZ);

    //Building rotations
    auto heading = math::radians(math::degrees(dir.z))._ * 0.5;
    auto attitude = math::radians(math::degrees(-dir.y))._ * 0.5;
    auto bank = math::radians(math::degrees(dir.x))._ * 0.5;
    // Assuming the angles are in radians.
    double c1 = cos(heading);
    double s1 = sin(heading);
    double c2 = cos(attitude);
    double s2 = sin(attitude);
    double c3 = cos(bank);
    double s3 = sin(bank);
    double c1c2 = c1 * c2;
    double s1s2 = s1 * s2;
    auto w = static_cast<float>(c1c2 * c3 - s1s2 * s3);
    auto x = static_cast<float>(c1c2 * s3 + s1s2 * c3);
    auto y = static_cast<float>(s1 * c2 * c3 + c1 * s2 * s3);
    auto z = static_cast<float>(c1 * s2 * c3 - s1 * c2 * s3);

    glm::quat baseRotation = glm::quat(x,y,z,w);

    //Building rotations
    heading = math::radians(math::degrees(rx))._ * 0.5;
    attitude = math::radians(math::degrees(ry))._ * 0.5;
    bank = math::radians(math::degrees(rx))._ * 0.5;
    // Assuming the angles are in radians.
    c1 = cos(heading);
    s1 = sin(heading);
    c2 = cos(attitude);
    s2 = sin(attitude);
    c3 = cos(bank);
    s3 = sin(bank);
    c1c2 = c1 * c2;
    s1s2 = s1 * s2;
    w = static_cast<float>(c1c2 * c3 - s1s2 * s3);
    x = static_cast<float>(c1c2 * s3 + s1s2 * c3);
    y = static_cast<float>(s1 * c2 * c3 + c1 * s2 * s3);
    z = static_cast<float>(c1 * s2 * c3 - s1 * c2 * s3);

    glm::quat newRotation = glm::quat(x, y, z, w);
    glm::quat finalRotation = baseRotation * newRotation;
    glm::quat finalRotationNormalized = glm::normalize(finalRotation);

    auto eulerAngles = glm::eulerAngles(finalRotationNormalized);
    dir.x = math::degrees(math::radians(eulerAngles.z))._;
    dir.y = math::degrees(math::radians(eulerAngles.x))._;
    dir.z = math::degrees(math::radians(eulerAngles.y))._;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
}

void World::rotate_model_to_ground_normal(SceneObject* obj, bool smoothNormals)
{
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    updateTilesEntry(obj, model_update::remove);

    glm::vec3 rayPos = obj->pos;
    math::degrees::vec3& dir = obj->dir;


    selection_result results;
    for_chunk_at(rayPos, [&](MapChunk* chunk)
        {
            {
                math::ray intersect_ray(rayPos, glm::vec3(0.f, -1.f, 0.f));
                chunk->intersect(intersect_ray, &results, true);
            }
            // object is below ground
            if (results.empty())
            {
                math::ray intersect_ray(rayPos, glm::vec3(0.f, 1.f, 0.f));
                chunk->intersect(intersect_ray, &results, true);
            }
        });

    // !\ todo We shouldn't end up with empty ever (but we do, on completely flat ground)
    if (results.empty())
    {
        // just to avoid models disappearing when this happens
        updateTilesEntry(obj, model_update::add);
        return;
    }


    // We hit the terrain, now we take the normal of this position and use it to get the rotation we want.
    auto const& hitChunkInfo = std::get<selected_chunk_type>(results.front().second);

    glm::quat q;
    glm::vec3 varnormal;

    // Surface Normal
    auto& p0 = hitChunkInfo.chunk->mVertices[std::get<0>(hitChunkInfo.triangle)];
    auto& p1 = hitChunkInfo.chunk->mVertices[std::get<1>(hitChunkInfo.triangle)];
    auto& p2 = hitChunkInfo.chunk->mVertices[std::get<2>(hitChunkInfo.triangle)];

    glm::vec3 v1 = p1 - p0;
    glm::vec3 v2 = p2 - p0;

    auto tmpVec = glm::cross(v2, v1);
    varnormal.x = tmpVec.z;
    varnormal.y = tmpVec.y;
    varnormal.z = tmpVec.x;

    // Smooth option, gradient the normal towards closest vertex
    if (smoothNormals) // Vertex Normal
    {
        auto normalWeights = getBarycentricCoordinatesAt(p0, p1, p2, hitChunkInfo.position, varnormal);

        auto& tile_buffer = hitChunkInfo.chunk->mt->getChunkHeightmapBuffer();
        int chunk_start = (hitChunkInfo.chunk->px * 16 + hitChunkInfo.chunk->py) * mapbufsize * 4;

        const auto& vNormal0 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<0>(hitChunkInfo.triangle) * 4]);
        const auto& vNormal1 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<1>(hitChunkInfo.triangle) * 4]);
        const auto& vNormal2 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<2>(hitChunkInfo.triangle) * 4]);

        varnormal.x =
            vNormal0.x * normalWeights.x +
            vNormal1.x * normalWeights.y +
            vNormal2.x * normalWeights.z;

        varnormal.y =
            vNormal0.y * normalWeights.x +
            vNormal1.y * normalWeights.y +
            vNormal2.y * normalWeights.z;

        varnormal.z =
            vNormal0.z * normalWeights.x +
            vNormal1.z * normalWeights.y +
            vNormal2.z * normalWeights.z;
    }


    glm::vec3 worldUp = glm::vec3(0, 1, 0);
    glm::vec3 a = glm::cross(worldUp, varnormal);

    q.x = a.x;
    q.y = a.y;
    q.z = a.z;

    auto worldLengthSqrd = glm::length(worldUp) * glm::length(worldUp);
    auto normalLengthSqrd = glm::length(varnormal) * glm::length(varnormal);
    auto worldDotNormal = glm::dot(worldUp, varnormal);

    q.w = std::sqrt((worldLengthSqrd * normalLengthSqrd) + (worldDotNormal));

    auto normalizedQ = glm::normalize(q);

    //math::degrees::vec3 new_dir;
    // To euler, because wow
      /*
      // roll (x-axis rotation)
      double sinr_cosp = 2.0 * (q.w * q.x + q.y * q.z);
      double cosr_cosp = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
      new_dir.z = std::atan2(sinr_cosp, cosr_cosp) * 180.0f / math::constants::pi;

      // pitch (y-axis rotation)
      double sinp = 2.0 * (q.w * q.y - q.z * q.x);
      if (std::abs(sinp) >= 1)
        new_dir.y = std::copysign(math::constants::pi / 2, sinp) * 180.0f / math::constants::pi; // use 90 degrees if out of range
      else
        new_dir.y = std::asin(sinp) * 180.0f / math::constants::pi;

      // yaw (z-axis rotation)
      double siny_cosp = 2.0 * (q.w * q.z + q.x * q.y);
      double cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
      new_dir.x = std::atan2(siny_cosp, cosy_cosp) * 180.0f / math::constants::pi;
     }*/

    auto eulerAngles = glm::eulerAngles(normalizedQ);
    dir.x = math::degrees(math::radians(eulerAngles.z))._; //Roll
    dir.y = math::degrees(math::radians(eulerAngles.x))._; //Pitch
    dir.z = math::degrees(math::radians(eulerAngles.y))._; //Yaw

    obj->recalcExtents();

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    updateTilesEntry(obj, model_update::add);
}

void World::rotate_selected_models_to_ground_normal(bool smoothNormals)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);

    rotate_model_to_ground_normal(obj, smoothNormals);
  }
  update_selected_model_groups();
}

void World::set_current_selection(selection_type entry)
{
  ZoneScoped;
  reset_selection();
  add_to_selection(entry);
}

// updating pivot is expensive, in mass selection situation, it should only be updated once after operation is done
// now checks if model is already selected, don't need to call is_selected anymore !
bool World::add_to_selection(selection_type entry, bool skip_group, bool update_pivot)
{
  ZoneScoped;
  if (entry.index() == eEntry_Object)
  {

    auto obj = std::get<selected_object_type>(entry);

    auto result = selected_uids.insert(obj->uid);

    if (!result.second)
    {
      // Duplicate existed
      return false;
    }

    _selected_model_count++;
    // check if it is in a group
    if (!skip_group)
    {
        for (auto& group : _selection_groups)
        {
            if (group.contains_object(obj))
            {
                // make sure to add it to selection before donig group selection so it doesn't get selected twice
                _current_selection.push_back(entry);
                // this then calls add_to_selection() with skip_group = true to avoid repetition
                group.select_group();
                return true;
            }
        }
    }
  }
  _current_selection.push_back(entry);

  if (update_pivot)
    update_selection_pivot();

  return true;
}

void World::remove_from_selection(selection_type entry, bool skip_group, bool update_pivot)
{
  ZoneScoped;
  if (entry.index() == eEntry_Object)
  {
    auto obj = std::get<selected_object_type>(entry);
    size_t erased_count = selected_uids.erase(obj->uid);
    if (erased_count == 0)
      return;
  }

  std::vector<selection_type>::iterator position = std::find(_current_selection.begin(), _current_selection.end(), entry);
  if (position != _current_selection.end())
  {
    if (entry.index() == eEntry_Object)
    {
      _selected_model_count--;
      // check if it is in a group
      if (!skip_group)
      {
        for (auto& group : _selection_groups)
        {
          auto obj = std::get<selected_object_type>(entry);
          if (group.contains_object(obj))
          {
              // this then calls remove_from_selection() with skip_group = true to avoid repetition
              group.unselect_group();
              break;
          }
        }
      }
    }

    _current_selection.erase(position);
    if (update_pivot)
      update_selection_pivot();
  }
}

void World::remove_from_selection(std::uint32_t uid, bool skip_group, bool update_pivot)
{
  ZoneScoped;
  size_t erased_count = selected_uids.erase(uid);
  if (erased_count == 0)
    return;

  for (auto it = _current_selection.begin(); it != _current_selection.end(); ++it)
  {
    if (it->index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(*it);

    if (obj->uid == uid)
    {
      _selected_model_count--;
      _current_selection.erase(it);

      // check if it is in a group
      if (!skip_group)
      {
        for (auto& group : _selection_groups)
        {
          if (group.contains_object(obj))
          {
            // this then calls remove_from_selection() with skip_group = true to avoid repetition
            group.unselect_group();
            break;
          }
        }
      }
      if (update_pivot)
        update_selection_pivot();
      return;
    }

  }
}

void World::reset_selection()
{
  ZoneScoped;
  selected_uids.clear();
  _current_selection.clear();
  _multi_select_pivot = std::nullopt;
  _selected_model_count = 0;

  for (auto& selection_group : _selection_groups)
  {
      selection_group.setUnselected();
  }
}

void World::delete_selected_models()
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  // erase selected groups as well
  for (auto& group : _selection_groups)
  {
      if (group.isSelected())
      {
          group.remove_group();
      }
  }

  _model_instance_storage.delete_instances(get_selected_objects(), true);
  need_model_updates = true;
  reset_selection();
}

glm::vec3 World::get_ground_height(glm::vec3 pos)
{
  selection_result hits;
    
  for_chunk_at(pos, [&](MapChunk* chunk)
  {
      {
        // ray origin should be independent of the object's current y 
        glm::vec3 ray_pos(pos.x, chunk->getMaxHeight() + 1.0f, pos.z);
        math::ray intersect_ray(ray_pos, glm::vec3(0.f, -1.f, 0.f));
        chunk->intersect(intersect_ray, &hits, true);
      }
  });

  if (hits.empty())
  {
      LogError << "Snap to ground ray intersection failed" << std::endl;
      // return objects position instead of world space y = 0
      return pos;
  }

  return std::get<selected_chunk_type>(hits[0].second).position;
}

std::optional<glm::vec3> World::try_get_ground_height(glm::vec3 const& pos)
{
  MapTile* tile = mapIndex.getTile(pos);
  if (!tile || !tile->finishedLoading())
    return std::nullopt;

  MapChunk* chunk = tile->getChunk(
      (pos.x - tile->xbase) / CHUNKSIZE,
      (pos.z - tile->zbase) / CHUNKSIZE);
  if (!chunk)
    return std::nullopt;

  selection_result hits;
  glm::vec3 const ray_pos(pos.x, chunk->getMaxHeight() + 1.0f, pos.z);
  math::ray const intersect_ray(ray_pos, glm::vec3(0.f, -1.f, 0.f));
  chunk->intersect(intersect_ray, &hits, true);
  if (hits.empty())
    return std::nullopt;

  return std::get<selected_chunk_type>(hits[0].second).position;
}

void World::snap_selected_models_to_the_ground()
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    // the ground can only be intersected once
    pos.y = get_ground_height(pos).y;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selection_pivot();
  update_selected_model_groups();
}

void World::scale_selected_models(float v, object_scaling_type type, bool scale_positions_around_pivot)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  v = std::clamp(v, SceneObject::min_scale(), SceneObject::max_scale());

  bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;
  bool const scale_around_pivot = scale_positions_around_pivot
    && type == object_scaling_type::mult
    && has_multiple_model_selected()
    && _multi_select_pivot.has_value();

  if (scale_around_pivot)
  {
    // A group transform needs one effective multiplier for both object sizes and
    // their offsets from the pivot. Clamp it once for the whole selection so an
    // individual object reaching a scale limit cannot distort the arrangement.
    float min_factor = SceneObject::min_scale();
    float max_factor = SceneObject::max_scale();
    bool has_scalable_object = false;

    for (auto const& entry : _current_selection)
    {
      if (entry.index() != eEntry_Object)
        continue;

      auto* obj = std::get<selected_object_type>(entry);
      if (obj->which() == eWMO && !modern_features)
        continue;

      has_scalable_object = true;
      min_factor = std::max(min_factor, SceneObject::min_scale() / obj->scale);
      max_factor = std::min(max_factor, SceneObject::max_scale() / obj->scale);
    }

    if (!has_scalable_object)
      return;

    v = std::clamp(v, min_factor, max_factor);
  }

  bool positions_changed = false;

  for (auto& entry : _current_selection)
  {
    if (entry.index() != eEntry_Object)
      continue;

    auto* obj = std::get<selected_object_type>(entry);
    bool const can_scale = obj->which() == eMODEL || modern_features;
    float new_scale = obj->scale;

    if (can_scale)
    {
      switch (type)
      {
      case World::object_scaling_type::set:
        new_scale = v;
        break;
      case World::object_scaling_type::add:
        new_scale += v;
        break;
      case World::object_scaling_type::mult:
        new_scale *= v;
        break;
      }
      new_scale = std::clamp(new_scale, SceneObject::min_scale(), SceneObject::max_scale());
    }

    bool const position_changed = scale_around_pivot && v != 1.f;
    bool const scale_changed = can_scale && (scale_around_pivot
      ? new_scale != obj->scale
      : std::abs(new_scale - obj->scale) >= ModelInstance::min_scale());

    if (!position_changed && !scale_changed)
      continue;

    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    updateTilesEntry(entry, model_update::remove);

    if (position_changed)
    {
      glm::vec3 const& pivot = _multi_select_pivot.value();
      obj->pos = pivot + (obj->pos - pivot) * v;
      positions_changed = true;
    }
    if (scale_changed)
      obj->scale = new_scale;

    obj->recalcExtents();
    updateTilesEntry(entry, model_update::add);
  }

  if (positions_changed)
    update_selection_pivot();
  update_selected_model_groups();
}

void World::move_selected_models(float dx, float dy, float dz)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    updateTilesEntry(entry, model_update::remove);

    pos.x += dx;
    pos.y += dy;
    pos.z += dz;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selection_pivot();
  update_selected_model_groups();
}

void World::move_model(selection_type entry, float dx, float dy, float dz)
{
    ZoneScoped;
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
        return;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    updateTilesEntry(entry, model_update::remove);

    pos.x += dx;
    pos.y += dy;
    pos.z += dz;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);

}

void World::move_selected_models(glm::vec3 const& delta)
{
  move_selected_models(delta.x, delta.y, delta.z);
}

void World::set_selected_models_pos(float x, float y, float z, bool change_height)
{
  return set_selected_models_pos({ x,y,z }, change_height);
}

void World::set_selected_models_pos(glm::vec3 const& pos, bool change_height)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  // move models relative to the pivot when several are selected
  if (has_multiple_model_selected())
  {
    glm::vec3 diff = pos - _multi_select_pivot.value();

    if (change_height)
    {
      move_selected_models(diff);
    }
    else
    {
      move_selected_models(diff.x, 0.f, diff.z);
    }

    return;
  }

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    obj->pos = pos;
    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selection_pivot();
  update_selected_model_groups();
}

void World::set_model_pos(selection_type entry, glm::vec3 const& pos, bool change_height)
{
  ZoneScoped;
  auto type = entry.index();
  if (type == eEntry_MapChunk)
  {
      return;
  }
  
  updateTilesEntry(entry, model_update::remove);
  
  auto& obj = std::get<selected_object_type>(entry);
  NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
  obj->pos = pos;
  obj->recalcExtents();
  
  updateTilesEntry(entry, model_update::add);
}

void World::rotate_selected_models(math::degrees rx, math::degrees ry, math::degrees rz, bool use_pivot)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  math::degrees::vec3 dir_change(rx._, ry._, rz._);
  bool has_multi_select = has_multiple_model_selected();

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    if (use_pivot && has_multi_select)
    {
      glm::vec3& pos = obj->pos;
      math::degrees::vec3& dir = obj->dir;
      glm::vec3 diff_pos = pos - _multi_select_pivot.value();

      glm::quat rotationQuat = glm::quat(glm::vec3(glm::radians(rx._), glm::radians(ry._), glm::radians(rz._)));
      glm::vec3 rot_result = glm::toMat4(rotationQuat) * glm::vec4(diff_pos,0);

      pos += rot_result - diff_pos;
    }
    else
    {
      // math::degrees::vec3& dir = obj->dir;
      // dir += dir_change;
    }
    math::degrees::vec3& dir = obj->dir;
    dir += dir_change;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::set_selected_models_rotation(math::degrees rx, math::degrees ry, math::degrees rz)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  math::degrees::vec3 new_dir(rx._, ry._, rz._);

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type != eEntry_Object)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    updateTilesEntry(entry, model_update::remove);

    math::degrees::vec3& dir = obj->dir;

    dir = new_dir;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::update_selected_model_groups()
{
  for (auto& selection_group : _selection_groups)
  {
      if (selection_group.isSelected())
          selection_group.recalcExtents();
  }
}

MapChunk* World::getChunkAt(glm::vec3 const& pos)
{
  MapTile* tile(mapIndex.getTile(pos));
  if (tile && tile->finishedLoading())
  {
    return tile->getChunk((pos.x - tile->xbase) / CHUNKSIZE, (pos.z - tile->zbase) / CHUNKSIZE);
  }
  return nullptr;
}

bool World::isInIndoorWmoGroup(std::array<glm::vec3, 2> obj_bounds, glm::mat4x4 obj_transform)
{
    bool is_indoor = false;
    // check if model bounds is within wmo bounds then check each indor wmo group bounds
    _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
        {
            auto wmo_extents = wmo_instance.getExtents();
            // check if global wmo bounds intersect
            if (obj_bounds[1].x >= wmo_extents[0].x
                && obj_bounds[1].y >= wmo_extents[0].y
                && obj_bounds[1].z >= wmo_extents[0].z
                && wmo_extents[1].x >= obj_bounds[0].x
                && wmo_extents[1].y >= obj_bounds[0].y
                && wmo_extents[1].z >= obj_bounds[0].z)

            {
                for (int i = 0; i < (int)wmo_instance.wmo->groups.size(); ++i)
                {
                    auto const& group = wmo_instance.wmo->groups[i];

                    if (group.is_indoor())
                    {
                        // must call getGroupExtent() to initialize wmo_instance.group_extents
                        // clear group extents to free memory ?
                        auto& group_extents = wmo_instance.getGroupExtents().at(i);


                        bool aabb_test = obj_bounds[1].x >= group_extents.first.x
                            && obj_bounds[1].y >= group_extents.first.y
                            && obj_bounds[1].z >= group_extents.first.z
                            && group_extents.second.x >= obj_bounds[0].x
                            && group_extents.second.y >= obj_bounds[0].y
                            && group_extents.second.z >= obj_bounds[0].z;

                        // TODO : do a precise calculation instead of using axis aligned bounding boxes.
                        if (aabb_test) // oriented box check
                        {
                            /* TODO
                            if (collide_test)
                            {
                                is_indoor = true;
                                return;
                            }
                            */
                        }
                    }
                }
            }
        });

    return is_indoor;
}

selection_result World::intersect (glm::mat4x4 const& model_view
                                  , math::ray const& ray
                                  , const bool pOnlyMap
                                  , const bool do_objects
                                  , const bool draw_terrain
                                  , const bool draw_wmo
                                  , const bool draw_models
                                  , const bool draw_hidden_models
                                  , const bool draw_wmo_exterior
                                  , const bool animate
                                  , const bool first_object_occurence
                                  , const bool opaque_only_tris
                                  , const float obj_distance_max
                                  , const bool do_wmo_interiors
                                  )
{
  ZoneScopedN("World::intersect()");
  selection_result results;

  if (draw_terrain)
  {
    ZoneScopedN("World::intersect() : intersect terrain");

    for (auto& pair : _loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      TileIndex index{ static_cast<std::size_t>(pair.first.first)
                        , static_cast<std::size_t>(pair.first.second) };

      // handle tiles that got unloaded mid-frame to avoid illegal access
      if (!mapIndex.tileLoaded(index) || mapIndex.tileAwaitingLoading(index))
          continue;

      if (!tile->finishedLoading())
        continue;

      if (tile->intersect(ray, &results))
        break;
    }
  }

  if (!pOnlyMap && do_objects)
  {
    if (!draw_models && !draw_wmo)
      return std::move(results);

    ////////////// Optimized version, can iterate the same objects multiple times if they are on borders though

    // store in a set container to avoid duplicates, this is pretty slow, doing a few extra rays is much faster if duplicates aren't a problem
    // std::unordered_set<ModelInstance*> modelInstances;
    // std::unordered_set<WMOInstance*> wmoInstances;

    for (auto& pair : _loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      TileIndex index{ static_cast<std::size_t>(pair.first.first)
                , static_cast<std::size_t>(pair.first.second) };

      // add some distance check ?
      // if (tile-> > )
      //   continue;

      if (!mapIndex.tileLoaded(index) || mapIndex.tileAwaitingLoading(index))
        continue;

      if (!tile->finishedLoading())
        continue;

      tile->recalcCombinedExtents();

      if (!ray.intersect_bounds(tile->getCombinedExtents()[0], tile->getCombinedExtents()[1]))
      {
        continue;
      }

      for (auto& pair : tile->getObjectInstances())
      {
        if (pair.second[0]->which() == eMODEL && draw_models)
        {
          
          for (auto& instance : pair.second)
          {
            auto model_instance = static_cast<ModelInstance*>(instance);

            if (obj_distance_max != 0.0f)
            {
              const float distance = glm::distance(ray.origin(), instance->pos);
              if ((distance - instance->getBoundingRadius()) > obj_distance_max)
                continue;
            }

            if (draw_hidden_models || !model_instance->model->is_hidden())
              // modelInstances.insert(model_instance);
              model_instance->intersect(model_view, ray, &results, animtime, animate, first_object_occurence, opaque_only_tris);
          }
        }
        else if (pair.second[0]->which() == eWMO && draw_wmo)
        {
          for (auto& instance : pair.second)
          {
            auto wmo_instance = static_cast<WMOInstance*>(instance);

            if (obj_distance_max != 0.0f)
            {
              const float distance = glm::distance(ray.origin(), instance->pos);
              if ((distance - instance->getBoundingRadius()) > obj_distance_max)
                continue;
            }

            if (draw_hidden_models || !wmo_instance->wmo->is_hidden())
              // wmoInstances.insert(wmo_instance);
              wmo_instance->intersect(ray, &results, draw_wmo_exterior, do_wmo_interiors, first_object_occurence);
          }
        }
      }
    }
  }

  return std::move(results);
}

void World::update_models_emitters(float dt)
{
  ZoneScoped;
  while (dt > 0.1f)
  {
    ModelManager::updateEmitters(0.1f);
    dt -= 0.1f;
  }
  ModelManager::updateEmitters(dt);
}

unsigned int World::getAreaID (glm::vec3 const& pos)
{
  ZoneScoped;
  return for_maybe_chunk_at (pos, [&] (MapChunk* chunk) { return chunk->getAreaID(); }).value_or(-1);
}

void World::clearHeight(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
    chunk->clearHeight();
  });
  for_all_chunks_on_tile(pos, [this] (MapChunk* chunk) {
      recalc_norms (chunk);
  });
}

void World::clearAllModelsOnADT(TileIndex const& tile, bool action)
{
  ZoneScoped;
  _model_instance_storage.delete_instances_from_tile(tile, action);
  // update_models_by_filename();
}

void World::CropWaterADT(const TileIndex& pos)
{
  ZoneScoped;
  for_tile_at(pos, [](MapTile* tile)
  {
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j)
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

    tile->CropWater();
  });
}

void World::setAreaID(glm::vec3 const& pos, int id, bool adt, float radius)
{
  ZoneScoped;
  if (adt)
  {
    for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
    {
      NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
      chunk->setAreaID(id);
    });
  }
  else
  {

    if (radius >= 0)
    {
      for_all_chunks_in_range(pos, radius,
                              [&] (MapChunk* chunk)
                              {
                                NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
                                chunk->setAreaID(id);
                                return true;
                              }
      );

    }
    else
    {
      for_chunk_at(pos, [&](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
        chunk->setAreaID(id);
      });
    }
  }
}

Noggit::NoggitRenderContext World::getRenderContext() const
{
  return _context;
}

bool World::GetVertex(float x, float z, glm::vec3 *V) const
{
  ZoneScoped;
  TileIndex tile({x, 0, z});

  if (!mapIndex.tileLoaded(tile))
  {
    return false;
  }

  MapTile* adt = mapIndex.getTile(tile);

  return adt->GetVertex(x, z, V);
}



void World::changeShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        return chunk->ChangeMCCV(pos, color, change, radius, editMode);
      }
    );
}

void World::stampShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode, QImage* img, bool paint, bool use_image_colors)
{
  ZoneScoped;
  for_all_chunks_in_rect
    ( pos, radius
      , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        return chunk->stampMCCV(pos, color, change, radius, editMode, img, paint, use_image_colors);
      }
    );
}

glm::vec3 World::pickShaderColor(glm::vec3 const& pos)
{
  ZoneScoped;
  glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
  for_all_chunks_in_range
  (pos, 0.1f
    , [&] (MapChunk* chunk)
  {
    color = chunk->pickMCCV(pos);
    return true;
  }
  );

  return color;
}

auto World::stamp(glm::vec3 const& pos, float dt, QImage const* img, float radiusOuter
, float radiusInner, int brushType, bool sculpt, BrushShape shape) -> void
{
  ZoneScoped;
  auto action = NOGGIT_CUR_ACTION;
  float delta = action->getDelta() + dt;
  action->setDelta(delta);

  for_all_chunks_in_rect(pos, radiusOuter,
                          [=](MapChunk* chunk) -> bool
                          {
                            auto action = NOGGIT_CUR_ACTION;
                            action->registerChunkTerrainChange(chunk);
                            action->setBlockCursor(!sculpt);
                            chunk->stamp(pos, dt, img, radiusOuter, radiusInner, brushType, sculpt,
                                         shape); return true;
                          }
                          , [this](MapChunk* chunk) -> void
                          {
                            recalc_norms(chunk);

                            // check if coord axis > 0
                            // if true, get chunk by coord axis - 1
                            // else, check if tile coord axis > 0
                            // if true, get tile by tile coord axis - 1,  get last chunk by axis
                            auto get_neighbor =
                              [this, chunk](int px, int py) -> MapChunk*
                              {
                                MapChunk* neighbor{};

                                int new_chunk_x = px + chunk->px;
                                int new_chunk_z = py + chunk->py;

                                if (new_chunk_x < 0 || new_chunk_z < 0 || new_chunk_x == 16 || new_chunk_z == 16)
                                {
                                  TileIndex index(chunk->mt->index.x + px, chunk->mt->index.z + py);
                                  if (index.x != std::numeric_limits<std::size_t>::max()
                                  && index.z != std::numeric_limits<std::size_t>::max()
                                  && index.x != 64
                                  && index.z != 64)
                                  {
                                    MapTile* neighbor_tile = mapIndex.getTile(index);

                                    if (!neighbor_tile)
                                      return nullptr;

                                    neighbor = neighbor_tile->getChunk((new_chunk_x + 16) % 16,
                                                                       (new_chunk_z + 16) % 16);
                                  }
                                }
                                else
                                {
                                  neighbor = chunk->mt->getChunk(new_chunk_x, new_chunk_z);
                                }

                                return neighbor;
                              };

                            if (auto neighbor = get_neighbor(-1, 0); neighbor)
                              chunk->fixGapLeft(neighbor);

                            if (auto neighbor = get_neighbor(0, -1); neighbor)
                              chunk->fixGapAbove(neighbor);

                            if (auto neighbor = get_neighbor(1, 0); neighbor)
                              neighbor->fixGapLeft(chunk);

                            if (auto neighbor = get_neighbor(0, 1); neighbor)
                              neighbor->fixGapAbove(chunk);

                          });
}


void World::changeObjectsWithTerrain(glm::vec3 const& pos, float change, float radius, int BrushType,
                                     float inner_radius, bool iter_wmos_, bool iter_m2s,
                                     BrushShape shape)
{
    // applies the terrain brush to the terrain objects hit
    ZoneScoped;

    if (!iter_wmos_ && !iter_m2s)
        return;

  // Identical code to chunk->changeTerrain()
  //    if (_snap_m2_objects_chkbox->isChecked() || _snap_wmo_objects_chkbox->isChecked()) {
  float const candidate_radius = shape == BrushShape::SQUARE
    ? radius * std::sqrt(2.0f)
    : radius;
  auto objects_hit = getObjectsInRange(pos, candidate_radius, true, iter_wmos_, iter_m2s);

  for (auto obj : objects_hit)
  {

    float dt = change;

    float dist, xdiff, zdiff;
    bool changed = false;

    xdiff = obj->pos.x - pos.x;
    zdiff = obj->pos.z - pos.z;

    if (BrushType == eTerrainType_Quadra)
    {
        if ((std::abs(xdiff) < std::abs(radius / 2)) && (std::abs(zdiff) < std::abs(radius / 2)))
        {
            dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
            dt = dt * (1.0f - dist * inner_radius / radius);
            changed = true;
        }
    }
    else
    {
        dist = shape == BrushShape::SQUARE
          ? std::max(std::abs(xdiff), std::abs(zdiff))
          : std::sqrt(xdiff * xdiff + zdiff * zdiff);
        if (dist < radius)
        {
            changed = true;

            switch (BrushType)
            {
            case eTerrainType_Flat:
                break;
            case eTerrainType_Linear:
                dt = dt * (1.0f - dist * (1.0f - inner_radius) / radius);
                break;
            case eTerrainType_Smooth:
                dt = dt / (1.0f + dist / radius);
                break;
            case eTerrainType_Polynom:
                dt = dt * ((dist / radius) * (dist / radius) + dist / radius + 1.0f);
                break;
            case eTerrainType_Trigo:
                dt = dt * cos(dist / radius);
                break;
            case eTerrainType_Gaussian:
                dt = dist < radius * inner_radius ? dt * std::exp(-(std::pow(radius * inner_radius / radius, 2) / (2 * std::pow(0.39f, 2)))) : dt * std::exp(-(std::pow(dist / radius, 2) / (2 * std::pow(0.39f, 2))));

                break;
            default:
                LogError << "Invalid terrain edit type (" << inner_radius << ")" << std::endl;
                changed = false;
                break;
            }
        }
    }
    if (changed)
    {
        move_model(obj, 0.0f, dt, 0.0f);
        // set_model_pos(obj, glm::vec3(obj->pos.x, obj->pos.y + dt, obj->pos.z));
    }
  }
}

void World::changeTerrain(glm::vec3 const& pos, float change, float radius, int BrushType,
                          float inner_radius, BrushShape shape)
{
  ZoneScoped;

  auto edit = [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
    return chunk->changeTerrain(pos, change, radius, BrushType, inner_radius, shape);
  };
  auto post = [this](MapChunk* chunk) { recalc_norms(chunk); };

  if (shape == BrushShape::SQUARE)
    for_all_chunks_in_rect(pos, radius, edit, post);
  else
    for_all_chunks_in_range(pos, radius, edit, post);
}

std::vector<selected_object_type> World::getObjectsInRange(glm::vec3 const& pos, float radius, bool ignore_height, bool iter_wmos_, bool iter_m2s)
{
    // ignores height by default

    std::vector<selected_object_type> objects_hit_list;

    /* This causes duplicates at tile edges
    for (MapTile* tile : mapIndex.tiles_in_range(pos, radius))
    {
        if (!tile->finishedLoading())
        {
            continue;
        }

        std::vector<uint32_t>* uids = tile->get_uids();

        for (uint32_t uid : *uids)
        {
            auto instance = _model_instance_storage.get_instance(uid);

            */
    if (iter_m2s)
    {
        _model_instance_storage.for_each_m2_instance([&](ModelInstance& model_instance)
            {
                selected_object_type obj = &model_instance;
                auto obj_pos = obj->pos;
                if (ignore_height)
                {
                    obj_pos = glm::vec3(obj->pos.x, pos.y, obj->pos.z);
                }
                if (glm::distance(obj_pos, pos) <= radius) // this is just origin point
                {
                    objects_hit_list.push_back(obj);
                }
            });
    }

    if (iter_wmos_)
    {
        _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
            {
                selected_object_type obj = &wmo_instance;
                auto obj_pos = obj->pos;
                if (ignore_height)
                {
                    obj_pos = glm::vec3(obj->pos.x, pos.y, obj->pos.z);
                }
                if (glm::distance(obj_pos, pos) <= radius)
                {
                    objects_hit_list.push_back(obj);
                }
            });
    }

    return objects_hit_list;
}

void World::flattenTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode, const glm::vec3& origin, math::degrees angle, math::degrees orientation)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->flattenTerrain(pos, remain, radius, BrushType, mode, origin, angle, orientation);
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

std::vector<std::pair<SceneObject*, float>> World::getObjectsGroundDistance(glm::vec3 const& pos, float radius, bool iter_wmos_, bool iter_m2s)
{
    std::vector<std::pair<SceneObject*, float>> objects_ground_distance;

    if (!iter_wmos_ && !iter_m2s)
        return objects_ground_distance;

    auto objects_hit = getObjectsInRange(pos, radius, true
        , iter_wmos_, iter_m2s);

    for (auto obj : objects_hit)
    {
        if ((obj->which() == eMODEL && !iter_m2s) || (obj->which() == eWMO && !iter_wmos_))
            continue;
        float height_diff = obj->pos.y - get_ground_height(obj->pos).y;
        objects_ground_distance.push_back(std::pair<SceneObject*, float>(obj, height_diff));
    }

    return objects_ground_distance;
}

void World::blurTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode)
{
  ZoneScoped;

  if (BrushType == eFlattenType_Origin || radius <= 0.f)
  {
    return;
  }

  // Preserve the original wide-area cone blur, but evaluate it from an
  // immutable snapshot. The old per-chunk pass sampled vertices that earlier
  // chunks had already changed, so duplicate border vertices could receive
  // different results and split the seam.
  using grid_key = std::pair<int, int>;
  struct height_sample
  {
    double total = 0.0;
    int count = 0;
  };
  struct blur_sample
  {
    float x;
    float z;
    float height;
  };

  float const half_unit = UNITSIZE * 0.5f;
  auto const key_for = [half_unit](glm::vec3 const& vertex) -> grid_key
  {
    return {static_cast<int>(std::lround(vertex.x / half_unit)),
            static_cast<int>(std::lround(vertex.z / half_unit))};
  };

  std::map<grid_key, height_sample> snapshot;
  std::vector<MapChunk*> target_chunks;

  for (MapTile* tile : mapIndex.tiles_in_range(pos, radius))
  {
    if (!tile || !tile->finishedLoading())
    {
      continue;
    }

    for (MapChunk* chunk : tile->chunks_in_range(pos, radius))
    {
      target_chunks.push_back(chunk);

      for (glm::vec3 const& vertex : chunk->mVertices)
      {
        auto& sample = snapshot[key_for(vertex)];
        sample.total += vertex.y;
        ++sample.count;
      }
    }
  }

  auto const snapshot_height = [&snapshot](grid_key const& key) -> std::optional<float>
  {
    auto const it = snapshot.find(key);
    if (it == snapshot.end() || !it->second.count)
    {
      return std::nullopt;
    }
    return static_cast<float>(it->second.total / it->second.count);
  };

  // These are the same staggered sample positions and GetVertex lookups used
  // by the original implementation. Capture them all before mutating terrain.
  std::vector<blur_sample> blur_samples;
  int const sample_radius = static_cast<int>(radius / UNITSIZE);
  blur_samples.reserve((sample_radius * 4 + 1) * (sample_radius * 2 + 1));
  for (int row = -sample_radius * 2; row <= sample_radius * 2; ++row)
  {
    float const sample_z = pos.z + row * half_unit;
    for (int column = -sample_radius; column <= sample_radius; ++column)
    {
      float const sample_x = pos.x + column * UNITSIZE + (row % 2) * half_unit;
      glm::vec3 vertex;
      if (GetVertex(sample_x, sample_z, &vertex))
      {
        blur_samples.push_back({sample_x, sample_z, vertex.y});
      }
    }
  }

  std::map<grid_key, float> blurred_heights;
  for (MapChunk* chunk : target_chunks)
  {
    for (glm::vec3 const& vertex : chunk->mVertices)
    {
      grid_key const key = key_for(vertex);
      if (blurred_heights.find(key) != blurred_heights.end())
      {
        continue;
      }

      float const vertex_x = key.first * half_unit;
      float const vertex_z = key.second * half_unit;
      auto const center = snapshot_height(key);
      if (!center)
      {
        continue;
      }

      // Terrain brushes operate on a horizontal heightmap footprint. Using the
      // cursor's Y coordinate here makes steep vertices fall outside the brush
      // even when their XZ position is visibly inside its circle.
      float const xdiff = vertex_x - pos.x;
      float const zdiff = vertex_z - pos.z;
      float const dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
      if (dist >= radius)
      {
        continue;
      }

      double total_height = 0.0;
      double total_weight = 0.0;
      for (blur_sample const& sample : blur_samples)
      {
        float const xdiff = sample.x - vertex_x;
        float const zdiff = sample.z - vertex_z;
        float const sample_dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
        if (sample_dist > radius)
        {
          continue;
        }

        float const weight = 1.f - sample_dist / radius;
        total_height += weight * sample.height;
        total_weight += weight;
      }

      if (total_weight <= 0.0)
      {
        continue;
      }

      float const target = static_cast<float>(total_height / total_weight);
      if ((target > *center && !mode.raise) || (target < *center && !mode.lower))
      {
        blurred_heights.emplace(key, *center);
        continue;
      }

      float const amount = BrushType == eFlattenType_Flat ? remain
        : BrushType == eFlattenType_Linear ? remain * (1.f - dist / radius)
        : BrushType == eFlattenType_Smooth ? std::pow(remain, 1.f + dist / radius)
        : throw std::logic_error("bad brush type");
      blurred_heights.emplace(key, glm::mix(*center, target, amount));
    }
  }

  std::vector<MapChunk*> modified_chunks;
  for (MapChunk* chunk : target_chunks)
  {
    bool changed = false;
    for (glm::vec3 const& vertex : chunk->mVertices)
    {
      auto const height = blurred_heights.find(key_for(vertex));
      if (height != blurred_heights.end() && vertex.y != height->second)
      {
        changed = true;
        break;
      }
    }

    if (!changed)
    {
      continue;
    }

    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
    for (glm::vec3& vertex : chunk->mVertices)
    {
      auto const height = blurred_heights.find(key_for(vertex));
      if (height != blurred_heights.end())
      {
        vertex.y = height->second;
      }
    }
    chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
    mapIndex.setChanged(chunk->mt);
    modified_chunks.push_back(chunk);
  }

  for (MapChunk* chunk : modified_chunks)
  {
    recalc_norms(chunk);
  }
}

void World::recalc_norms (MapChunk* chunk) const
{
    ZoneScoped;
    chunk->recalcNorms();
}

bool World::paintTexture(glm::vec3 const& pos, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture)
{
  ZoneScoped;
  auto paint = [&] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    return chunk->paintTexture(pos, brush, strength, pressure, texture);
  };

  if (brush->getShape() == BrushShape::SQUARE)
    return for_all_chunks_in_rect(pos, brush->getRadius(), paint);

  return for_all_chunks_in_range
    ( pos, brush->getRadius()
    , [&] (MapChunk* chunk)
      {
        return paint(chunk);
      }
    );
}

namespace
{
  bool road_segment_may_touch_chunk(MapChunk const* chunk, glm::vec3 const& from,
                                    glm::vec3 const& to, float half_width)
  {
    float const min_x = std::min(from.x, to.x) - half_width;
    float const max_x = std::max(from.x, to.x) + half_width;
    float const min_z = std::min(from.z, to.z) - half_width;
    float const max_z = std::max(from.z, to.z) + half_width;
    return !(max_x < chunk->xbase || min_x > chunk->xbase + CHUNKSIZE
             || max_z < chunk->zbase || min_z > chunk->zbase + CHUNKSIZE);
  }
}

bool World::canPaintRoadSegment(glm::vec3 const& from, glm::vec3 const& to,
                                sampled_road_style const& style, float width_scale,
                                bool replace_conflicting_textures)
{
  bool can_paint = true;
  glm::vec3 const midpoint = (from + to) * 0.5f;
  float const half_width = std::max(TEXDETAILSIZE, style.half_width * width_scale);
  float const search_radius = glm::distance(glm::vec2(from.x, from.z), glm::vec2(to.x, to.z)) * 0.5f
    + half_width;

  for_all_chunks_in_rect(midpoint, search_radius, [&](MapChunk* chunk)
  {
    if (road_segment_may_touch_chunk(chunk, from, to, half_width)
        && !chunk->getTextureSet()->canApplyRoadStyle(style, replace_conflicting_textures))
    {
      can_paint = false;
    }
    return false;
  });
  return can_paint;
}

bool World::canPaintRoadPath(std::vector<glm::vec3> const& points,
                             sampled_road_style const& style, float width_scale,
                             bool replace_conflicting_textures)
{
  if (points.size() < 2)
  {
    return false;
  }
  for (std::size_t index = 0; index + 1 < points.size(); ++index)
  {
    if (!canPaintRoadSegment(points[index], points[index + 1], style, width_scale,
        replace_conflicting_textures))
    {
      return false;
    }
  }
  return true;
}

road_paint_result World::paintRoadSegment(glm::vec3 const& from, glm::vec3 const& to,
                                          sampled_road_style const& style,
                                          float width_scale, float opacity_scale,
                                          bool replace_conflicting_textures)
{
  road_paint_result combined;
  if (!canPaintRoadSegment(from, to, style, width_scale, replace_conflicting_textures))
  {
    combined.blocked_by_texture_limit = true;
    return combined;
  }

  glm::vec3 const midpoint = (from + to) * 0.5f;
  float const half_width = std::max(TEXDETAILSIZE, style.half_width * width_scale);
  float const search_radius = glm::distance(glm::vec2(from.x, from.z), glm::vec2(to.x, to.z)) * 0.5f
    + half_width;
  for_all_chunks_in_rect(midpoint, search_radius, [&](MapChunk* chunk)
  {
    if (!road_segment_may_touch_chunk(chunk, from, to, half_width))
    {
      return false;
    }
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    road_paint_result const result = chunk->getTextureSet()->paintRoadSegment(
      from, to, style, width_scale, opacity_scale, replace_conflicting_textures);
    combined.changed |= result.changed;
    combined.blocked_by_texture_limit |= result.blocked_by_texture_limit;
    combined.replaced_texture_layers += result.replaced_texture_layers;
    return result.changed;
  });
  return combined;
}

road_paint_result World::paintRoadPath(std::vector<glm::vec3> const& points,
                                       sampled_road_style const& style,
                                       float width_scale, float opacity_scale,
                                       bool replace_conflicting_textures)
{
  road_paint_result combined;
  if (points.size() < 2 || !canPaintRoadPath(points, style, width_scale,
      replace_conflicting_textures))
  {
    combined.blocked_by_texture_limit = true;
    return combined;
  }

  float min_x = points.front().x;
  float max_x = points.front().x;
  float min_z = points.front().z;
  float max_z = points.front().z;
  for (glm::vec3 const& point : points)
  {
    min_x = std::min(min_x, point.x);
    max_x = std::max(max_x, point.x);
    min_z = std::min(min_z, point.z);
    max_z = std::max(max_z, point.z);
  }
  float const half_width = std::max(TEXDETAILSIZE, style.half_width * width_scale);
  glm::vec3 const midpoint{(min_x + max_x) * 0.5f, points.front().y, (min_z + max_z) * 0.5f};
  float const search_radius = glm::length(glm::vec2{max_x - min_x, max_z - min_z}) * 0.5f
    + half_width;
  min_x -= half_width;
  max_x += half_width;
  min_z -= half_width;
  max_z += half_width;

  for_all_chunks_in_rect(midpoint, search_radius, [&](MapChunk* chunk)
  {
    if (max_x < chunk->xbase || min_x > chunk->xbase + CHUNKSIZE
        || max_z < chunk->zbase || min_z > chunk->zbase + CHUNKSIZE)
    {
      return false;
    }
    bool path_may_touch_chunk = false;
    for (std::size_t index = 0; index + 1 < points.size(); ++index)
    {
      if (road_segment_may_touch_chunk(chunk, points[index], points[index + 1], half_width))
      {
        path_may_touch_chunk = true;
        break;
      }
    }
    if (!path_may_touch_chunk)
    {
      return false;
    }
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    road_paint_result const result = chunk->getTextureSet()->paintRoadPath(
      points, style, width_scale, opacity_scale, replace_conflicting_textures);
    combined.changed |= result.changed;
    combined.blocked_by_texture_limit |= result.blocked_by_texture_limit;
    combined.replaced_texture_layers += result.replaced_texture_layers;
    return result.changed;
  });
  return combined;
}

bool World::stampTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture, QImage* img, bool paint)
{
  ZoneScoped;
  return for_all_chunks_in_rect
    ( pos, brush->getRadius()
      , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        return chunk->stampTexture(pos, brush, strength, pressure, texture, img, paint);
      }
    );
}

bool World::sprayTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, float spraySize, float sprayPressure, scoped_blp_texture_reference texture)
{
  ZoneScoped;
  bool succ = false;
  float inc = brush->getRadius() / 4.0f;

  for (float pz = pos.z - spraySize; pz < pos.z + spraySize; pz += inc)
  {
    for (float px = pos.x - spraySize; px < pos.x + spraySize; px += inc)
    {
      bool const inside_spray = brush->getShape() == BrushShape::SQUARE
        || std::sqrt(std::pow(px - pos.x, 2) + std::pow(pz - pos.z, 2)) <= spraySize;
      if (inside_spray && ((rand() % 1000) < sprayPressure))
      {
        succ |= paintTexture({px, pos.y, pz}, brush, strength, pressure, texture);
      }
    }
  }

  return succ;
}

bool World::replaceTexture(glm::vec3 const& pos, float radius, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture, bool entire_chunk, bool entire_tile)
{
  ZoneScoped;

  if (entire_tile)
  {
      bool changed(false);

      for (MapTile* tile : mapIndex.tiles_in_range(pos, radius))
      {
          if (!tile->finishedLoading())
          {
              continue;
          }

          for (int i = 0; i < 16; ++i)
          {
              for (int j = 0; j < 16; ++j)
              {
                  MapChunk* chunk = tile->getChunk(i, j);
                  NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                  if (chunk->replaceTexture(pos, radius, old_texture, new_texture, true))
                  {
                      changed = true;
                      mapIndex.setChanged(tile);
                  }
              }
          }
      }
      return changed;
  }
  else
  {
    return for_all_chunks_in_range
      ( pos, radius
        , [&](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
          return chunk->replaceTexture(pos, radius, old_texture, new_texture, entire_chunk);
        }
      );
  }
}

void World::eraseTextures(glm::vec3 const& pos)
{
  ZoneScoped;
  for_chunk_at(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
  });
}

void World::overwriteTextureAtCurrentChunk(glm::vec3 const& pos, scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture)
{
  ZoneScoped;
  for_chunk_at(pos, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->switchTexture(oldTexture, std::move (newTexture));
  });
}

void World::paintGroundEffectExclusion(glm::vec3 const& pos, float radius, bool exclusion)
{
    ZoneScoped;
    for_all_chunks_in_range
    (pos, radius
        , [&](MapChunk* chunk)
        {
            // TODO action
            NOGGIT_CUR_ACTION->registerChunkDetailDoodadExclusionChange(chunk);

            // chunk->setHole(pos, radius, exclusion);
            chunk->paintDetailDoodadsExclusion(pos, radius, exclusion);
            return true;
        }
    );
}

namespace
{
    // Assigns effect_id to every layer of the chunk matching the texture
    // (empty texture matches every layer); returns whether anything changed.
    bool apply_ground_effect_to_chunk(MapChunk* chunk, std::string const& texture, unsigned int effect_id,
                                      bool override_existing, bool register_action,
                                      std::optional<unsigned int> only_effect_id = std::nullopt)
    {
        auto texture_set = chunk->getTextureSet();
        bool changed = false;
        for (int layer = 0; layer < texture_set->num(); ++layer)
        {
            if (!texture.empty() && texture_set->filename(layer) != texture)
            {
                continue;
            }
            unsigned int const current = texture_set->getEffectForLayer(layer);
            if (only_effect_id.has_value() && current != only_effect_id.value())
            {
                continue;
            }
            if (current == effect_id)
            {
                continue;
            }
            if (!override_existing && current && current != 0xFFFFFFFF)
            {
                continue;
            }
            if (!changed && register_action)
            {
                NOGGIT_CUR_ACTION->registerChunkLayerInfoChange(chunk);
            }
            changed = true;
            texture_set->setEffect(layer, static_cast<int>(effect_id));
        }
        return changed;
    }
}

void World::paintGroundEffect(glm::vec3 const& pos, float radius, std::string const& texture, unsigned int effect_id)
{
    ZoneScoped;
    for_all_chunks_in_range
    (pos, radius
        , [&](MapChunk* chunk)
        {
            return apply_ground_effect_to_chunk(chunk, texture, effect_id, true, true);
        }
    );
}

void World::applyGroundEffectToTileAt(glm::vec3 const& pos, std::string const& texture, unsigned int effect_id,
                                      bool override_existing, std::optional<unsigned int> only_effect_id)
{
    ZoneScoped;
    MapTile* tile(mapIndex.getTile(pos));
    if (!tile || !tile->finishedLoading())
    {
        return;
    }

    bool tile_changed = false;
    for (int cx = 0; cx < 16; ++cx)
    {
        for (int cz = 0; cz < 16; ++cz)
        {
            if (apply_ground_effect_to_chunk(tile->getChunk(cx, cz), texture, effect_id,
                                             override_existing, true, only_effect_id))
            {
                tile_changed = true;
            }
        }
    }

    if (tile_changed)
    {
        mapIndex.setChanged(tile);
    }
}

void World::applyGroundEffectToArea(int area_id, bool whole_zone, std::string const& texture, unsigned int effect_id,
                                    bool override_existing, std::optional<unsigned int> only_effect_id)
{
    ZoneScoped;
    for (MapTile* tile : mapIndex.loaded_tiles())
    {
        if (!tile->finishedLoading())
        {
            continue;
        }

        bool tile_changed = false;
        for (int cx = 0; cx < 16; ++cx)
        {
            for (int cz = 0; cz < 16; ++cz)
            {
                MapChunk* chunk = tile->getChunk(cx, cz);

                int chunk_area = chunk->getAreaID();
                if (whole_zone)
                {
                    chunk_area = AreaDB::resolve_zone_id(chunk_area);
                }
                if (chunk_area != area_id)
                {
                    continue;
                }

                if (apply_ground_effect_to_chunk(chunk, texture, effect_id,
                                                 override_existing, true, only_effect_id))
                {
                    tile_changed = true;
                }
            }
        }

        if (tile_changed)
        {
            mapIndex.setChanged(tile);
        }
    }
}

void World::applyGroundEffectGlobal(std::string const& texture, unsigned int effect_id, bool override_existing,
                                    int area_filter, bool whole_zone, QProgressDialog* progress,
                                    std::optional<unsigned int> only_effect_id)
{
    ZoneScoped;
    int processed = 0;
    if (progress)
    {
        progress->setRange(0, 64 * 64);
        progress->setValue(0);
    }
    for (size_t z = 0; z < 64; z++)
    {
        for (size_t x = 0; x < 64; x++)
        {
            if (progress && progress->wasCanceled())
            {
                return;
            }
            TileIndex tile_index(x, z);

            bool unload = !mapIndex.tileLoaded(tile_index) && !mapIndex.tileAwaitingLoading(tile_index);
            MapTile* tile = mapIndex.loadTile(tile_index);

            if (!tile)
            {
                if (progress) progress->setValue(++processed);
                continue;
            }

            tile->wait_until_loaded();

            bool tile_changed = false;
            for (int cx = 0; cx < 16; ++cx)
            {
                for (int cz = 0; cz < 16; ++cz)
                {
                    MapChunk* chunk = tile->getChunk(cx, cz);

                    if (area_filter >= 0)
                    {
                        int chunk_area = chunk->getAreaID();
                        if (whole_zone)
                        {
                            chunk_area = AreaDB::resolve_zone_id(chunk_area);
                        }
                        if (chunk_area != area_filter)
                        {
                            continue;
                        }
                    }

                    // no undo registration: swept tiles unload again below
                    if (apply_ground_effect_to_chunk(chunk, texture, effect_id,
                                                     override_existing, false, only_effect_id))
                    {
                        tile_changed = true;
                    }
                }
            }

            // only tiles that actually contained the texture get rewritten
            if (tile_changed)
            {
                tile->saveTile(this);
                mapIndex.markOnDisc(tile_index, true);
                mapIndex.unsetChanged(tile_index);
            }

            if (unload)
            {
                mapIndex.unloadTile(tile_index);
            }
            if (progress) progress->setValue(++processed);
        }
    }
    if (progress) progress->setValue(64 * 64);
}

void World::setHole(glm::vec3 const& pos, float radius, bool big, bool hole)
{
  ZoneScoped;
  for_all_chunks_in_range
      ( pos, radius
        , [&](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
          chunk->setHole(pos, radius, big, hole);
          return true;
        }
      );
}

void World::setHoleADT(glm::vec3 const& pos, bool hole)
{
  ZoneScoped;

  for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
    chunk->setHole(pos, 1.0f, true, hole);
  });
}

void World::loadAllTiles(glm::vec3& camera_pos)
{
  ZoneScoped;

  // for (size_t z = 0; z < 64; z++)
  // {
  //   for (size_t x = 0; x < 64; x++)
  //   {
  //     TileIndex tile(x, z);
  // 
  //     MapTile* mTile = mapIndex.loadTile(tile);
  // 
  //     if (mTile)
  //     {
  //       // mTile->wait_until_loaded();
  //     }
  //   }
  // }

  // test loading tiles from player to outer
  // Create a vector to hold distances and corresponding tiles
  std::vector<std::pair<float, TileIndex>> distanceTilePairs;
  // Fill the vector with Manhattan distances and tiles
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      if (!mapIndex.hasTile(tile))
        continue;

      // int distance = calculateManhattanDistance(playerRow, playerCol, tile);
      float playerRow = /*std::floor*/(camera_pos.x / TILESIZE);
      float playerCol = /*std::floor*/(camera_pos.z / TILESIZE);
      float distance = std::abs(playerRow - tile.x) + std::abs(playerCol - tile.z);
      // tile.dist()
      distanceTilePairs.emplace_back(distance, tile);

    }
  }
  // Sort the vector based on distance
  std::sort(distanceTilePairs.begin(), distanceTilePairs.end(),
    [](const auto& a, const auto& b) {
      return a.first < b.first; // Sort by distance
    });

  for (const auto& pair : distanceTilePairs)
  {
    MapTile* mTile = mapIndex.loadTile(pair.second);

    if (mTile)
    {
      // mTile->wait_until_loaded();
    }
  }
}

unsigned World::getNumLoadedTiles() const
{
  return _n_loaded_tiles;
}

unsigned World::getNumRenderedTiles() const
{
  return _n_rendered_tiles;
}

unsigned World::getNumRenderedObjects() const
{
  return _n_rendered_objects;
}

void World::convert_alphamap(QProgressDialog* progress_dialog, bool to_big_alpha)
{
  ZoneScoped;

  if (to_big_alpha == mapIndex.hasBigAlpha())
  {
    return;
  }


  int count = 0;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      // not cancellable.
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        mTile->convert_alphamap(to_big_alpha);
        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
        count++;
        progress_dialog->setValue(count);
      }
    }
  }
  mapIndex.convert_alphamap(to_big_alpha);
  mapIndex.save();
}


void World::deleteModelInstance(int uid, bool action)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_model_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid, action);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteWMOInstance(int uid, bool action)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_wmo_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid, action);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteInstance(int uid, bool action)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid, action);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteInstances(std::vector<std::uint32_t> const& uids, bool action)
{
  bool removed_any = false;
  for (std::uint32_t uid : uids)
  {
    auto instance = _model_instance_storage.get_instance(uid);
    if (!instance)
      continue;

    SceneObject* object = std::get<selected_object_type>(*instance);
    if (object->chunk_mover_preview)
      continue;

    std::vector<MapTile*> const referenced_tiles = object->getTiles();
    for (MapTile* tile : referenced_tiles)
    {
      if (!tile)
        continue;
      tile->remove_model(object);
      mapIndex.setChanged(tile);
    }

    _model_instance_storage.delete_instance_without_world_update(uid, action);
    removed_any = true;
  }

  if (removed_any)
  {
    need_model_updates = true;
    reset_selection();
  }
}

bool World::uid_duplicates_found() const
{
  ZoneScoped;
  return _model_instance_storage.uid_duplicates_found();
}

void World::delete_duplicate_model_and_wmo_instances()
{
  ZoneScoped;
  reset_selection();

  _model_instance_storage.clear_duplicates(false);
  need_model_updates = true;
}

void World::unload_every_model_and_wmo_instance()
{
  ZoneScoped;
  reset_selection();

  _model_instance_storage.clear();

  // _models_by_filename.clear();
}

void World::addM2 ( BlizzardArchive::Listfile::FileKey const& file_key
                  , glm::vec3 newPos
                  , float scale
                  , glm::vec3 rotation
                  , Noggit::object_paste_params* paste_params
                  , bool action
                  )
{
  ZoneScoped;
  ModelInstance model_instance = ModelInstance(file_key, _context);

  model_instance.uid = _model_instance_storage.new_unique_uid();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;

  if (paste_params)
  {
    if (_settings->value("model/random_rotation", false).toBool())
    {
      float min = paste_params->minRotation;
      float max = paste_params->maxRotation;
      model_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_tilt", false).toBool ())
    {
      float min = paste_params->minTilt;
      float max = paste_params->maxTilt;
      model_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
      model_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_size", false).toBool ())
    {
      float min = paste_params->minScale;
      float max = paste_params->maxScale;
      model_instance.scale = misc::randfloat(min, max);
    }
  }

  // to ensure the tiles are updated correctly
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_model_instance(std::move(model_instance), true, action);

  // _models_by_filename[file_key.filepath()].push_back(_model_instance_storage.get_model_instance(uid).value());
}

ModelInstance* World::addM2AndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
    , glm::vec3 newPos
    , float scale
    , math::degrees::vec3 rotation
    , Noggit::object_paste_params* paste_params
    , bool ignore_params
    , bool action
)
{
  ZoneScoped;
  ModelInstance model_instance = ModelInstance(file_key, _context);

  model_instance.uid = _model_instance_storage.new_unique_uid();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;

  if (paste_params && !ignore_params)
  {
    if (_settings->value("model/random_rotation", false).toBool())
    {
      float min = paste_params->minRotation;
      float max = paste_params->maxRotation;
      model_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_tilt", false).toBool ())
    {
      float min = paste_params->minTilt;
      float max = paste_params->maxTilt;
      model_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
      model_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_size", false).toBool ())
    {
      float min = paste_params->minScale;
      float max = paste_params->maxScale;
      model_instance.scale = misc::randfloat(min, max);
    }
  }

  // to ensure the tiles are updated correctly
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_model_instance(std::move(model_instance), true, action);

  auto instance = _model_instance_storage.get_model_instance(uid); // .value();
  // _models_by_filename[file_key.filepath()].push_back(instance);

  return instance.value();
}

ModelInstance* World::addChunkMoverPreviewM2(
    BlizzardArchive::Listfile::FileKey const& file_key, glm::vec3 newPos, float scale,
    math::degrees::vec3 rotation)
{
  ModelInstance model_instance(file_key, _context);
  model_instance.uid = _model_instance_storage.new_preview_uid();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;
  model_instance.chunk_mover_preview = true;
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();
  std::uint32_t const uid = _model_instance_storage.add_model_instance(
      std::move(model_instance), false, false);
  ModelInstance* instance = _model_instance_storage.get_model_instance(uid).value();
  auto const& extents = instance->getExtents();
  TileIndex const start(extents[0]);
  TileIndex const end(extents[1]);
  if (start.is_valid() && end.is_valid())
    for (std::size_t z = start.z; z <= end.z; ++z)
      for (std::size_t x = start.x; x <= end.x; ++x)
        if (MapTile* tile = mapIndex.getTile(TileIndex{x, z}); tile && tile->finishedLoading())
          tile->add_model(instance);
  return instance;
}

void World::addWMO ( BlizzardArchive::Listfile::FileKey const& file_key
                   , glm::vec3 newPos
                   , float scale
                   , math::degrees::vec3 rotation
                   , Noggit::object_paste_params* paste_params
                   , bool action
                   )
{
  ZoneScoped;
  WMOInstance wmo_instance(file_key, _context);

  wmo_instance.uid = _model_instance_storage.new_unique_uid();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;

  if (paste_params)
  {
      if (_settings->value("model/random_rotation", false).toBool())
      {
          float min = paste_params->minRotation;
          float max = paste_params->maxRotation;
          wmo_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
      }

      if (_settings->value("model/random_tilt", false).toBool())
      {
          float min = paste_params->minTilt;
          float max = paste_params->maxTilt;
          wmo_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
          wmo_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
      }

      if (_settings->value("model/random_size", false).toBool())
      {
          float min = paste_params->minScale;
          float max = paste_params->maxScale;
          wmo_instance.scale = misc::randfloat(min, max);
      }
  }


  // to ensure the tiles are updated correctly
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();

  _model_instance_storage.add_wmo_instance(std::move(wmo_instance), true, action);
}

WMOInstance* World::addWMOAndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
    , glm::vec3 newPos
    , math::degrees::vec3 rotation
    , float scale
    , bool action
)
{
  ZoneScoped;
  WMOInstance wmo_instance(file_key, _context);

  wmo_instance.uid = _model_instance_storage.new_unique_uid();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;
  wmo_instance.scale = scale;

  // to ensure the tiles are updated correctly
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_wmo_instance(std::move(wmo_instance), true, action);

  auto instance = _model_instance_storage.get_wmo_instance(uid);

  return instance.value();
}

WMOInstance* World::addChunkMoverPreviewWMO(
    BlizzardArchive::Listfile::FileKey const& file_key, glm::vec3 newPos,
    math::degrees::vec3 rotation, float scale)
{
  WMOInstance wmo_instance(file_key, _context);
  wmo_instance.uid = _model_instance_storage.new_preview_uid();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;
  wmo_instance.scale = scale;
  wmo_instance.chunk_mover_preview = true;
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();
  std::uint32_t const uid = _model_instance_storage.add_wmo_instance(
      std::move(wmo_instance), false, false);
  WMOInstance* instance = _model_instance_storage.get_wmo_instance(uid).value();
  auto const& extents = instance->getExtents();
  TileIndex const start(extents[0]);
  TileIndex const end(extents[1]);
  if (start.is_valid() && end.is_valid())
    for (std::size_t z = start.z; z <= end.z; ++z)
      for (std::size_t x = start.x; x <= end.x; ++x)
        if (MapTile* tile = mapIndex.getTile(TileIndex{x, z}); tile && tile->finishedLoading())
          tile->add_model(instance);
  return instance;
}

bool World::updateChunkMoverPreviewInstance(std::uint32_t uid, glm::vec3 new_pos,
                                            math::degrees::vec3 rotation, float scale)
{
  auto instance = _model_instance_storage.get_instance(uid);
  if (!instance)
    return false;

  SceneObject* object = std::get<selected_object_type>(*instance);
  if (!object->chunk_mover_preview)
    return false;

  // Preview objects are viewport-only, so update their tile references directly
  // instead of deleting/reconstructing the model and waiting on its asset again.
  std::vector<MapTile*> const old_tiles = object->getTiles();
  for (MapTile* tile : old_tiles)
    if (tile)
      tile->remove_model(object);

  object->pos = new_pos;
  object->dir = rotation;
  object->scale = scale;
  object->recalcExtents();

  auto const& extents = object->getExtents();
  TileIndex const start(extents[0]);
  TileIndex const end(extents[1]);
  if (start.is_valid() && end.is_valid())
    for (std::size_t z = start.z; z <= end.z; ++z)
      for (std::size_t x = start.x; x <= end.x; ++x)
        if (MapTile* tile = mapIndex.getTile(TileIndex{x, z}); tile && tile->finishedLoading())
          tile->add_model(object);

  need_model_updates = true;
  return true;
}

void World::deleteChunkMoverPreviewInstance(std::uint32_t uid)
{
  auto instance = _model_instance_storage.get_instance(uid);
  if (!instance)
    return;

  SceneObject* object = std::get<selected_object_type>(*instance);
  if (!object->chunk_mover_preview)
    return;

  std::vector<MapTile*> const referenced_tiles = object->getTiles();
  for (MapTile* tile : referenced_tiles)
    if (tile)
      tile->remove_model(object);
  _model_instance_storage.delete_preview_instance(uid);
}


std::uint32_t World::add_model_instance(ModelInstance model_instance, bool from_reloading, bool action)
{
  ZoneScoped;
  return _model_instance_storage.add_model_instance(std::move(model_instance), from_reloading, action);
}

std::uint32_t World::add_wmo_instance(WMOInstance wmo_instance, bool from_reloading, bool action)
{
  ZoneScoped;
  // Check if WMO has a low resolution model
  // also sets up all attributes currently
  bool haslowres = horizon.wmoHasLowRes(&wmo_instance);

  return _model_instance_storage.add_wmo_instance(std::move(wmo_instance), from_reloading, action);

  // if (haslowres)
  // {
  //   const auto obj = get_model(uid_after);
  //   assert(obj);
  //   if (obj)
  //   {
  //     WMOInstance* instance = static_cast<WMOInstance*>(std::get<selected_object_type>(obj.value()));
  // 
  //     int breakpoint = 0;
  //   }
  // }
  // 
  // return uid_after;
}

std::optional<selection_type> World::get_model(std::uint32_t uid)
{
  ZoneScoped;
  return _model_instance_storage.get_instance(uid);
}

void World::remove_models_if_needed(std::vector<uint32_t> const& uids)
{
  ZoneScoped;
  // todo: manage instances properly
  // don't unload anything during the uid fix all,
  // otherwise models spanning several adts will be unloaded too soon
  if (mapIndex.uid_fix_all_in_progress())
  {
    return;
  }

  for (uint32_t uid : uids)
  {
    // Chunk-mover ghosts are owned by ChunkClipboard rather than by an ADT.
    // A ghost can span several tiles, but it is inserted into storage only
    // once. Letting the first unloading tile decrement the normal ADT
    // reference count erases the shared instance while other loaded tiles
    // still retain its pointer. The clipboard removes the ghost explicitly.
    if (auto instance = _model_instance_storage.get_instance(uid))
    {
      SceneObject* object = std::get<selected_object_type>(*instance);
      if (object->chunk_mover_preview)
        continue;
    }

    // it handles the removal from the selection if necessary
    _model_instance_storage.unload_instance_and_remove_from_selection_if_necessary(uid);
  }

  // deselect the terrain when an adt is unloaded
  if (_current_selection.size() == 1 && _current_selection.at(0).index() == eEntry_MapChunk)
  {
    reset_selection();
  }
  else
  {
    update_selection_pivot();
  }
  /*
  if (uids.size())
  {
    need_model_updates = true;
  }*/
}

void World::reload_tile(TileIndex const& tile)
{
  ZoneScoped;
  reset_selection();
  mapIndex.reloadTile(tile);
}

void World::deleteObjects(std::vector<selected_object_type> const& types, bool action)
{
  ZoneScoped;
  _model_instance_storage.delete_instances(types, action);
  need_model_updates = true;
}

void World::updateTilesEntry(selection_type const& entry, model_update type)
{
  ZoneScoped;
  if (entry.index() != eEntry_Object)
    return;

  auto obj = std::get<selected_object_type>(entry);

  if (obj->which() == eWMO)
    updateTilesWMO (static_cast<WMOInstance*>(obj), type);
  else if (obj->which() == eMODEL)
    updateTilesModel (static_cast<ModelInstance*>(obj), type);

}


void World::updateTilesEntry(SceneObject* entry, model_update type)
{
  ZoneScoped;
  if (entry->which() == eWMO)
    updateTilesWMO (static_cast<WMOInstance*>(entry), type);
  else if (entry->which() == eMODEL)
    updateTilesModel (static_cast<ModelInstance*>(entry), type);

}

void World::updateTilesWMO(WMOInstance* wmo, model_update type)
{
  ZoneScoped;
  _tile_update_queue.queue_update(wmo, type);
}

void World::updateTilesModel(ModelInstance* m2, model_update type)
{
  ZoneScoped;
  _tile_update_queue.queue_update(m2, type);
}

void World::wait_for_all_tile_updates()
{
  ZoneScoped;
  _tile_update_queue.wait_for_all_update();
}

unsigned int World::getMapID() const
{
  ZoneScoped;
  return mapIndex._map_id;
}

void World::clearTextures(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
  });
}


void World::exportADTAlphamap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QDir dir(path + "/world/maps/" + basename.c_str());
      if (!dir.exists())
        dir.mkpath(".");

      for (int i = 1; i < 4; ++i)
      {
        QImage img = tile->getAlphamapImage(i);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
        + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
        + "_layer" + std::to_string(i).c_str() + ".png", "PNG");
      }

    }
  );
}

void World::exportADTNormalmap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
      {
        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = tile->getNormalmapImage();
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                 + "_normal.png", "PNG");
      }
  );
}

void World::exportADTAlphamap(glm::vec3 const& pos, std::string const& filename)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QDir dir(path + "/world/maps/" + basename.c_str());
      if (!dir.exists())
        dir.mkpath(".");

      QString tex(filename.c_str());
      QImage img = tile->getAlphamapImage(filename);
      img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
               + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
               + "_" + tex.replace("/", "-") + ".png", "PNG");

    }
  );
}

void World::exportADTHeightmap(glm::vec3 const& pos, float min_height, float max_height)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
                {
                  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
                  if (!(path.endsWith('\\') || path.endsWith('/')))
                  {
                    path += "/";
                  }

                  QDir dir(path + "/world/maps/" + basename.c_str());
                  if (!dir.exists())
                    dir.mkpath(".");

                  QImage img = tile->getHeightmapImage(min_height, max_height);
                  img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_height.png", "PNG");


                }
  );
}

void World::exportADTVertexColorMap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
                {
                  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
                  if (!(path.endsWith('\\') || path.endsWith('/')))
                  {
                    path += "/";
                  }

                  QDir dir(path + "/world/maps/" + basename.c_str());
                  if (!dir.exists())
                    dir.mkpath(".");

                  QImage img = tile->getVertexColorsImage();
                  img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_vcol.png", "PNG");


                }
  );
}

void World::importADTAlphamap(glm::vec3 const& pos, QImage const& image, unsigned layer, bool cleanup)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
  });

  if (image.width() != 1024 || image.height() != 1024)
  {
    QImage scaled = image.scaled(1024, 1024, Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
                  {
                    tile->setAlphaImage(scaled, layer, cleanup);
                  }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setAlphaImage(image, layer, cleanup);
      }
    );
  }

}

void World::importADTAlphamap(glm::vec3 const& pos, bool cleanup)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
  });

  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      for (int i = 1; i < 4; ++i)
      {
        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                       + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                       + "_layer" +  std::to_string(i).c_str() + ".png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        if (img.width() != 1024 || img.height() != 1024)
          img = img.scaled(1024, 1024, Qt::AspectRatioMode::IgnoreAspectRatio);

        tile->setAlphaImage(img, i, true);
      }

    }
  );
}

void World::importADTHeightmap(glm::vec3 const& pos, QImage const& image, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  int desired_dimensions = tiledEdges ? 256 : 257;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
  });

  if (image.width() != desired_dimensions || image.height() != desired_dimensions)
  {
    QImage scaled = image.scaled(desired_dimensions, desired_dimensions, Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setHeightmapImage(scaled, min_height, max_height, mode, tiledEdges);
      }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setHeightmapImage(image, min_height, max_height, mode, tiledEdges);
      }
    );
  }
}

void World::importADTHeightmap(glm::vec3 const& pos, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {

      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                         + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                         + "_height" + ".png";

      if (!QFileInfo::exists(filename))
      {
          QMessageBox::warning
          (nullptr
              , "File not found"
              , "File not found: " + filename
              , QMessageBox::Ok
          );
        return;
      }


      for_all_chunks_on_tile(pos, [](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
      });

      QImage img;
      img.load(filename, "PNG");

      size_t desiredSize = tiledEdges ? 256 : 257;
      if (img.width() != desiredSize || img.height() != desiredSize)
        img = img.scaled(static_cast<int>(desiredSize), static_cast<int>(desiredSize), Qt::AspectRatioMode::IgnoreAspectRatio);

      tile->setHeightmapImage(img, min_height, max_height, mode, tiledEdges);

    }
  );
}

void World::importADTWatermap(glm::vec3 const& pos, QImage const& image, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
    ZoneScoped;
    int desired_dimensions = tiledEdges ? 256 : 257;
    for_all_chunks_on_tile(pos, [](MapChunk* chunk)
        {
            NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
        });

    if (image.width() != desired_dimensions || image.height() != desired_dimensions)
    {
        QImage scaled = image.scaled(desired_dimensions, desired_dimensions, Qt::AspectRatioMode::IgnoreAspectRatio);

        for_tile_at(pos
            , [&](MapTile* tile)
            {
                tile->Water.setWatermapImage(scaled, min_height, max_height, mode, tiledEdges);
            }
        );

    }
    else
    {
        for_tile_at(pos
            , [&](MapTile* tile)
            {
                tile->Water.setWatermapImage(image, min_height, max_height, mode, tiledEdges);
            }
        );
    }
}

void World::importADTVertexColorMap(glm::vec3 const& pos, int mode, bool tiledEdges)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
      {

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_vcol" + ".png";

        if(!QFileInfo::exists(filename))
          return;

        for_all_chunks_on_tile(pos, [](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        });

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
          img = img.scaled(static_cast<int>(desiredSize), static_cast<int>(desiredSize), Qt::AspectRatioMode::IgnoreAspectRatio);

        tile->setVertexColorImage(img, mode, tiledEdges);

      }
  );
}

void World::ensureAllTilesetsADT(glm::vec3 const& pos)
{
  ZoneScoped;
  static QStringList textures {"tileset/generic/black.blp",
                               "tileset/generic/red.blp",
                               "tileset/generic/green.blp",
                               "tileset/generic/blue.blp",};

  for_all_chunks_on_tile(pos, [=](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);

    for (int i = 0; i < 4; ++i)
    {
      if (chunk->texture_set->num() <= i)
      {
        scoped_blp_texture_reference tex {textures[i].toStdString(), Noggit::NoggitRenderContext::MAP_VIEW};
        chunk->texture_set->addTexture(tex);
      }
    }

  });
}

void World::importADTVertexColorMap(glm::vec3 const& pos, QImage const& image, int mode, bool tiledEdges)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
  });

  size_t desiredDimensions = tiledEdges ? 256 : 257;

  if (image.width() != desiredDimensions || image.height() != desiredDimensions)
  {
    QImage scaled = image.scaled(static_cast<int>(desiredDimensions), static_cast<int>(desiredDimensions), Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
        {
          tile->setVertexColorImage(scaled, mode, tiledEdges);
        }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
        {
          tile->setVertexColorImage(image, mode, tiledEdges);
        }
    );
  }
}

void World::setBaseTexture(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
    if (!!Noggit::Ui::selected_texture::get())
    {
      chunk->addTexture(*Noggit::Ui::selected_texture::get());
    }
  });
}

void World::clear_shadows(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkShadowChange(chunk);
    chunk->clear_shadows();
  });
}

constexpr float HALFSHADOWSIZE = (TEXDETAILSIZE / 2.0f);

void World::swapTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex)
{
  ZoneScoped;
  if (auto const replacement_texture = Noggit::Ui::selected_texture::get())
  {
    swapTextureOnTile(mapIndex.getTile(pos), tex, *replacement_texture);
  }
}

std::size_t World::swapTextureOnTile(MapTile* tile,
                                     scoped_blp_texture_reference const& texture_to_replace,
                                     scoped_blp_texture_reference const& replacement_texture)
{
  ZoneScoped;
  if (!tile || !tile->finishedLoading() || tile->loading_failed()
      || texture_to_replace == replacement_texture)
  {
    return 0;
  }

  std::size_t changed_chunks = 0;
  for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
  {
    for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
    {
      MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);
      if (!chunk || chunk->getTextureSet()->texture_id(texture_to_replace) < 0)
      {
        continue;
      }

      NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
      if (chunk->switchTexture(texture_to_replace, replacement_texture))
      {
        ++changed_chunks;
      }
    }
  }

  if (changed_chunks)
  {
    mapIndex.setChanged(tile);
  }

  return changed_chunks;
}

std::size_t World::swapTexturesOnTile(
    MapTile* tile,
    std::vector<std::pair<scoped_blp_texture_reference, scoped_blp_texture_reference>> const& replacements)
{
  ZoneScoped;
  if (!tile || !tile->finishedLoading() || tile->loading_failed() || replacements.empty())
  {
    return 0;
  }

  std::size_t changed_chunks = 0;
  for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
  {
    for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
    {
      MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);
      if (!chunk)
      {
        continue;
      }

      bool has_source = false;
      for (auto const& replacement : replacements)
      {
        if (replacement.first != replacement.second
            && chunk->getTextureSet()->texture_id(replacement.first) >= 0)
        {
          has_source = true;
          break;
        }
      }
      if (!has_source)
      {
        continue;
      }

      NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
      if (chunk->switchTextures(replacements))
      {
        ++changed_chunks;
      }
    }
  }

  if (changed_chunks)
  {
    mapIndex.setChanged(tile);
  }

  return changed_chunks;
}

void World::swapTextureGlobal(scoped_blp_texture_reference tex)
{
    ZoneScoped;
    if (!!Noggit::Ui::selected_texture::get())
    {

        for (size_t z = 0; z < 64; z++)
        {
            for (size_t x = 0; x < 64; x++)
            {
                TileIndex tile(x, z);

                bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
                MapTile* mTile = mapIndex.loadTile(tile);

                if (mTile)
                {
                    mTile->wait_until_loaded();

                    bool tile_changed = false;
                    for_all_chunks_on_tile(mTile, [&](MapChunk* chunk)
                    {
                        // NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                        bool swapped = chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
                        if (swapped)
                            tile_changed = true;
                    });

                    if (tile_changed)
                    {
                        mTile->saveTile(this);
                        mapIndex.markOnDisc(tile, true);
                        mapIndex.unsetChanged(tile);
                    }

                    if (unload)
                    {
                        mapIndex.unloadTile(tile);
                    }
                }
            }
        }
    }
}

void World::removeTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex)
{
    ZoneScoped;
    if (!!Noggit::Ui::selected_texture::get())
    {
        for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
            {
                NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                // chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
                chunk->eraseTexture(tex);
            });
    }
}


void World::removeTexDuplicateOnADT(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->texture_set->removeDuplicate();
  } );
}

void World::change_texture_flags(glm::vec3 const& pos, scoped_blp_texture_reference const& tex, std::size_t flags)
{
  ZoneScoped;
  for_chunk_at(pos, [&] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->change_texture_flags(tex, flags);
  });
}

std::optional<glm::vec3> World::intersectLiquid(math::ray const& ray, int target_layer,
                                                std::uint64_t surface_token)
{
  std::optional<float> nearest_distance;

  auto test_layer = [&](ChunkWater const* chunk_water, liquid_layer const& layer)
  {
    glm::vec3 const bounds_min{chunk_water->xbase, layer.min() - 0.01f,
                               chunk_water->zbase};
    glm::vec3 const bounds_max{chunk_water->xbase + CHUNKSIZE, layer.max() + 0.01f,
                               chunk_water->zbase + CHUNKSIZE};
    if (!ray.intersect_bounds(bounds_min, bounds_max))
      return;

    auto const& vertices = layer.getVertices();
    for (int z = 0; z < 8; ++z)
      for (int x = 0; x < 8; ++x)
      {
        if (!layer.hasSubchunk(x, z))
          continue;

        int const id = z * 9 + x;
        for (auto const triangle : {std::array<int, 3>{id, id + 9, id + 1},
                                    std::array<int, 3>{id + 1, id + 9, id + 10}})
        {
          auto const distance = ray.intersect_triangle(vertices[triangle[0]].position,
                                                       vertices[triangle[1]].position,
                                                       vertices[triangle[2]].position);
          if (distance && (!nearest_distance || *distance < *nearest_distance))
            nearest_distance = *distance;
        }
      }
  };

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    if (!tile || !tile->finishedLoading())
      continue;

    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
      {
        ChunkWater* chunk_water = tile->Water.getChunk(x, z);
        auto const* layers = chunk_water->getLayers();

        if (surface_token)
        {
          auto const found = std::find_if(layers->begin(), layers->end(),
            [surface_token](liquid_layer const& layer)
            {
              return layer.surfaceToken() == surface_token;
            });
          if (found != layers->end())
            test_layer(chunk_water, *found);
        }
        else if (target_layer >= 0 && target_layer < static_cast<int>(layers->size()))
        {
          test_layer(chunk_water, (*layers)[target_layer]);
        }
        else if (target_layer < 0)
        {
          for (liquid_layer const& layer : *layers)
            test_layer(chunk_water, layer);
        }
      }
  }

  return nearest_distance ? std::optional<glm::vec3>{ray.position(*nearest_distance)}
                          : std::nullopt;
}

void World::paintLiquid( glm::vec3 const& pos
                       , float radius
                       , int liquid_id
                       , bool add
                       , math::radians const& angle
                       , math::radians const& orientation
                       , bool lock
                       , glm::vec3 const& origin
                       , bool override_height
                       , bool override_liquid_id
                       , float opacity_factor
                       , int target_layer
                       , std::uint64_t surface_token
                       )
{
  ZoneScoped;

  std::vector<MapChunk*> paint_chunks;
  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk)
  {
    paint_chunks.push_back(chunk);
    return true;
  });

  bool effective_lock = lock;
  glm::vec3 effective_origin = origin;
  bool const extending_coverage = add && !override_height && !lock && target_layer >= 0;
  std::unordered_map<LiquidVertexKey, float, LiquidVertexKeyHash> original_heights;
  std::unordered_map<LiquidVertexKey, float, LiquidVertexKeyHash> edge_plane_heights;
  std::unordered_map<MapChunk*, std::array<bool, 9 * 9>> vertices_used_before;

  // Coverage extends the selected surface at its existing elevation. Search
  // one liquid cell beyond the brush so painting across a chunk boundary can
  // inherit the adjacent chunk's height as well.
  if (extending_coverage)
  {
    std::vector<MapChunk*> sample_chunks;
    for_all_chunks_in_range(pos, radius + UNITSIZE, [&](MapChunk* chunk)
    {
      sample_chunks.push_back(chunk);
      return true;
    });

    auto existing_vertices = collectLiquidVertices(sample_chunks, target_layer, surface_token);
    float closest_distance_squared = std::numeric_limits<float>::max();
    std::optional<float> inherited_height;
    for (auto const& [key, group] : existing_vertices)
    {
      float const height = group.originalHeight();
      original_heights.emplace(key, height);

      float const dx = group.x - pos.x;
      float const dz = group.z - pos.z;
      float const distance_squared = dx * dx + dz * dz;
      if (distance_squared < closest_distance_squared)
      {
        closest_distance_squared = distance_squared;
        inherited_height = height;
      }
    }

    if (inherited_height)
    {
      effective_lock = true;
      effective_origin = {pos.x, *inherited_height, pos.z};

      // Layer indices are local to each MH2O chunk and are not guaranteed to
      // identify the same plane on the neighboring chunk. Build a secondary
      // edge map from every pre-existing plane, selecting the candidate closest
      // to the inherited surface height at each world-space grid coordinate.
      std::unordered_map<LiquidVertexKey, float, LiquidVertexKeyHash> best_height_delta;
      for (MapChunk* chunk : sample_chunks)
      {
        for (liquid_layer const& layer : *chunk->liquid_chunk()->getLayers())
        {
          auto const& vertices = layer.getVertices();
          for (int z = 0; z < 9; ++z)
            for (int x = 0; x < 9; ++x)
            {
              if (!layer.usesVertex(x, z))
                continue;

              glm::vec3 const& vertex = vertices[z * 9 + x].position;
              LiquidVertexKey const key = liquidVertexKey(vertex.x, vertex.z);
              float const delta = std::abs(vertex.y - *inherited_height);
              auto const best = best_height_delta.find(key);
              if (best == best_height_delta.end() || delta < best->second)
              {
                best_height_delta[key] = delta;
                edge_plane_heights[key] = vertex.y;
              }
            }
        }
      }

      // A surface token is stable across chunks, so its exact vertices win.
      // Without one, the local layer index may name a different neighboring
      // plane and the height-based match above is the safer identity.
      if (surface_token)
        for (auto const& [key, height] : original_heights)
          edge_plane_heights[key] = height;
    }

    for (MapChunk* chunk : paint_chunks)
    {
      auto& used = vertices_used_before[chunk];
      liquid_layer* layer = selectedLiquidLayer(chunk, target_layer, surface_token);
      if (!layer)
        continue;

      for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x)
          used[z * 9 + x] = layer->usesVertex(x, z);
    }
  }

  for (MapChunk* chunk : paint_chunks)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
    chunk->liquid_chunk()->paintLiquid(pos, radius, liquid_id, add, angle, orientation,
                                       effective_lock, effective_origin, override_height,
                                       override_liquid_id, chunk, opacity_factor, target_layer,
                                       surface_token);
  }

  // MH2O chunks duplicate vertices along their borders. Match only newly
  // activated copies to the exact pre-paint world-space vertex height, leaving
  // every vertex that already belonged to the surface untouched.
  if (extending_coverage && !edge_plane_heights.empty())
  {
    for (MapChunk* chunk : paint_chunks)
    {
      liquid_layer* layer = selectedLiquidLayer(chunk, target_layer, surface_token);
      if (!layer)
        continue;

      auto const snapshot = vertices_used_before.find(chunk);
      if (snapshot == vertices_used_before.end())
        continue;

      bool changed = false;
      auto& vertices = layer->getVertices();
      for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x)
        {
          int const index = z * 9 + x;
          if (snapshot->second[index] || !layer->usesVertex(x, z))
            continue;

          auto const height = edge_plane_heights.find(
            liquidVertexKey(vertices[index].position.x, vertices[index].position.z));
          if (height == edge_plane_heights.end())
            continue;

          vertices[index].position.y = height->second;
          changed = true;
        }

      if (changed)
      {
        layer->refresh();
        chunk->liquid_chunk()->update_layers();
      }
    }
  }
}

void World::paintLiquidDepth(glm::vec3 const& pos, float radius, float depth, int target_layer,
                             std::uint64_t surface_token)
{
  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
    chunk->liquid_chunk()->paintDepth(pos, radius, depth, target_layer, surface_token);
    return true;
  });
}

void World::projectLiquidUV(glm::vec3 const& pos, float radius, float scale, math::radians rotation,
                            int target_layer, std::uint64_t surface_token)
{
  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
    chunk->liquid_chunk()->projectUV(pos, radius, scale, rotation, target_layer, surface_token);
    return true;
  });
}

void World::paintLiquidAttribute(glm::vec3 const& pos, float radius, LiquidAttribute attribute,
                                 bool value, int target_layer,
                                 std::uint64_t surface_token)
{
  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
    chunk->liquid_chunk()->paintAttribute(pos, radius, attribute, value, target_layer,
                                          surface_token);
    return true;
  });
}

void World::clearLiquidAttributes(const TileIndex& pos,
                                  std::optional<LiquidAttribute> attribute)
{
  for_tile_at(pos, [&](MapTile* tile)
  {
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
      {
        MapChunk* chunk = tile->getChunk(x, z);
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
        if (attribute)
          chunk->liquid_chunk()->clearAttribute(*attribute);
        else
          chunk->liquid_chunk()->clearAttributes();
      }
  });
}

void World::regenerateLiquidAttributes(const TileIndex& pos)
{
  for_tile_at(pos, [&](MapTile* tile)
  {
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
      {
        MapChunk* chunk = tile->getChunk(x, z);
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
        chunk->liquid_chunk()->regenerateAttributes();
      }
  });
}

void World::clearLiquidFishingFlagsOutsideLiquid(const TileIndex& pos)
{
  for_tile_at(pos, [&](MapTile* tile)
  {
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
      {
        MapChunk* chunk = tile->getChunk(x, z);
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
        chunk->liquid_chunk()->clearFishableAttributesOutsideLiquid();
      }
  });
}

void World::raiseLowerLiquid(glm::vec3 const& pos, float radius, float change,
                             float inner_radius, int falloff, int target_layer,
                             std::uint64_t surface_token)
{
  std::vector<MapChunk*> chunks;
  for_all_chunks_in_range(pos, radius + UNITSIZE, [&](MapChunk* chunk)
  {
    chunks.push_back(chunk);
    return true;
  });

  auto groups = collectLiquidVertices(chunks, target_layer, surface_token);
  editLiquidVertexGroups(groups, pos, radius, inner_radius, falloff,
    [change](LiquidVertexGroup const& group, float weight) -> std::optional<float>
    {
      return group.originalHeight() + change * weight;
    });
}

void World::flattenLiquid(glm::vec3 const& pos, float radius, float strength,
                          float inner_radius, int falloff, glm::vec3 const& origin,
                          math::radians angle, math::radians orientation, int target_layer,
                          std::uint64_t surface_token)
{
  std::vector<MapChunk*> chunks;
  for_all_chunks_in_range(pos, radius + UNITSIZE, [&](MapChunk* chunk)
  {
    chunks.push_back(chunk);
    return true;
  });

  auto groups = collectLiquidVertices(chunks, target_layer, surface_token);
  editLiquidVertexGroups(groups, pos, radius, inner_radius, falloff,
    [&](LiquidVertexGroup const& group, float weight) -> std::optional<float>
    {
      glm::vec3 const vertex{group.x, group.originalHeight(), group.z};
      float const target = misc::angledHeight(origin, vertex, angle, orientation);
      return glm::mix(group.originalHeight(), target, std::clamp(strength * weight, 0.f, 1.f));
    });
}

void World::smoothLiquid(glm::vec3 const& pos, float radius, float strength,
                         float inner_radius, int falloff, int target_layer,
                         std::uint64_t surface_token)
{
  std::vector<MapChunk*> chunks;
  // Terrain blur samples a square neighborhood around the cursor before
  // applying a radial weight around each edited vertex. Include its corners.
  for_all_chunks_in_range(pos, radius * 1.5f + UNITSIZE, [&](MapChunk* chunk)
  {
    chunks.push_back(chunk);
    return true;
  });

  auto groups = collectLiquidVertices(chunks, target_layer, surface_token);
  editLiquidVertexGroups(groups, pos, radius, inner_radius, falloff,
    [&](LiquidVertexGroup const& group, float weight) -> std::optional<float>
    {
      float total = 0.f;
      float total_weight = 0.f;

      // Match terrain blur: each edited vertex averages the full liquid
      // neighborhood covered by the brush, weighted by distance from that
      // vertex. The previous 3x3 kernel could only round the lip of a height
      // step; it could not spread the transition across the brush radius.
      for (auto const& [sample_key, sample] : groups)
      {
        if (std::abs(sample.x - pos.x) > radius || std::abs(sample.z - pos.z) > radius)
          continue;

        float const dx = sample.x - group.x;
        float const dz = sample.z - group.z;
        float const sample_distance = std::sqrt(dx * dx + dz * dz);
        if (sample_distance > radius)
          continue;

        float const sample_weight = 1.f - sample_distance / radius;
        total += sample.originalHeight() * sample_weight;
        total_weight += sample_weight;
      }

      if (total_weight <= 0.f)
        return std::nullopt;

      float const target = total / total_weight;
      return glm::mix(group.originalHeight(), target, std::clamp(strength * weight, 0.f, 1.f));
    });
}

void World::setWaterType(const TileIndex& pos, int type, int layer)
{
  ZoneScoped;
  for_tile_at ( pos
              , [&] (MapTile* tile)
                {
                  for (int i = 0; i < 16; ++i)
                    for (int j = 0; j < 16; ++j)
                      NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

                  tile->Water.setType (type, layer);
                }
              );
}

int World::getWaterType(const TileIndex& tile, int layer) const
{
  ZoneScoped;
  if (mapIndex.tileLoaded(tile))
  {
    return mapIndex.getTile(tile)->Water.getType (layer);
  }
  else
  {
    return 0;
  }
}

void World::autoGenWaterTrans(const TileIndex& pos, float factor)
{
  ZoneScoped;
  for_tile_at(pos, [&](MapTile* tile)
  {
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j)
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

    tile->Water.autoGen(factor);
  });
}

void World::CleanupEmptyTexturesChunks()
{
    ZoneScoped;
    for (MapTile* tile : mapIndex.loaded_tiles())
    {
        bool tileChanged = false;

        for (unsigned ty = 0; ty < 16; ty++)
        {
            for (unsigned tx = 0; tx < 16; tx++)
            {
                MapChunk* chunk = tile->getChunk(tx, ty);

                TextureSet* texture_set = chunk->getTextureSet();

                bool changed = texture_set->eraseUnusedTextures();

                if (changed)
                {
                    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                    tileChanged = true;
                }
            }
        }
        if (tileChanged)
        {
            mapIndex.setChanged(tile);
        }
    }
}

void World::fixAllGaps()
{
  ZoneScoped;
  std::vector<MapChunk*> chunks;

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    MapTile* left = mapIndex.getTileLeft(tile);
    MapTile* above = mapIndex.getTileAbove(tile);
    bool tileChanged = false;

    // fix the gaps with the adt at the left of the current one
    if (left)
    {
      for (unsigned ty = 0; ty < 16; ty++)
      {
        MapChunk* chunk = tile->getChunk(0, ty);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (chunk->fixGapLeft(left->getChunk(15, ty)))
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }

    // fix the gaps with the adt above the current one
    if (above)
    {
      for (unsigned tx = 0; tx < 16; tx++)
      {
        MapChunk* chunk = tile->getChunk(tx, 0);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (chunk->fixGapAbove(above->getChunk(tx, 15)))
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }

    // fix gaps within the adt
    for (unsigned ty = 0; ty < 16; ty++)
    {
      for (unsigned tx = 0; tx < 16; tx++)
      {
        MapChunk* chunk = tile->getChunk(tx, ty);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        bool changed = false;

        // if the chunk isn't the first of the row
        if (tx && chunk->fixGapLeft(tile->getChunk(tx - 1, ty)))
        {
          changed = true;
        }

        // if the chunk isn't the first of the column
        if (ty && chunk->fixGapAbove(tile->getChunk(tx, ty - 1)))
        {
          changed = true;
        }

        if (changed)
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }
    if (tileChanged)
    {
      mapIndex.setChanged(tile);
    }
  }

  for (MapChunk* chunk : chunks)
  {
    recalc_norms (chunk);
  }
}

bool World::isUnderMap(glm::vec3 const& pos) const
{
  ZoneScoped;
  TileIndex const tile (pos);

  if (mapIndex.tileLoaded(tile))
  {
    unsigned chnkX = (pos.x / CHUNKSIZE) - tile.x * 16;
    unsigned chnkZ = (pos.z / CHUNKSIZE) - tile.z * 16;

    // check using the cursor height
    return (mapIndex.getTile(tile)->getChunk(chnkX, chnkZ)->getMinHeight()) > pos.y + 2.0f;
  }

  return true;
}

void World::selectVertices(glm::vec3 const& pos, float radius)
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();

  _vertex_center_updated = false;
  _vertex_border_updated = false;

  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk){
    _vertex_chunks.emplace(chunk);
    _vertex_tiles.emplace(chunk->mt);
    chunk->selectVertex(pos, radius, _vertices_selected);
    return true;
  });

}

bool World::deselectVertices(glm::vec3 const& pos, float radius)
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();

  _vertex_center_updated = false;
  _vertex_border_updated = false;
  std::unordered_set<glm::vec3*> inRange;

  for (glm::vec3* v : _vertices_selected)
  {
    if (misc::dist(*v, pos) <= radius)
    {
      inRange.emplace(v);
    }
  }

  for (glm::vec3* v : inRange)
  {
    _vertices_selected.erase(v);
  }

  return _vertices_selected.empty();
}

void World::moveVertices(float h)
{
  ZoneScoped;
  Noggit::Action* cur_action = NOGGIT_CUR_ACTION;

  assert(cur_action && "moveVertices called without an action running.");

  for (auto& chunk : _vertex_chunks)
    cur_action->registerChunkTerrainChange(chunk);

  _vertex_center_updated = false;
  for (glm::vec3* v : _vertices_selected)
  {
    v->y += h;
  }

  updateVertexCenter();
  updateSelectedVertices();
}

void World::updateSelectedVertices()
{
  ZoneScoped;
  for (MapTile* tile : _vertex_tiles)
  {
    mapIndex.setChanged(tile);
  }

  // fix only the border chunks to be more efficient
  for (MapChunk* chunk : vertexBorderChunks())
  {
    chunk->fixVertices(_vertices_selected);
  }

  for (MapChunk* chunk : _vertex_chunks)
  {
    chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
    recalc_norms (chunk);
  }
}

void World::orientVertices ( glm::vec3 const& ref_pos
                           , math::degrees vertex_angle
                           , math::degrees vertex_orientation
                           )
{
  ZoneScoped;
  Noggit::Action* cur_action = NOGGIT_CUR_ACTION;

  assert(cur_action && "orientVertices called without an action running.");

  for (auto& chunk : _vertex_chunks)
    cur_action->registerChunkTerrainChange(chunk);

  for (glm::vec3* v : _vertices_selected)
  {
    v->y = misc::angledHeight(ref_pos, *v, vertex_angle, vertex_orientation);
  }
  updateSelectedVertices();
}

void World::flattenVertices (float height)
{
  ZoneScoped;
  for (glm::vec3* v : _vertices_selected)
  {
    v->y = height;
  }
  updateSelectedVertices();
}

void World::clearVertexSelection()
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();
  _vertex_border_updated = false;
  _vertex_center_updated = false;
  _vertices_selected.clear();
  _vertex_chunks.clear();
  _vertex_tiles.clear();
}

void World::updateVertexCenter()
{
  ZoneScoped;
  _vertex_center_updated = true;
  _vertex_center = { 0,0,0 };
  float f = 1.0f / _vertices_selected.size();
  for (glm::vec3* v : _vertices_selected)
  {
    _vertex_center += (*v) * f;
  }
}

glm::vec3 const& World::vertexCenter()
{
  ZoneScoped;
  if (!_vertex_center_updated)
  {
    updateVertexCenter();
  }

  return _vertex_center;
}

std::unordered_set<MapChunk*>& World::vertexBorderChunks()
{
  ZoneScoped;
  if (!_vertex_border_updated)
  {
    _vertex_border_updated = true;
    _vertex_border_chunks.clear();

    for (MapChunk* chunk : _vertex_chunks)
    {
      if (chunk->isBorderChunk(_vertices_selected))
      {
        _vertex_border_chunks.emplace(chunk);
      }
    }
  }
  return _vertex_border_chunks;
}
/*
void World::update_models_by_filename()
{
  ZoneScoped;
  _models_by_filename.clear();

  _model_instance_storage.for_each_m2_instance([&] (ModelInstance& model_instance)
  {
    _models_by_filename[model_instance.model->file_key().filepath()].push_back(&model_instance);
    // to make sure the transform matrix are up to date
    model_instance.ensureExtents();
  });

  need_model_updates = false;
}
*/
void World::range_add_to_selection(glm::vec3 const& pos, float radius, bool remove)
{
  ZoneScoped;

  auto objects_in_range = getObjectsInRange(pos, radius);

  for (auto obj : objects_in_range)
  {
    if (remove)
    {
      remove_from_selection(obj, false, false);
    }
    else
    {
      add_to_selection(obj, false, false);
    }
  }
  update_selection_pivot();
}

Noggit::world_model_instances_storage& World::getModelInstanceStorage()
{
  return _model_instance_storage;
}

float World::getMaxTileHeight(const TileIndex& tile)
{
  ZoneScoped;
  MapTile* m_tile = mapIndex.getTile(tile);

  m_tile->forceRecalcExtents();
  float max_height = m_tile->getMaxHeight();

  std::vector<uint32_t>* uids = m_tile->get_uids();

  for (uint32_t uid : *uids)
  {
    auto instance = _model_instance_storage.get_instance(uid);

    if (instance.value().index() == eEntry_Object)
    {
      auto obj = std::get<selected_object_type>(instance.value());
      obj->ensureExtents();
      max_height = std::max(max_height, std::max(obj->getExtents()[0].y, obj->getExtents()[1].y));
    }
  }


  return max_height;
}

SceneObject* World::getObjectInstance(std::uint32_t uid)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_instance(uid);

  if (!instance)
    return nullptr;

  if (instance.value().index() == eEntry_Object)
  {
    return std::get<selected_object_type>(instance.value());
  }

  return nullptr;
}

void World::setBasename(const std::string &name)
{
  ZoneScoped;
  basename = name;
  mapIndex.set_basename(name);
}


Noggit::VertexSelectionCache World::getVertexSelectionCache()
{
  ZoneScoped;
  return std::move(Noggit::VertexSelectionCache{_vertex_tiles, _vertex_chunks, _vertex_border_chunks,
                                                _vertices_selected, _vertex_center});
}

void World::setVertexSelectionCache(Noggit::VertexSelectionCache& cache)
{
  ZoneScoped;
  _vertex_tiles = cache.vertex_tiles;
  _vertex_chunks = cache.vertex_chunks;
  _vertex_border_chunks = cache.vertex_border_chunks;
  _vertices_selected = cache.vertices_selected;
  _vertex_center = cache.vertex_center;

  _vertex_center_updated = false;
  _vertex_border_updated = false;
}

void World::exportAllADTsAlphamap()
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        for (int i = 1; i < 4; ++i)
        {
          QImage img = mTile->getAlphamapImage(i);
          img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                   + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                   + "_layer" + std::to_string(i).c_str() + ".png", "PNG");
        }

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsAlphamap(const std::string& filename)
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        bool found = false;

        for (int i = 0; i < 16; ++i)
        {
          for (int j = 0; j < 16; ++j)
          {
            auto chunk = mTile->getChunk(i, j);

            for (int k = 1; k < chunk->texture_set->num(); ++k)
            {
              if (chunk->texture_set->filename(k) == filename)
              {
                found = true;
                break;
              }
            }
          }
        }

        if (!found)
          continue;

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QString tex(filename.c_str());
        QImage img = mTile->getAlphamapImage(filename);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_" + tex.replace("/", "-") + ".png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsHeightmap()
{
  ZoneScoped;
  float min_height = std::numeric_limits<float>::max();
  float max_height = std::numeric_limits<float>::lowest();

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        float max = mTile->getMaxHeight();
        float min = mTile->getMinHeight();

        if (max_height < max)
          max_height = max;

        if (min_height > min)
          min_height = min;

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = mTile->getHeightmapImage(min_height, max_height);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_height.png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsVertexColorMap()
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = mTile->getVertexColorsImage();
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_vcol.png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::importAllADTsAlphamaps(QProgressDialog* progress_dialog)
{
  bool clean_up = false;
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  // use batch to not load the entire map at once, takes 50+gb for 64x64
  // memory cost is about 5.5mb per tile
  constexpr int MAX_IN_FLIGHT = 256; // 128 costs 700mb

  int count = 0;

  std::unordered_map< unsigned int, bool> unloads;

  std::vector<TileIndex> allTiles;
  allTiles.reserve(progress_dialog->maximum());

  // collect all tiles
  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      TileIndex tile(x, z);
      if (!mapIndex.hasTile(tile))
        continue;

      bool shouldUnload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      unloads[tile.index()] = shouldUnload;

      allTiles.push_back(tile);
    }
  }

  assert(progress_dialog->maximum() == allTiles.size());
  const int total = allTiles.size();

  std::deque<MapTile*> inFlight;
  size_t nextTileIdx = 0;
  size_t processed = 0;

  auto processTile = [&](MapTile* mTile)
    {
    if (!mTile)
      return;

    // ensure loaded
    mTile->wait_until_loaded();

    for (int layer = 1; layer < 4; ++layer)
    {
      QString filename = QString("%1/world/maps/%2/%2_%3_%4_layer%5.png")
        .arg(path)
        .arg(basename.c_str())
        .arg(mTile->index.x)
        .arg(mTile->index.z)
        .arg(layer);

      if (!QFileInfo::exists(filename))
        continue;

      QImage img;
      img.load(filename, "PNG");
      if (img.width() != 1024 || img.height() != 1024)
        img = img.scaled(1024, 1024, Qt::IgnoreAspectRatio);

      mTile->setAlphaImage(img, layer, clean_up);
    }

    mTile->saveTile(this);
    auto idx = mTile->index;
    mapIndex.markOnDisc(idx, true);
    mapIndex.unsetChanged(idx);

    auto it = unloads.find(idx.index());
    if (it != unloads.end() && it->second)
      mapIndex.unloadTile(idx);

    ++processed;
    if ((processed % 5) == 0 || processed == total)
      progress_dialog->setValue(static_cast<int>(processed));
    };

  auto tryProcessFinished = [&]() {
    for (auto it = inFlight.begin(); it != inFlight.end(); )
    {
      if (progress_dialog->wasCanceled())
        return;

      MapTile* mt = *it;
      if (mt->finishedLoading())
      {
        processTile(mt);
        it = inFlight.erase(it);
      }
      else
      {
        ++it;
      }
    }
    };

  while (processed < total)
  {
    if (progress_dialog->wasCanceled())
      return;

    tryProcessFinished();

    // queue new tiles while we have capacity
    while (nextTileIdx < total && inFlight.size() < MAX_IN_FLIGHT)
    {
      const TileIndex& t = allTiles[nextTileIdx++];

      MapTile* mTile = mapIndex.loadTile(t);
      if (!mTile)
        continue;

      // if it is already finished loading, process immediately
      if (mTile->finishedLoading())
      {
        processTile(mTile);
      }
      else
      {
        inFlight.push_back(mTile);
      }
    }

    // if queue is full and nothing finished, block minimally on the oldest
    if (!inFlight.empty() && inFlight.size() >= MAX_IN_FLIGHT)
    {
      MapTile* oldest = inFlight.front();
      if (!oldest->finishedLoading())
      {
        // minimal blocking only because we are at capacity
        oldest->wait_until_loaded();
      }
      // process oldest
      processTile(oldest);
      inFlight.pop_front();
    }

    // if there's spare capacity but no new tiles to queue, try processing again
    if (nextTileIdx >= total && !inFlight.empty())
    {
      tryProcessFinished();
      // If still not finished, block on front
      if (!inFlight.empty() && !inFlight.front()->finishedLoading())
      {
        inFlight.front()->wait_until_loaded();
        processTile(inFlight.front());
        inFlight.pop_front();
      }
    }
  }

  // flush any remaining in-flight tiles
  while (!inFlight.empty())
  {
    MapTile* mt = inFlight.front();
    inFlight.pop_front();
    if (!mt->finishedLoading())
      mt->wait_until_loaded();
    processTile(mt);
  }

  progress_dialog->setValue(static_cast<int>(processed));

}

void World::importAllADTsHeightmaps(QProgressDialog* progress_dialog, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  int count = 0;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      if (progress_dialog->wasCanceled())
        return;
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);
      
      if (mTile)
      {
        mTile->wait_until_loaded();
      
        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                           + "_height.png";
      
        if (!QFileInfo::exists(filename))
            continue;
      
        QImage img;
        img.load(filename, "PNG");
      
        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
        {
          QImage scaled = img.scaled(257, 257, Qt::IgnoreAspectRatio);
          mTile->setHeightmapImage(scaled, min_height, max_height, mode, tiledEdges);
        }
        else
        {
          mTile->setHeightmapImage(img, min_height, max_height, mode, tiledEdges);
        }
      
        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);
      
        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
        count++;
        progress_dialog->setValue(count);
      }
    }
  }
}

void World::importAllADTVertexColorMaps(unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                           + "_vcol.png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
        {
          QImage scaled = img.scaled(257, 257, Qt::IgnoreAspectRatio);
          mTile->setVertexColorImage(scaled, mode, tiledEdges);
        }
        else
        {
          mTile->setVertexColorImage(img, mode, tiledEdges);
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::ensureAllTilesetsAllADTs()
{
  ZoneScoped;
  static QStringList textures {"tileset/generic/black.blp",
                               "tileset/generic/red.blp",
                               "tileset/generic/green.blp",
                               "tileset/generic/blue.blp",};

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        for (int i = 0; i < 16; ++i)
        {
          for (int j = 0; j < 16; ++j)
          {
            auto chunk = mTile->getChunk(i, j);

            for (int i = 0; i < 4; ++i)
            {
              if (chunk->texture_set->num() <= i)
              {
                scoped_blp_texture_reference tex {textures[i].toStdString(), Noggit::NoggitRenderContext::MAP_VIEW};
                chunk->texture_set->addTexture(tex);
              }
            }

          }
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::notifyTileRendererOnSelectedTextureChange()
{
  ZoneScoped;

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    tile->renderer()->notifyTileRendererOnSelectedTextureChange();
  }
}

void World::select_objects_in_area(
  const std::array<glm::vec2, 2>& selection_box, 
  bool reset_selection,
  const glm::mat4x4& view,
  const glm::mat4x4& projection,
  int viewport_width, 
  int viewport_height,
  float user_depth,
  const glm::vec3& camera_position)
{
  ZoneScoped;
  
  if (reset_selection)
  {
    this->reset_selection();
  }

  glm::mat4 VPmatrix = projection * view;
  glm::mat4x4 const invertedProjViewMatrix = glm::inverse(VPmatrix);
  auto const transposed_view = glm::transpose(view);

  constexpr int max_position_raycast_processing = 10000;
  constexpr int max_bounds_raycast_processing = 5000; // when selecting large amount of objects, avoid doing complex ray calculations to not freeze
  constexpr float bounds_check_scale = 0.9f; // size of the bounding box to use when interesecting with selection rectangle
  constexpr float obj_raycast_min_size = 30.0f; // screen size rectangle lenght in pixels

  int processed_obj_count = 0; // num objects that had a raycast test at least once
  // int debug_count_obj_min_size = 0;
  // int debug_count_obj_min_size_not = 0;

  for (auto& map_object : _loaded_tiles_buffer)
  {
    MapTile* tile = map_object.second;

    if (!tile)
    {
      break;
    }

    // some optimizations to see if the tile is in selection before iterating objects in it
    {
      // tile not in screen, skip
      // frustum.intersects(tile_extents[1], tile_extents[0])
      if (!tile->_was_rendered_last_frame)
        continue;

      // check if tile combined extents are within selection rectangle
      // note very useful because cases where a tile is fully rendered and not selected are very rare
 
      // skip if no objects
      if (tile->getObjectInstances().empty())
        continue;

      // bool valid = false;
      // auto screenBounds = misc::getBoundingBoxScreenBounds(tile->getCombinedExtents(), VPmatrix
      //   , viewport_width, viewport_height, valid,  1.0f);
      // 
      // // this only works if all tile points are in screen space
      // if (valid && !math::boxIntersects(screenBounds[0], screenBounds[1]
      //   , selection_box[0], selection_box[1]))
      // {
      //   continue;
      // }
    }

    for (auto const& pair : tile->getObjectInstances())
    {
      [[unlikely]]
      if (!(pair.first->finishedLoading()) || pair.first->loading_failed())
        continue;

      [[unlikely]]
      if (pair.second.empty())
        continue;

      SceneObjectTypes objectType = pair.second[0]->which();

      // check if object is hidden
      if (objectType == eWMO)
      {
        WMOInstance* model_instance = static_cast<WMOInstance*>(pair.second[0]);

        if (model_instance->wmo->is_hidden())
          continue;
      }
      else if (objectType == eMODEL)
      {
        ModelInstance* model_instance = static_cast<ModelInstance*>(pair.second[0]);

        if (model_instance->model->is_hidden())
          continue;
      }
      else
      [[unlikely]]
      {
        continue;
      }

      for (auto const& instance : pair.second)
      {
        // problem : M2s have additional sized based culling with >isInRenderDist()
        // if (!instance->_rendered_last_frame)
        //   continue;

        // rectangle selection Pipeline
        // 1 : regular distance checks with object's position
        // 2 : check if oriented bounding box center is in selection rectangle (2D screen projection)
        // 3 : If so, do a raycast that position and check if it's occluded (if any terrain is hit before object center point)
        // if raycast succeeded it's valid !
        // If not, continue : 
        // 4 : check if bounding box is within selection in 2D screen space to test other points (extents screen projection + rectangles intersection)
        //    ! if not, it definitely doesn't intersect, quit.
        // 5 : First, check if object takes enough screenspace, if not and center point failed, it's useless to test more
        // 6 : Now iterate a list of key points from bounding box points to check if they're occluded :
        //    - Optional : get the center of the intersection rectangle (overlap area between obj screen bounds and selection rectangle)
        //                 Project it to 3D and add it to the list of points to check
        //    - First, check if each point is in the selection rectangle (screen projection)
        //    - Now raycast each point and check if each point is occluded by terrain
        //    - if ANY point succeeds and isn't occluded, it means the object isn't entirely occluded and we can select it

        if (processed_obj_count > max_position_raycast_processing)
          break;

        // 1 : regular distance checks with object's position
        const float distance = glm::distance(camera_position, instance->pos);
        if (distance > user_depth || distance > renderer()->cullDistance())
          continue;

        math::aabb obj_world_aabb(instance->getExtents()[0], instance->getExtents()[1]);
        auto aabb_center = obj_world_aabb.center();

        bool point_valid = false;
        auto center_screen_pos = misc::projectPointToScreen(aabb_center, VPmatrix, viewport_width, viewport_height, point_valid);
        // if screenPos.w < 0.0f, object is behind camera
        // check object bounding radius instead to compare the object's size, if it clips with the camera.
        if (center_screen_pos.w < -instance->getBoundingRadius())
        {
          continue;
        }

        bool do_selection = false;
        // 2: check if position point is within rectangle first because it is much cheaper
        {
          const glm::vec2 screenPos2D = glm::vec2(center_screen_pos);
          if (misc::pointInside(screenPos2D, selection_box))
          {
            // processed_obj_count++;
            // 3: check if center point is occluded by terrain
            if (processed_obj_count < max_position_raycast_processing && 
              !is_point_occluded_by_terrain(aabb_center, transposed_view, VPmatrix, viewport_width, viewport_height, camera_position))
            {
              // if not occluded success! select it and skip other checks
              add_to_selection(instance, false, false);
              continue;
            }
            // else
            //   bool debug_breakpoint = true;
          }
        }
        
        // 4 : if center point raycast didn't succeed, check again if bounding box is within selection in 2D screen space to test other points

        std::array<glm::vec3, 2> local_extents;
        if (objectType == eWMO)
        {
          WMOInstance* wmo_instance = static_cast<WMOInstance*>(pair.second[0]);
          local_extents = wmo_instance->getLocalExtents();
        }
        else if (objectType == eMODEL)
        {
          ModelInstance* model_instance = static_cast<ModelInstance*>(pair.second[0]);
          local_extents = model_instance->getLocalExtents();
        }

        int num_valid_points = 0;
        std::array<glm::vec2, 2> obj_screnbounds = misc::getBoundingBoxScreenBounds(local_extents, VPmatrix
          , viewport_width, viewport_height, num_valid_points, instance->transformMatrix(), bounds_check_scale);
          
        LogError << "point A reached (" << std::endl;

        if (num_valid_points < 3)
          continue;

        // Screen bounds intersection check 
        // 
        // if (!math::boxIntersects(obj_screnbounds[0], obj_screnbounds[1]
        //   , selection_box[0], selection_box[1]))
        // {
        //   // if rectangles don't intersect, just skip
        //   continue;
        // }

        LogError << "point B reached (" << std::endl;

        // 1 : get the intersection rectangle of screen space and bounding box
        glm::vec2 intersectionMin = glm::max(obj_screnbounds[0], selection_box[0]);
        glm::vec2 intersectionMax = glm::min(obj_screnbounds[1], selection_box[1]);
        // Check for Valid Intersection:
        // if (intersectionMin.x < intersectionMax.x && intersectionMin.y < intersectionMax.y)
        if (!(intersectionMin.x < intersectionMax.x) || !(intersectionMin.y < intersectionMax.y))
          continue;

        LogError << "point C reached (" << std::endl;
        
        // 2 : get center
        glm::vec2 intersectionCenter = (intersectionMin + intersectionMax) * 0.5f;
        // 3 : convert 2D screenspace point back to 3d
        glm::vec4 normalisedView = invertedProjViewMatrix * misc::normalized_device_coords(intersectionCenter.x, intersectionCenter.y,
          viewport_width, viewport_height);
        glm::vec3 intersectionCenter_pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);

        std::array<glm::vec3, 8> obj_world_bounds_corners;
        // For animated models, recalc vertex bounding box
        if (objectType == eMODEL)
        {
          ModelInstance* model_instance = static_cast<ModelInstance*>(pair.second[0]);
          if (model_instance->model->animated_mesh() && model_instance->model->mesh_bounds_ratio < 0.8f)
          {
            auto animated_local_extents = model_instance->model->getAnimatedBoundingBox();

            // hack, animated coords are already adjusted
            animated_local_extents[0] = glm::vec3(animated_local_extents[0].x, -animated_local_extents[0].z, animated_local_extents[0].y);
            animated_local_extents[1] = glm::vec3(animated_local_extents[1].x, -animated_local_extents[1].z, animated_local_extents[1].y);

            // update screen bounds
            num_valid_points = 0;
            obj_screnbounds = misc::getBoundingBoxScreenBounds(animated_local_extents, VPmatrix
              , viewport_width, viewport_height, num_valid_points, instance->transformMatrix(), bounds_check_scale);


            // check if animated BB intersected
            if (num_valid_points < 3)
              continue;
            if (!math::boxIntersects(obj_screnbounds[0], obj_screnbounds[1]
              , selection_box[0], selection_box[1]))
            {
              continue;
            }

            math::aabb animated_local_aabb(animated_local_extents[0], animated_local_extents[1]);
            // converts to world
            obj_world_bounds_corners = animated_local_aabb.rotated_corners(instance->transformMatrix(), true);

            // get extents and update bb to use
            obj_world_aabb = math::aabb(std::vector<glm::vec3>(obj_world_bounds_corners.begin(), obj_world_bounds_corners.end()));

            // raycast the center of the intersecting animated bounds
            // 
            // get the center of the intersection rectangle
            // 1 : get the intersection rectangle of screen space and bounding box
            intersectionMin = glm::max(obj_screnbounds[0], selection_box[0]);
            intersectionMax = glm::min(obj_screnbounds[1], selection_box[1]);
            // Check for Valid Intersection:
            if (intersectionMin.x < intersectionMax.x && intersectionMin.y < intersectionMax.y) {
              // Valid intersection
            }
            else 
            {
              continue;
            }
            // 2 : get center
            intersectionCenter = (intersectionMin + intersectionMax) * 0.5f;
            // 3 : convert 2D screenspace point back to 3d
            normalisedView = invertedProjViewMatrix * misc::normalized_device_coords(intersectionCenter.x, intersectionCenter.y,
              viewport_width, viewport_height);
            intersectionCenter_pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);
            //////

          }
          else
          {
            obj_world_bounds_corners = obj_world_aabb.rotated_corners(instance->transformMatrix(), false);
          }
        }
        else if (objectType == eWMO)
        {
          obj_world_bounds_corners = obj_world_aabb.rotated_corners(instance->transformMatrix(), false);
        }


        // 4.5 2nd raycast. Check if center of the intersection box is visible
        // TODO : for WMOs this is way to generous due to their more complex shape, it would be better to iterate the bounding box of each group
        if (!is_point_occluded_by_terrain(intersectionCenter_pos, transposed_view, VPmatrix, viewport_width, viewport_height
          , camera_position, (distance - instance->getBoundingRadius())))
        {
          // if not occluded success! select it and skip other checks
          add_to_selection(instance, false, false);
          continue;
        }

        // 5 : Optimization : Only do raycast bounds checks for object that take enough screen space
        // if object is too small checking other points is useless
        // we check _rendered_last_frame because m2s that are too small or frustum culled already don't render
        {
          float bounds_size = glm::distance(obj_screnbounds[0], obj_screnbounds[1]);
          if (bounds_size < obj_raycast_min_size || !instance->_rendered_last_frame)
          {
            // debug_count_obj_min_size_not++;
            continue;
          }
          else if (processed_obj_count > max_bounds_raycast_processing)
          {
            // select it anyways
            do_selection = true;
            // debug_count_obj_min_size++;
          }
        }

        constexpr bool enable_bounds_raycasts = true;
        //6 : Occlusion test on object's corners (that are in selection box)
        // uses ray casting, very expensive
        if (enable_bounds_raycasts && !do_selection /* && instance->_rendered_last_frame && (processed_obj_count < max_bounds_raycast_processing)*/)
        {
          processed_obj_count++;

          // TODO : instead iterate bounds of the intersection rectangle instead of object's bounds

          // Iterate key points instead of all 8 corners
          std::vector<glm::vec3> key_points = {
            // intersectionCenter_pos, // checked in 4.5 now
            // (obj_world_bounds_corners[0] + obj_world_bounds_corners[6]) * 0.5f,  // Center between top corners
            // obj_world_bounds_corners[0], // Top-right-front
            // obj_world_bounds_corners[5], // Top-left-back
            // obj_world_bounds_corners[4], // Top-left-front
            // obj_world_bounds_corners[1] // Top-right-back
          };

          // int required_num_unoccluded_corners = 2;
          bool object_occluded = true;

          // check if points are occluded by terrain
          // bool first_point = true;// special for intersectionCenter_pos because it doesn't have a distance, just a direction

          for (const auto& corner : key_points /*obj_aabb_corners*/)
          {
            // TODO : only need to do max top left and max top right in 2d instead of all corners?

            // only process points that are within selection rectangle
            bool point_valid = false;
            auto point_screen_pos = misc::projectPointToScreen(corner, VPmatrix, viewport_width, viewport_height, point_valid);
            if (!point_valid)
              continue;
            if (!misc::pointInside(point_screen_pos, selection_box))
              continue;

            bool corner_occluded = is_point_occluded_by_terrain(corner
              , transposed_view
              , VPmatrix
              , viewport_width
              , viewport_height
              , camera_position
              /*, first_point ? distance - instance->getBoundingRadius() : 0.0f*/);

            // first_point = false;

            if (!corner_occluded)
            {
              // if just one point isn't occluded is enough, select object
              object_occluded = false;
              break;
            }
            // object_occluded = true;
          }
        
          do_selection = !object_occluded;
        }

        if (!do_selection)
          continue;

        add_to_selection(instance, false, false);
      }
    }
  }

  this->update_selection_pivot();
}


bool World::is_point_occluded_by_terrain(const glm::vec3& point,
  const glm::mat4x4& transposed_view,
  const glm::mat4& VPmatrix,
  float viewport_width,
  float viewport_height,
  const glm::vec3& camera_position,
  float distance_override
  )
{
  /*
  bool point_valid = false;
  auto point_screen_pos = misc::projectPointToScreen(point, VPmatrix, viewport_width, viewport_height, point_valid);
  
  if (!point_valid)
  {
    return true;
  }*/

  math::ray ray(camera_position, point - camera_position); // 3d display mode only.

  // intersect only terrain with a ray to object's position
  selection_result terrain_intersect_results
  (intersect
  (transposed_view
    , ray
    , true
    , false
    , true
    , false
    , false
    , false
    , false
    , false
  )
  );

  float distance = distance_override == 0.0f ? glm::distance(camera_position, point) : distance_override;

  // bool point_occluded = false;
  for (const auto& terrain_hit : terrain_intersect_results)
  {
    // if terrain hit is further, skip
    if (terrain_hit.first + 5.0f > distance) // add some leeway, skip hits that are too close, especially for the terrain at object's origin
      continue;

    return true;
  }

  // no terrain intersection point above point
  return false;
}

void World::add_object_group_from_selection()
{
    // create group from selected objects
    selection_group selection_group(get_selected_objects(), this);
    selection_group._is_selected = true;

    _selection_groups.push_back(selection_group);

    // write group to project
    saveSelectionGroups();
}

/*
void World::remove_selection_group(selection_group* group)
{
   std::vector<selection_type>::iterator position = std::find(_selection_groups.begin(), _selection_groups.end(), group);
   if (position != _selection_groups.end())
   {
       _selection_groups.erase(position);
   }

   for (auto it = _selection_groups.begin(); it != _selection_groups.end(); ++it)
   {
       auto it_group = *it;
       if (it_group.getMembers().size() == group->getMembers().size() && it_group.getExtents() == group->getExtents())
       // if (it_group.isSelected())
       {
           _selection_groups.erase(it);
           saveSelectionGroups();
           return;
       }
   }
}*/

void World::clear_selection_groups()
{
    for (auto& group : _selection_groups)
    {
        // auto it_group = *it;
        // it->remove_group();
        group.remove_group(false);
    }
    _selection_groups.clear(); // in case it didn't properly clear
    saveSelectionGroups(); // only save once
}

