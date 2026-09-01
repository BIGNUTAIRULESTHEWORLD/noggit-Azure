// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/rendering/WorldRender.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/World.h>

#include <QtCore/QCoreApplication>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace
{
  constexpr std::size_t max_selected_adts = 5;

  std::size_t matching_chunk_count(MapTile* tile,
                                   scoped_blp_texture_reference const& texture_to_replace)
  {
    if (!tile || !tile->finishedLoading() || tile->loading_failed())
    {
      return 0;
    }

    std::size_t count = 0;
    for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
    {
      for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
      {
        MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);
        if (chunk && chunk->getTextureSet()->texture_id(texture_to_replace) >= 0)
        {
          ++count;
        }
      }
    }
    return count;
  }

  std::size_t matching_chunk_count(
      MapTile* tile,
      std::vector<std::pair<scoped_blp_texture_reference,
                            scoped_blp_texture_reference>> const& replacements)
  {
    if (!tile || !tile->finishedLoading() || tile->loading_failed())
    {
      return 0;
    }

    std::size_t count = 0;
    for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
    {
      for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
      {
        MapChunk* chunk = tile->getChunk(chunk_x, chunk_z);
        if (!chunk)
        {
          continue;
        }

        bool const has_source = std::any_of(
            replacements.begin(), replacements.end(), [chunk](auto const& replacement)
            {
              return replacement.first != replacement.second
                  && chunk->getTextureSet()->texture_id(replacement.first) >= 0;
            });
        if (has_source)
        {
          ++count;
        }
      }
    }
    return count;
  }

  QString texture_label(scoped_blp_texture_reference const& texture)
  {
    QString path = QString::fromStdString(texture->file_key().filepath());
    path.replace('\\', '/');
    return path.section('/', -1);
  }

}

namespace Noggit
{
  namespace Ui
  {
    texture_swapper::texture_swapper ( QWidget* parent
                                     , const glm::vec3* camera_pos
                                     , MapView* map_view
                                     )
      : QWidget (parent)
      , _texture_to_swap()
      , _radius(15.f)
      , _world(map_view->getWorld())
      , _map_view(map_view)
      , _camera_pos(camera_pos)
    {
      setWindowTitle ("Swap");
      setWindowFlags (Qt::Tool | Qt::WindowStaysOnTopHint);

      auto layout (new QFormLayout (this));

      _texture_to_swap_display = new current_texture(true, this);

      QPushButton* select = new QPushButton("Set source from selected texture", this);
      QPushButton* swap_adt = new QPushButton("Replace on current ADT", this);
      _add_batch_replacement_button = new QPushButton(
          tr("Add source -> selected replacement"), this);
      _select_adts_button = new QPushButton("Select nearby ADTs in viewport...", this);
      QPushButton* remove_text_adt = new QPushButton(tr("Remove this texture from ADT"), this);

      select->setToolTip(tr("Capture the currently selected texture as the texture to replace."));
      swap_adt->setToolTip(tr("Replace the captured source with the currently selected texture on this ADT."));
      _add_batch_replacement_button->setToolTip(
          tr("Add the captured source and currently selected replacement to the reusable batch list."));
      _select_adts_button->setToolTip(tr("Highlight rendered ADTs in the viewport and select up to five for one undoable replacement."));

      layout->addRow(new QLabel("Texture to swap"));
      layout->addRow(_texture_to_swap_display);
      layout->addRow(select);
      layout->addRow(swap_adt);
      layout->addRow(new QLabel(tr("Reusable batch replacements"), this));
      layout->addRow(_add_batch_replacement_button);

      _batch_replacement_list = new QListWidget(this);
      _batch_replacement_list->setMinimumHeight(96);
      _batch_replacement_list->setSelectionMode(QAbstractItemView::SingleSelection);
      layout->addRow(_batch_replacement_list);

      auto* batch_list_controls = new QWidget(this);
      auto* batch_list_controls_layout = new QHBoxLayout(batch_list_controls);
      batch_list_controls_layout->setContentsMargins(0, 0, 0, 0);
      _remove_batch_replacement_button = new QPushButton(tr("Remove selected"), batch_list_controls);
      _clear_batch_replacements_button = new QPushButton(tr("Clear list"), batch_list_controls);
      batch_list_controls_layout->addWidget(_remove_batch_replacement_button);
      batch_list_controls_layout->addWidget(_clear_batch_replacements_button);
      layout->addRow(batch_list_controls);

      _batch_replacement_status = new QLabel(this);
      _batch_replacement_status->setWordWrap(true);
      layout->addRow(_batch_replacement_status);
      layout->addRow(_select_adts_button);

      _adt_selection_status = new QLabel(this);
      _adt_selection_status->setWordWrap(true);
      _adt_selection_status->setStyleSheet(QStringLiteral("QLabel { padding: 4px; }"));
      _adt_selection_status->hide();
      layout->addRow(_adt_selection_status);

      _adt_selection_controls = new QWidget(this);
      auto* adt_selection_layout = new QHBoxLayout(_adt_selection_controls);
      adt_selection_layout->setContentsMargins(0, 0, 0, 0);
      _apply_selected_adts_button = new QPushButton(tr("Apply replacement"), _adt_selection_controls);
      auto* cancel_adt_selection = new QPushButton(tr("Cancel"), _adt_selection_controls);
      adt_selection_layout->addWidget(_apply_selected_adts_button, 1);
      adt_selection_layout->addWidget(cancel_adt_selection);
      _adt_selection_controls->hide();
      layout->addRow(_adt_selection_controls);
      layout->addRow(remove_text_adt);

      auto brush_widget (new QWidget(this));
      auto brush_layout (new QFormLayout(brush_widget));

      _brush_mode_group = new QGroupBox("Brush mode", brush_widget);
      // _brush_mode_group->setAlignment(Qt::AlignLeft);
      _brush_mode_group->setCheckable(true);
      _brush_mode_group->setChecked(false);

      layout->addRow(_brush_mode_group);
      _brush_mode_group->setLayout(brush_layout);


      _swap_entire_chunk = new QCheckBox(_brush_mode_group);
      _swap_entire_chunk->setText(tr("Entire chunk"));
      _swap_entire_chunk->setCheckState(Qt::CheckState::Unchecked);
      brush_layout->addRow(_swap_entire_chunk);

      _swap_entire_tile = new QCheckBox(_brush_mode_group);
      _swap_entire_tile->setText(tr("Entire tile"));
      _swap_entire_tile->setCheckState(Qt::CheckState::Unchecked);
      brush_layout->addRow(_swap_entire_tile);

      _radius_spin = new QDoubleSpinBox(_brush_mode_group);
      _radius_spin->setRange (0.f, 100.f);
      _radius_spin->setDecimals (2);
      _radius_spin->setValue (_radius);
      brush_layout->addRow ("Radius:", _radius_spin);

      _radius_slider = new QSlider (Qt::Orientation::Horizontal, _brush_mode_group);
      _radius_slider->setRange (0, 100);
      _radius_slider->setSliderPosition (_radius);
      brush_layout->addRow (_radius_slider);      
      
      connect(select, &QPushButton::clicked, [&, map_view]() {

        map_view->context()->makeCurrent(map_view->context()->surface());
        OpenGL::context::scoped_setter const _ (::gl, map_view->context());
        _texture_to_swap = selected_texture::get();
        if (_texture_to_swap)
        {
          _texture_to_swap_display->set_texture(_texture_to_swap.value()->file_key().filepath());
        }
      });

      connect(swap_adt, &QPushButton::clicked, [this, camera_pos, map_view]() {
        swap_current_adt(*camera_pos, map_view);
      });

      connect(_add_batch_replacement_button, &QPushButton::clicked, this,
              &texture_swapper::add_batch_replacement);
      connect(_remove_batch_replacement_button, &QPushButton::clicked, this,
              &texture_swapper::remove_selected_batch_replacement);
      connect(_clear_batch_replacements_button, &QPushButton::clicked, this,
              &texture_swapper::clear_batch_replacements);
      connect(_batch_replacement_list, &QListWidget::itemSelectionChanged, this, [this]
              {
                _remove_batch_replacement_button->setEnabled(
                    _batch_replacement_list->currentRow() >= 0);
              });

      connect(_select_adts_button, &QPushButton::clicked, this,
              &texture_swapper::begin_viewport_adt_selection);
      connect(_apply_selected_adts_button, &QPushButton::clicked, this,
              &texture_swapper::apply_viewport_adt_selection);
      connect(cancel_adt_selection, &QPushButton::clicked, this,
              &texture_swapper::cancel_viewport_adt_selection);

      connect(remove_text_adt, &QPushButton::clicked, [this, camera_pos, map_view]() {
          if (_texture_to_swap)
          {
              ActionManager::instance()->beginAction(map_view, ActionFlags::eCHUNKS_TEXTURE);
              _world->removeTexture(*camera_pos, _texture_to_swap.value());
              ActionManager::instance()->endAction();
          }
          });

      connect ( _radius_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&](double v)
                {
                  QSignalBlocker const blocker (_radius_slider);
                  _radius = v;
                  _radius_slider->setSliderPosition ((int)std::round (v));

                }
              );

      connect ( _radius_slider, &QSlider::valueChanged
              , [&](int v)
                {
                  QSignalBlocker const blocker (_radius_spin);
                  _radius = v;
                  _radius_spin->setValue(v);
                }
              );

      refresh_batch_replacement_ui();
    }

    std::optional<scoped_blp_texture_reference> const& texture_swapper::texture_to_swap() const
    {
      return _texture_to_swap;
    }

    float texture_swapper::radius() const
    {
      return _radius;
    }

    bool texture_swapper::entireChunk() const
    {
      return _swap_entire_chunk->isChecked();
    }

    bool texture_swapper::entireTile() const
    {
      return _swap_entire_tile->isChecked();
    }

    void texture_swapper::change_radius(float change)
    {
      _radius_spin->setValue(_radius + change);
    }

    bool texture_swapper::brush_mode() const
    {
      return _brush_mode_group->isChecked();
    }

    void texture_swapper::toggle_brush_mode()
    {
      _brush_mode_group->setChecked(!_brush_mode_group->isChecked());
    }

    void texture_swapper::set_texture(std::string const& filename)
    {
      _texture_to_swap = std::move(scoped_blp_texture_reference(filename, _world->getRenderContext()));
    }

    texture_swapper::~texture_swapper()
    {
      cancel_viewport_adt_selection();
    }

    void texture_swapper::add_batch_replacement()
    {
      auto const replacement_texture = selected_texture::get();
      if (!_texture_to_swap || !replacement_texture)
      {
        QMessageBox::information(this, tr("Batch replacement"),
                                 tr("Capture a source texture and select its replacement first."));
        return;
      }
      if (*_texture_to_swap == *replacement_texture)
      {
        QMessageBox::information(this, tr("Batch replacement"),
                                 tr("The source and replacement textures are the same."));
        return;
      }

      auto const existing = std::find_if(
          _batch_replacements.begin(), _batch_replacements.end(), [this](auto const& mapping)
          {
            return mapping.first == *_texture_to_swap;
          });
      if (existing != _batch_replacements.end())
      {
        _batch_replacements.erase(existing);
      }
      _batch_replacements.emplace_back(*_texture_to_swap, *replacement_texture);
      refresh_batch_replacement_ui();
    }

    void texture_swapper::remove_selected_batch_replacement()
    {
      int const row = _batch_replacement_list->currentRow();
      if (row < 0 || static_cast<std::size_t>(row) >= _batch_replacements.size())
      {
        return;
      }

      _batch_replacements.erase(_batch_replacements.begin() + row);
      refresh_batch_replacement_ui();
    }

    void texture_swapper::clear_batch_replacements()
    {
      _batch_replacements.clear();
      refresh_batch_replacement_ui();
    }

    void texture_swapper::refresh_batch_replacement_ui()
    {
      int const selected_row = _batch_replacement_list->currentRow();
      _batch_replacement_list->clear();

      for (auto const& mapping : _batch_replacements)
      {
        QString const source_path = QString::fromStdString(mapping.first->file_key().filepath());
        QString const replacement_path = QString::fromStdString(mapping.second->file_key().filepath());
        auto* item = new QListWidgetItem(
            tr("%1  ->  %2").arg(texture_label(mapping.first), texture_label(mapping.second)),
            _batch_replacement_list);
        item->setToolTip(tr("%1\n->\n%2").arg(source_path, replacement_path));
      }

      if (!_batch_replacements.empty() && selected_row >= 0)
      {
        _batch_replacement_list->setCurrentRow(
            std::min(selected_row, static_cast<int>(_batch_replacements.size()) - 1));
      }

      _batch_replacement_status->setText(
          tr("%1 mapping%2 saved for repeated five-ADT batches. The list remains after Apply.")
              .arg(_batch_replacements.size())
              .arg(_batch_replacements.size() == 1 ? QString() : QStringLiteral("s")));
      _remove_batch_replacement_button->setEnabled(_batch_replacement_list->currentRow() >= 0);
      _clear_batch_replacements_button->setEnabled(!_batch_replacements.empty());
      _select_adts_button->setEnabled(!_batch_replacements.empty());
      if (_viewport_adt_selection_active)
      {
        update_viewport_adt_selection_ui();
      }
    }

    bool texture_swapper::viewport_adt_selection_active() const
    {
      return _viewport_adt_selection_active;
    }

    void texture_swapper::set_adt_overlay(TileIndex const& index, int value)
    {
      MapTile* tile = _world->mapIndex.getTile(index);
      if (!tile || !tile->finishedLoading() || tile->loading_failed())
      {
        return;
      }

      for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
      {
        for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
        {
          if (MapChunk* chunk = tile->getChunk(chunk_x, chunk_z))
          {
            chunk->setChunkMoverOverlay(value);
          }
        }
      }
    }

    void texture_swapper::update_viewport_adt_selection_ui(QString const& feedback)
    {
      QString status = tr("Blue ADTs are in render range and selectable. Green ADTs are selected. "
                          "Click terrain to toggle an ADT.\n\n%1 / %2 ADTs selected.")
                           .arg(_selected_adts.size())
                           .arg(max_selected_adts);
      if (!feedback.isEmpty())
      {
        status += QStringLiteral("\n") + feedback;
      }
      _adt_selection_status->setText(status);
      _apply_selected_adts_button->setEnabled(
          !_selected_adts.empty() && !_batch_replacements.empty());
    }

    void texture_swapper::refresh_viewport_adt_selection()
    {
      if (!_viewport_adt_selection_active)
      {
        return;
      }

      std::set<TileIndex> rendered_adts;
      for (MapTile* tile : _world->mapIndex.loaded_tiles())
      {
        if (tile && tile->finishedLoading() && !tile->loading_failed()
            && tile->_was_rendered_last_frame)
        {
          rendered_adts.insert(tile->index);
        }
      }

      if (rendered_adts == _eligible_adts)
      {
        return;
      }

      for (TileIndex const& previous : _eligible_adts)
      {
        if (!rendered_adts.contains(previous) && !_selected_adts.contains(previous))
        {
          set_adt_overlay(previous, 0);
        }
      }

      for (TileIndex const& current : rendered_adts)
      {
        set_adt_overlay(current, _selected_adts.contains(current) ? 2 : 3);
      }
      for (TileIndex const& selected : _selected_adts)
      {
        set_adt_overlay(selected, 2);
      }

      _eligible_adts = std::move(rendered_adts);
      update_viewport_adt_selection_ui();
      _map_view->invalidate();
    }

    void texture_swapper::begin_viewport_adt_selection()
    {
      if (_batch_replacements.empty())
      {
        QMessageBox::information(this, tr("Texture replacement"),
                                 tr("Add at least one source-to-replacement mapping to the batch list first."));
        return;
      }
      if (ActionManager::instance()->getCurrentAction())
      {
        QMessageBox::warning(this, tr("Texture replacement"),
                             tr("Finish the current editing action before selecting ADTs."));
        return;
      }

      _viewport_adt_selection_active = true;
      _eligible_adts.clear();
      _selected_adts.clear();
      _select_adts_button->hide();
      _adt_selection_status->show();
      _adt_selection_controls->show();

      auto* terrain_params = _world->renderer()->getTerrainParamsUniformBlock();
      terrain_params->draw_selection_overlay = true;
      _world->renderer()->markTerrainParamsUniformBlockDirty();

      refresh_viewport_adt_selection();

      TileIndex const current_adt(*_camera_pos);
      if (_eligible_adts.contains(current_adt))
      {
        _selected_adts.insert(current_adt);
        set_adt_overlay(current_adt, 2);
      }
      update_viewport_adt_selection_ui();
      _map_view->invalidate();
    }

    bool texture_swapper::toggle_viewport_adt(glm::vec3 const& cursor_pos)
    {
      if (!_viewport_adt_selection_active)
      {
        return false;
      }

      TileIndex const clicked_adt(cursor_pos);
      auto const selected = _selected_adts.find(clicked_adt);
      if (selected != _selected_adts.end())
      {
        _selected_adts.erase(selected);
        set_adt_overlay(clicked_adt, _eligible_adts.contains(clicked_adt) ? 3 : 0);
        update_viewport_adt_selection_ui();
        _map_view->invalidate();
        return true;
      }

      if (!_eligible_adts.contains(clicked_adt))
      {
        update_viewport_adt_selection_ui(tr("That ADT is outside the current render range."));
        return false;
      }
      if (_selected_adts.size() >= max_selected_adts)
      {
        update_viewport_adt_selection_ui(tr("The five-ADT selection limit has been reached."));
        return false;
      }

      _selected_adts.insert(clicked_adt);
      set_adt_overlay(clicked_adt, 2);
      update_viewport_adt_selection_ui();
      _map_view->invalidate();
      return true;
    }

    void texture_swapper::cancel_viewport_adt_selection()
    {
      if (!_viewport_adt_selection_active)
      {
        return;
      }

      for (TileIndex const& eligible : _eligible_adts)
      {
        set_adt_overlay(eligible, 0);
      }
      for (TileIndex const& selected : _selected_adts)
      {
        set_adt_overlay(selected, 0);
      }

      _eligible_adts.clear();
      _selected_adts.clear();
      _viewport_adt_selection_active = false;
      _select_adts_button->show();
      _adt_selection_status->hide();
      _adt_selection_controls->hide();

      auto* terrain_params = _world->renderer()->getTerrainParamsUniformBlock();
      terrain_params->draw_selection_overlay = false;
      _world->renderer()->markTerrainParamsUniformBlockDirty();
      _map_view->invalidate();
    }

    void texture_swapper::apply_viewport_adt_selection()
    {
      if (!_viewport_adt_selection_active || _selected_adts.empty())
      {
        return;
      }

      std::vector<TileIndex> selected_tiles(_selected_adts.begin(), _selected_adts.end());
      cancel_viewport_adt_selection();
      swap_selected_adts(selected_tiles, _map_view);
    }

    void texture_swapper::swap_current_adt(glm::vec3 const& camera_pos, MapView* map_view)
    {
      auto const replacement_texture = selected_texture::get();
      if (!_texture_to_swap || !replacement_texture)
      {
        QMessageBox::information(this, tr("Texture replacement"),
                                 tr("Capture a source texture and select a replacement texture first."));
        return;
      }
      if (*_texture_to_swap == *replacement_texture)
      {
        QMessageBox::information(this, tr("Texture replacement"),
                                 tr("The source and replacement textures are the same."));
        return;
      }
      if (ActionManager::instance()->getCurrentAction())
      {
        QMessageBox::warning(this, tr("Texture replacement"),
                             tr("Finish the current editing action before replacing an ADT texture."));
        return;
      }

      MapTile* tile = _world->mapIndex.getTile(camera_pos);
      std::size_t const affected_chunks = matching_chunk_count(tile, *_texture_to_swap);
      if (!affected_chunks)
      {
        QMessageBox::information(this, tr("Texture replacement"),
                                 tr("The source texture was not found on the current ADT."));
        return;
      }

      ActionManager::instance()->beginAction(map_view, ActionFlags::eCHUNKS_TEXTURE);
      std::size_t const changed_chunks =
          _world->swapTextureOnTile(tile, *_texture_to_swap, *replacement_texture);
      ActionManager::instance()->endAction();

      QMessageBox::information(this, tr("Texture replacement"),
                               tr("Replaced the texture on %1 chunk%2. Save Changed when ready.")
                                   .arg(changed_chunks)
                                   .arg(changed_chunks == 1 ? QString() : QStringLiteral("s")));
    }

    void texture_swapper::swap_selected_adts(std::vector<TileIndex> const& selected_tiles,
                                             MapView* map_view)
    {
      if (_batch_replacements.empty())
      {
        QMessageBox::information(this, tr("Texture replacement"),
                                 tr("The reusable batch replacement list is empty."));
        return;
      }
      if (ActionManager::instance()->getCurrentAction())
      {
        QMessageBox::warning(this, tr("Texture replacement"),
                             tr("Finish the current editing action before replacing ADT textures."));
        return;
      }

      if (selected_tiles.empty())
      {
        return;
      }

      QProgressDialog progress(tr("Loading selected ADTs and scanning all texture mappings..."),
                               tr("Cancel"), 0, static_cast<int>(selected_tiles.size()), map_view);
      progress.setWindowTitle(tr("Texture replacement preflight"));
      progress.setWindowModality(Qt::ApplicationModal);
      progress.setMinimumDuration(0);

      struct affected_tile
      {
        MapTile* tile;
      };
      std::vector<affected_tile> affected_tiles;
      affected_tiles.reserve(selected_tiles.size());
      std::size_t affected_chunks = 0;
      std::size_t failed_tiles = 0;

      for (std::size_t i = 0; i < selected_tiles.size(); ++i)
      {
        if (progress.wasCanceled())
        {
          return;
        }

        TileIndex const& tile_index = selected_tiles.at(i);
        progress.setLabelText(tr("Loading ADT %1_%2 (%3 of %4)...")
                                  .arg(tile_index.x)
                                  .arg(tile_index.z)
                                  .arg(i + 1)
                                  .arg(selected_tiles.size()));
        QCoreApplication::processEvents();

        // Full model loading is intentional. MapTile's serializer rebuilds
        // MDDF/MODF from live instances, so a model-free tile must never be
        // left changed for the normal Save Changed workflow.
        MapTile* tile = _world->mapIndex.loadTile(tile_index);
        if (!tile)
        {
          ++failed_tiles;
        }
        else
        {
          tile->wait_until_loaded();
          if (tile->loading_failed())
          {
            ++failed_tiles;
          }
          else
          {
            std::size_t const chunks = matching_chunk_count(tile, _batch_replacements);
            if (chunks)
            {
              affected_tiles.push_back({tile});
              affected_chunks += chunks;
            }
          }
        }

        progress.setValue(static_cast<int>(i + 1));
        QCoreApplication::processEvents();
      }

      if (progress.wasCanceled())
      {
        return;
      }
      progress.close();

      if (affected_tiles.empty())
      {
        QString message = tr("None of the mapped source textures were found on the selected ADTs.");
        if (failed_tiles)
        {
          message += tr("\n\n%1 ADT%2 could not be loaded.")
                         .arg(failed_tiles)
                         .arg(failed_tiles == 1 ? QString() : QStringLiteral("s"));
        }
        QMessageBox::information(this, tr("Texture replacement"), message);
        return;
      }

      QString confirmation =
          tr("Apply %1 texture mapping%2 to %3 chunk%4 across %5 ADT%6?\n\n"
             "This creates one undo step. The ADTs will remain changed but will not be saved automatically.")
              .arg(_batch_replacements.size())
              .arg(_batch_replacements.size() == 1 ? QString() : QStringLiteral("s"))
              .arg(affected_chunks)
              .arg(affected_chunks == 1 ? QString() : QStringLiteral("s"))
              .arg(affected_tiles.size())
              .arg(affected_tiles.size() == 1 ? QString() : QStringLiteral("s"));
      if (failed_tiles)
      {
        confirmation += tr("\n\n%1 selected ADT%2 could not be loaded and will be skipped.")
                            .arg(failed_tiles)
                            .arg(failed_tiles == 1 ? QString() : QStringLiteral("s"));
      }
      if (QMessageBox::question(this, tr("Replace textures on selected ADTs"), confirmation,
                                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
      {
        return;
      }

      ActionManager::instance()->beginAction(map_view, ActionFlags::eCHUNKS_TEXTURE);
      std::size_t changed_chunks = 0;
      for (affected_tile const& affected : affected_tiles)
      {
        changed_chunks += _world->swapTexturesOnTile(affected.tile, _batch_replacements);
      }
      ActionManager::instance()->endAction();

      QMessageBox::information(this, tr("Texture replacement"),
                               tr("Applied %1 texture mapping%2 to %3 chunk%4 across %5 ADT%6. "
                                  "The mapping list is still available for the next batch. "
                                  "Use Save Changed when ready.")
                                   .arg(_batch_replacements.size())
                                   .arg(_batch_replacements.size() == 1 ? QString() : QStringLiteral("s"))
                                   .arg(changed_chunks)
                                   .arg(changed_chunks == 1 ? QString() : QStringLiteral("s"))
                                   .arg(affected_tiles.size())
                                   .arg(affected_tiles.size() == 1 ? QString() : QStringLiteral("s")));
    }

    current_texture* const texture_swapper::texture_display()
    {
      return _texture_to_swap_display;
    }
  }
}
