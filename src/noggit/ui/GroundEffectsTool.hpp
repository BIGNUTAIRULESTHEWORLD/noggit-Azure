// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/DBCFile.h>
#include <noggit/TileIndex.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/vec3.hpp>
#include <QtWidgets/QWidget>

class World;
class MapView;
class MapTile;
class MapChunk;

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QRadioButton;
class QSpinBox;

namespace Noggit
{
  namespace Ui
  {
    namespace Tools
    {
      class PreviewRenderer;

      namespace UiCommon
      {
        class ExtendedSlider;
      }
    }

    class texturing_tool;

    struct ground_effect_doodad
    {
      unsigned int ID = 0;
      std::string filename = "";
      // Flag (useless in 3.3.5).

      bool empty() const;;

      bool operator== (ground_effect_doodad* doodad2);
    };

    struct ground_effect_set
    {
    public:
      void load_from_id(unsigned int effect_id);

      // "<id> - <doodad stems>" so the sets list reads as more than a number
      void rebuild_name();

      bool empty() const;;

      // only ignores id and name (use filename to compare doodads)
      bool operator== (ground_effect_set* effect2);

      // Created by the user or auto generated.
      std::string Name = "";

      unsigned int ID = 0;
      // TODO: can pack doodad and weight in a struct
      ground_effect_doodad Doodads[4];
      unsigned int Weights[4]{ 1, 1, 1, 1 };
      unsigned int Amount = 0;
      unsigned int TerrainType = 0;
    };

    enum class ground_effect_brush_mode
    {
      none,
      exclusion,
      effect,
      erase_effect,
      erase_exclusion
    };

    class GroundEffectsTool : public QWidget
    {
      Q_OBJECT

    public:
      GroundEffectsTool(texturing_tool* texturing_tool, MapView* map_view, QWidget* parent = nullptr);
      void updateTerrainUniformParams();
      // Delete renderer.
      ~GroundEffectsTool();
      float radius() const;
      ground_effect_brush_mode brush_mode() const;
      void set_brush_mode(ground_effect_brush_mode mode);
      bool render_mode() const;
      void delete_renderer();

    protected:
      void showEvent(QShowEvent* event) override;

      //Close event triggers, hide event.
      void hideEvent(QHideEvent* event) override;;

    public:
      void setDoodadSlotFromBrowser(QString doodad_path);
      // Selected texture was changed.
      void TextureChanged();

      bool render_active_sets_overlay() const;;

      bool render_placement_map_overlay() const;;

      bool render_exclusion_map_overlay() const;;

      void change_radius(float change);;

      void unload();

      std::optional<ground_effect_set> getSelectedGroundEffect();
      // Select the effect used by the dominant terrain layer under the cursor.
      // This intentionally leaves the current terrain texture unchanged so the
      // sampled effect can immediately be painted onto that target texture.
      std::optional<unsigned int> sampleEffectAt(glm::vec3 const& pos);
      // Recompute the effect-id overlay color of the chunks a brush stroke touched.
      void refreshOverlayForChunksInRange(glm::vec3 const& pos, float radius);

    signals:
      void activeEffectChanged(unsigned int id, QString const& name);
      void brushSettingsChanged();

    private:
      std::optional<glm::vec3> getSelectedEffectColor();
      void refreshChunkOverlayColor(MapTile* tile, MapChunk* chunk);
      void createNewSet();
      void duplicateSelectedSet();
      void deleteSelectedSet();
      // effective per-slot spawn share from the client's 16-slot weight table
      void updateWeightShares();
      void updateLivePreview();
      void saveSelectedSet();
      void applySelectedSet();
      void clearEffectsAtScope();
      // shared scope switch (zone/area/tile/global) behind Apply and Clear;
      // empty texture matches every layer
      void applyEffectIdAtScope(std::string const& texture, unsigned int effect_id, bool override_existing,
                                QString const& global_confirm,
                                std::optional<unsigned int> only_effect_id = std::nullopt);
      // sets saved from this tool are remembered per project so they stay
      // listed even when no scanned chunk references them yet
      void loadProjectSetRegistry();
      void saveProjectSetRegistry();
      void loadBlizzardGroundEffectIds();
      void updateBlizzardAssignmentsCombo();
      void selectBlizzardGroundEffect(int combo_index);
      void setActiveGroundEffect(ground_effect_set const& effect);
      void updateDoodadPreviewRender(int slot_index);
      void scanTileForEffects(TileIndex tile_index);
      // appends project-saved sets missing from the list so they stay listed
      // whether or not a scanned chunk uses them
      void mergeProjectSetsIntoLoaded();
      void updateSetsList();
      void genEffectColors();

      std::vector<ground_effect_set> _loaded_effects;
      // ids of sets saved from this tool, persisted per project
      std::vector<unsigned int> _project_set_ids;
      // Restored on startup so reopening the tool does not silently show a
      // different set and make saved values appear to have been reset.
      unsigned int _last_selected_set_id = 0;
      // Store them for faster iteration on duplicates.
      std::unordered_map<unsigned int, ground_effect_set> _ground_effect_cache;
      // Ordered Blizzard ground-effect candidates for each terrain texture.
      std::unordered_map<std::string, std::vector<unsigned int>> _blizzard_ground_effect_ids;
      std::vector<glm::vec3> _effects_colors;
      MapView* _map_view;
      texturing_tool* _texturing_tool;
      Tools::PreviewRenderer* _preview_renderer;
      QGroupBox* _render_group_box;
      QButtonGroup* _render_type_group;
      // Render all the loaded effect sets for this texture in various colors.
      QRadioButton* _render_active_sets;
      // Only for the active/selected set of the current texture: 
      // - Render as red if set is present in the chunk and NOT the current active layer.
      // - Render as green if set is present in the chunk and is the current active layer.
      // - Render as black is set is not present.
      QRadioButton* _render_placement_map;
      // Render chunk units where effect doodads are disabled as white, rest as black.
      QRadioButton* _render_exclusion_map;
      QCheckBox* _chkbox_merge_duplicates;
      QComboBox* _blizzard_assignments_combo;
      QListWidget* _effect_sets_list;
      // For render previews.
      QListWidget* _object_list;
      // Per-slot doodad weights, aligned under the doodad icons.
      QSpinBox* _weight_spinboxes[4] = {};
      QLabel* _weight_share_labels[4] = {};
      QSpinBox* _spinbox_doodads_amount;
      QComboBox* _cbbox_terrain_type;
      QCheckBox* _apply_override_cb;
      QCheckBox* _live_preview_cb = nullptr;
      QComboBox* _clear_target_combo;
      // zone/area scope: sweep every ADT on disk instead of only loaded tiles
      QCheckBox* _scope_disk_sweep_cb;
      // 0 = zone, 1 = area, 2 = tile, 3 = global
      QButtonGroup* _generate_type_group;
      QGroupBox* _brush_grup_box;
      QButtonGroup* _brush_type_group;
      QRadioButton* _paint_effect;
      QRadioButton* _paint_exclusion;
      QRadioButton* _erase_effect;
      QRadioButton* _erase_exclusion;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _effect_radius_slider;
    };
  }
}
