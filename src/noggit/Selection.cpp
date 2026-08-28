#include <noggit/ChunkWater.hpp>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Selection.h>
#include <noggit/texture_set.hpp>
#include <noggit/World.h>

#include <sstream>


selected_chunk_type::selected_chunk_type(MapChunk* _chunk, const std::tuple<int, int, int>& _triangle, const glm::vec3& _position)
: chunk(_chunk)
, triangle(_triangle)
, position(_position)
{
    unit_index = chunk->getUnitIndextAt(position);
}

void selected_chunk_type::updateDetails(Noggit::Ui::detail_infos* detail_widget)
{
  std::stringstream select_info;

  mcnk_flags const& flags = chunk->header_flags;

  select_info << "<b>Chunk</b> (" << chunk->px << ", " << chunk->py << ") flat index: (" << chunk->py * 16 + chunk->px
      << ") of <b>tile</b> (" << chunk->mt->index.x << " , " << chunk->mt->index.z << ")"
      << "<br><b>area ID:</b> " << chunk->getAreaID() << " (\"" << gAreaDB.getAreaFullName(chunk->getAreaID()) << "\")"
      << "<br><b>flags</b>: "
      << (flags.flags.has_mcsh ? "<br>shadows " : "")
      << (flags.flags.impass ? "<br>impassable " : "")
      << (flags.flags.lq_river ? "<br>river " : "")
      << (flags.flags.lq_ocean ? "<br>ocean " : "")
      << (flags.flags.lq_magma ? "<br>lava" : "")
      << (flags.flags.lq_slime ? "<br>slime" : "");

  select_info << "\n<br><b>Chunk Unit</b> (" << unit_index.x << ", " << unit_index.y << ")"
      << "<br><b>Chunk Unit Effect Doodads disabled</b>: "
      << (chunk->getTextureSet()->getDoodadDisabledAt(unit_index.x, unit_index.y) ? "True" : "False")
      << "<br><b>Chunk Unit Active Doodad Effect Layer </b>: "
      << int(chunk->getTextureSet()->getDoodadActiveLayerIdAt(unit_index.x, unit_index.y))
      << ""
      <<"\n";


  std::array<float, 4> unit_texture_weights = chunk->getTextureSet()->get_textures_weight_for_unit(unit_index.x, unit_index.y);
  if (chunk->getTextureSet()->num())
  {
      select_info << "\n<br><b>DEBUG Chunk Unit texture weights:</b>"
          << "<br>0:" << unit_texture_weights[0] << "%";
  }
  if (chunk->getTextureSet()->num()>1)
      select_info << "<br>1:" << unit_texture_weights[1] << "%";
  if (chunk->getTextureSet()->num() > 2)
      select_info << "<br>2:" << unit_texture_weights[2] << "%";
  if (chunk->getTextureSet()->num() > 3)
      select_info << "<br>3:" << unit_texture_weights[3] << "%";


  // liquid details if the chunk has liquid data
  if (chunk->mt->Water.hasData(0))
  {
      ChunkWater* waterchunk = chunk->liquid_chunk();

      MH2O_Attributes attributes = waterchunk->getAttributes();

      if (waterchunk->hasData(0))
      {
          
          liquid_layer liquid = waterchunk->getLayers()->at(0); // only getting data from layer 0, maybe loop them ?
          int liquid_flags = liquid.getSubchunks();

          select_info << "<br><b>Liquid type</b>: " << liquid.liquidID() << " (\"" << gLiquidTypeDB.getLiquidName(liquid.liquidID()) << "\")"
              << "<br><b>liquid flags(center)</b>: "
              // getting flags from the center tile
              << ((attributes.fishable >> (4 * 8 + 4)) & 1 ? "fishable " : "")
              << ((attributes.fatigue >> (4 * 8 + 4)) & 1 ? "fatigue " : "")

              << (liquid.has_fatigue() ? "<br><b>entire chunk has fatigue!</b>" : "");
      }
  }
  else
  {
      select_info << "<br><b>no liquid data</b>";
  }

  select_info << "\n<br><b>textures used:</b> " << chunk->texture_set->num()
      << "<br><b>textures:</b><span>";

  unsigned counter = 0;
  for (auto& tex : *(chunk->texture_set->getTextures()))
  {
    bool stuck = !tex->finishedLoading();
    bool error = tex->finishedLoading() && !tex->is_uploaded();

    select_info << "<br> ";

    if (stuck)
      select_info << "<font color=\"Orange\">";

    if (error)
      select_info << "<font color=\"Red\">";

    select_info << "<b>" << (counter + 1) << ":</b> " << tex->file_key().stringRepr();

    if (stuck || error)
      select_info << "</font>";
    unsigned int effect_id = chunk->getTextureSet()->getEffectForLayer(counter);
    if (effect_id == 0xFFFFFFFF)
        effect_id = 0;
    select_info << "<br><b>Ground Effect</b>: " << effect_id;
        counter++;
  }

  //! \todo get a list of textures and their flags as well as detail doodads.

  select_info << "</span><br>";

  detail_widget->setText(select_info.str());
}

selection_group::selection_group(const std::vector<SceneObject*>& selected_objects, World* world)
    : _world(world)
{
    if (selected_objects.empty())
        return;

    // _is_selected = true;
    _members_uid.reserve(selected_objects.size());
    for (auto& selected_obj : selected_objects)
    {
        selected_obj->_grouped = true;
        _members_uid.push_back(selected_obj->uid);
    }
    recalcExtents();
    // can't save when initialiazing because it would save durign initial loading
    // save_json();
}

// only called when initializing world before objects are loaded, so can't set selected_obj->_grouped
selection_group::selection_group(const std::vector<unsigned int>& objects_uids, World* world)
    : _world(world)
{
    if (objects_uids.empty())
        return;

    // _is_selected = true;
    _members_uid = objects_uids;
    // std::unordered_set<unsigned int> _members_uid(objects_uids.begin(), objects_uids.end());

    recalcExtents();
    // save_json();
}

void selection_group::save_json()
{
    _world->saveSelectionGroups();
}

void selection_group::remove_member(unsigned int object_uid)
{
    if (_members_uid.size() == 1)
    {
        remove_group();
        save_json();
        return;
    }

    for (auto it = _members_uid.begin(); it != _members_uid.end(); ++it)
    {
        auto member_uid = *it;
        std::optional<selection_type> obj = _world->get_model(member_uid);
        if (!obj)
            continue;
        SceneObject* instance = std::get<SceneObject*>(obj.value());

        if (instance->uid == object_uid)
        {
            _members_uid.erase(it);
            instance->_grouped = false;
            save_json();
            return;
        }
    }
}

bool selection_group::contains_object(SceneObject* object)
{
    for (unsigned int member_uid : _members_uid)
    {
        if (object->uid == member_uid)
            return true;
    }

    return false;
}

void selection_group::select_group()
{
    for (unsigned int obj_uid : _members_uid)
    {
        std::optional<selection_type> obj = _world->get_model(obj_uid);
        if (!obj)
            continue;

        SceneObject* instance = std::get<SceneObject*>(obj.value());

        instance->_grouped = true; // ensure grouped attribute, some models could still be unloaded when creating the group

        _world->add_to_selection(obj.value(), true, false);
    }
    _world->update_selection_pivot();

    _is_selected = true;
}

void selection_group::unselect_group()
{
    for (unsigned int obj_uid : _members_uid)
    {
        // don't need to check if it's not selected
        _world->remove_from_selection(obj_uid, true, false);
    }
    _world->update_selection_pivot();
    _is_selected = false;
}

// only remove the group, not used to delete objects in it
void selection_group::remove_group(bool save)
{
    // remove grouped attribute
    for (unsigned int member_uid : _members_uid)
    {
        std::optional<selection_type> obj = _world->get_model(member_uid);
        if (!obj)
            continue;
        SceneObject* instance = std::get<SceneObject*>(obj.value());

        instance->_grouped = false;
    }

    for (auto it = _world->_selection_groups.begin(); it != _world->_selection_groups.end(); ++it)
    {
        auto it_group = *it;
        if (it_group.getMembers().size() == _members_uid.size() && it_group.getExtents() == _group_extents)
            // if (it_group.isSelected())
        {
            _world->_selection_groups.erase(it);
            // saveSelectionGroups();
            if (save)
                _world->saveSelectionGroups();
            return;
        }
    }
    assert(false);
    return; // if group wasn't found somehow, BAD
}

void selection_group::recalcExtents()
{
    bool first_obj = true;
    for (unsigned int obj_uid : _members_uid)
    {
        std::optional<selection_type> obj = _world->get_model(obj_uid);
        if (!obj)
            continue;

        SceneObject* instance = std::get<SceneObject*>(obj.value());
        if (first_obj)
        {
            _group_extents = instance->getExtents();
            first_obj = false;
            continue;
        }

        // min = glm::min(min, point);
        if (instance->getExtents()[0].x < _group_extents[0].x)
            _group_extents[0].x = instance->getExtents()[0].x;
        if (instance->getExtents()[0].y < _group_extents[0].y)
            _group_extents[0].y = instance->getExtents()[0].y;
        if (instance->getExtents()[0].z < _group_extents[0].z)
            _group_extents[0].z = instance->getExtents()[0].z;

        if (instance->getExtents()[1].x > _group_extents[1].x)
            _group_extents[1].x = instance->getExtents()[1].x;
        if (instance->getExtents()[1].y > _group_extents[1].y)
            _group_extents[1].y = instance->getExtents()[1].y;
        if (instance->getExtents()[1].z > _group_extents[1].z)
            _group_extents[1].z = instance->getExtents()[1].z;
    }
}

std::vector<unsigned int> const& selection_group::getMembers() const
{
  return _members_uid;
}

[[nodiscard]]
std::array<glm::vec3, 2> const& selection_group::getExtents() const
{
  return _group_extents;
}

bool selection_group::isSelected() const
{
  return _is_selected;
}

void selection_group::setUnselected()
{
  _is_selected = false;
}
