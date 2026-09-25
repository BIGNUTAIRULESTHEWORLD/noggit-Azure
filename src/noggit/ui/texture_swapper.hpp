// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/TileIndex.hpp>
#include <noggit/TextureManager.h>

#include <QtCore/QString>
#include <QtWidgets/QWidget>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

class World;
class MapView;

class QCheckBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

namespace Noggit
{
  namespace Ui
  {
    class current_texture;

    class texture_swapper : public QWidget
    {
    public:
      texture_swapper ( QWidget* parent
                      , const glm::vec3* camera_pos
                      , MapView* map_view
                      );
      ~texture_swapper() override;

      std::optional<scoped_blp_texture_reference> const& texture_to_swap() const;

      float radius() const;

      bool entireChunk() const;

      bool entireTile() const;

      void change_radius(float change);

      bool brush_mode() const;

      void toggle_brush_mode();

      void set_texture(std::string const& filename);

      current_texture* const texture_display();

      bool viewport_selection_active() const;
      bool viewport_chunk_selection_active() const;
      float viewport_chunk_selection_radius() const;
      void refresh_viewport_selection();
      bool toggle_viewport_adt(glm::vec3 const& cursor_pos);
      bool paint_viewport_chunks(glm::vec3 const& cursor_pos, bool deselect);
      void cancel_viewport_selection();

    private:
      using texture_replacement =
          std::pair<scoped_blp_texture_reference, scoped_blp_texture_reference>;

      struct selected_chunk_index
      {
        TileIndex tile_index{0, 0};
        unsigned x = 0;
        unsigned z = 0;

        friend bool operator<(selected_chunk_index const& lhs,
                              selected_chunk_index const& rhs)
        {
          return std::tie(lhs.tile_index, lhs.x, lhs.z)
               < std::tie(rhs.tile_index, rhs.x, rhs.z);
        }
      };

      enum class viewport_selection_mode
      {
        none,
        adts,
        chunks
      };

      void swap_current_adt(glm::vec3 const& camera_pos, MapView* map_view);
      void swap_selected_adts(std::vector<TileIndex> const& selected_tiles, MapView* map_view);
      void swap_selected_chunks(std::vector<selected_chunk_index> const& selected_chunks,
                                MapView* map_view);
      void add_batch_replacement();
      void remove_selected_batch_replacement();
      void clear_batch_replacements();
      void refresh_batch_replacement_ui();
      void begin_viewport_adt_selection();
      void begin_viewport_chunk_selection();
      void apply_viewport_selection();
      void update_viewport_selection_ui(QString const& feedback = {});
      void set_adt_overlay(TileIndex const& index, int value);
      void set_chunk_overlay(selected_chunk_index const& index, int value);

      std::optional<scoped_blp_texture_reference> _texture_to_swap;
      float _radius;

    private:
      current_texture* _texture_to_swap_display;

      QGroupBox* _brush_mode_group;
      QSlider* _radius_slider;
      QCheckBox* _swap_entire_chunk;
      QCheckBox* _swap_entire_tile;
      QDoubleSpinBox* _radius_spin;
      World* _world;
      MapView* _map_view;
      glm::vec3 const* _camera_pos;
      QPushButton* _select_adts_button;
      QPushButton* _select_chunks_button;
      QWidget* _adt_selection_controls;
      QLabel* _adt_selection_status;
      QPushButton* _apply_selected_adts_button;
      QListWidget* _batch_replacement_list;
      QLabel* _batch_replacement_status;
      QPushButton* _add_batch_replacement_button;
      QPushButton* _remove_batch_replacement_button;
      QPushButton* _clear_batch_replacements_button;
      std::vector<texture_replacement> _batch_replacements;
      viewport_selection_mode _viewport_selection_mode = viewport_selection_mode::none;
      std::set<TileIndex> _eligible_adts;
      std::set<TileIndex> _selected_adts;
      std::set<selected_chunk_index> _selected_chunks;
    };
  }
}
