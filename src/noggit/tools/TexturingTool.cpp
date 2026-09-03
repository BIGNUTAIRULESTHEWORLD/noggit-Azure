// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "FlattenBlurTool.hpp"

#include "TexturingTool.hpp"
#include <noggit/ActionManager.hpp>
#include <noggit/DetailDoodads.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/MapChunk.h>
#include <noggit/Selection.h>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/GroundEffectsTool.hpp>
#include <noggit/ui/texture_palette_small.hpp>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/TexturePicker.h>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/ui/tools/UiCommon/ImageMaskSelector.hpp>
#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/World.h>

#include <QDockWidget>
#include <QMenu>
#include <QSettings>
#include <QStatusBar>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <utility>

#include <glm/geometric.hpp>

namespace Noggit
{
    TexturingTool::TexturingTool(MapView* mapView)
        : Tool{ mapView }
    {
        addHotkey("toggleTool"_hash, Hotkey{
            .onPress = [=] { _texturingTool->toggle_tool(); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("setBrushLevelMinMax"_hash, Hotkey{
            .onPress = [=] { _texturingTool->toggle_brush_level_min_max(); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("increaseRadius"_hash, Hotkey{
            .onPress = [=] { _texturingTool->change_radius(0.1f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("decreaseRadius"_hash, Hotkey{
            .onPress = [=] { _texturingTool->change_radius(-0.1f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("setBrushLevel0Pct"_hash, Hotkey{
            .onPress = [=] { _texturingTool->set_brush_level(0.0f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("setBrushLevel25Pct"_hash, Hotkey{
            .onPress = [=] { _texturingTool->set_brush_level(255.0f * 0.25f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("setBrushLevel50Pct"_hash, Hotkey{
            .onPress = [=] { _texturingTool->set_brush_level(255.0f * 0.5f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("setBrushLevel75Pct"_hash, Hotkey{
            .onPress = [=] { _texturingTool->set_brush_level(255.0f * 0.75f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("setBrushLevel100Pct"_hash, Hotkey{
            .onPress = [=] { _texturingTool->set_brush_level(255.0f); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("toggleTexturePalette"_hash, Hotkey{
            .onPress = [=] { _show_texture_palette_window.toggle(); },
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::paint && !NOGGIT_CUR_ACTION; },
            });

        addHotkey("toggleTextureBrowser"_hash, Hotkey{
            .onPress = [=] { _show_texture_browser_window.toggle(); },
            .condition = [=]
            {
                return mapView->get_editing_mode() == editing_mode::paint
                  && (!_texturingTool || !_texturingTool->roadModeEnabled()) && !NOGGIT_CUR_ACTION;
            },
            });

        addHotkey("roadCancel"_hash, Hotkey{
            .onPress = [this] { cancelRoadSession(); },
            .condition = [this, mapView]
            {
                return _texturingTool && mapView->get_editing_mode() == editing_mode::paint
                  && _texturingTool->roadModeEnabled()
                  && (_road_session != road_session_state::idle || _road_style.has_value()
                      || !_road_reference_centerline.empty())
                  && !NOGGIT_CUR_ACTION;
            },
            });

        QObject::connect(mapView
            , &MapView::selectionUpdated
            , [=](std::vector<selection_type>& selection)
            {
              if (mapView->isUiHidden() || _texturingTool->isHidden() || !_texturePickerNeedUpdate)
              {
                return;
              }

              _texturePickerDock->setVisible(true);
              _texturePicker->setMainTexture(_texturingTool->_current_texture);
              _texturePicker->getTextures(*selection.begin());

              _texturePickerNeedUpdate = false;
            }
        );
    }

    TexturingTool::~TexturingTool()
    {
        unload();
        delete _texturePickerDock;
        delete _texturePaletteDock;
        delete _textureBrowserDock;
        delete _texturingTool;
    }

    void TexturingTool::unload()
    {
        if (_texturingTool)
        {
            _texturingTool->texture_swap_tool()->cancel_viewport_adt_selection();
            _texturingTool->unload();
        }

        Tool::unload();
    }

    char const* TexturingTool::name() const
    {
        return "Texture Painter";
    }

    editing_mode TexturingTool::editingMode() const
    {
        return editing_mode::paint;
    }

    Ui::FontNoggit::Icons TexturingTool::icon() const
    {
        return Ui::FontNoggit::TOOL_TEXTURE_PAINT;
    }

    void TexturingTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        auto mv = mapView();
        /* Tool */
        _texturingTool = new Ui::texturing_tool(&mv->getCamera()->position, mv, &_show_texture_palette_window, mv);
        toolPanel->registerTool(this, _texturingTool);

        // Connects
        QObject::connect(_texturingTool->texture_swap_tool()->texture_display()
            , &Noggit::Ui::current_texture::texture_dropped
            , [=](std::string const& filename)
            {
                mv->makeCurrent();
                OpenGL::context::scoped_setter const _(::gl, mv->context());

                _texturingTool->texture_swap_tool()->set_texture(filename);
            }
        );

        QObject::connect(_texturingTool->_current_texture
            , &Noggit::Ui::current_texture::texture_dropped
            , [=](std::string const& filename)
            {
                mv->makeCurrent();
                OpenGL::context::scoped_setter const _(::gl, mv->context());

                Noggit::Ui::selected_texture::set({ filename, mv->getRenderContext() });
            }
        );

        QObject::connect(_texturingTool->_current_texture, &Noggit::Ui::current_texture::clicked
            , [=]
            {
                _show_texture_browser_window.set(!_show_texture_browser_window.get());
            }
        );

        QObject::connect(_texturingTool, &Ui::texturing_tool::texturePaletteToggled,
            [=]()
            {
                _show_texture_palette_window.set(!_show_texture_palette_window.get());
            });

        QObject::connect(_texturingTool, &Ui::texturing_tool::roadCommitRequested,
            [this]() { commitRoadPreview(); });
        QObject::connect(_texturingTool, &Ui::texturing_tool::roadClearRequested,
            [this]() { clearRoadPreview(false); });
        QObject::connect(_texturingTool, &Ui::texturing_tool::roadReferenceSelectionRequested,
            [this]() { beginRoadReferenceSelection(); });
        QObject::connect(_texturingTool, &Ui::texturing_tool::roadReferenceAccepted,
            [this]() { acceptRoadReferenceSelection(); });
        QObject::connect(_texturingTool, &Ui::texturing_tool::roadCancelRequested,
            [this]() { cancelRoadSession(); });

        setupTextureBrowser(mv);
        setupTexturePalette(mv);
        setupTexturePicker(mv);
    }

    void TexturingTool::setupTextureBrowser(MapView* mv)
    {
      // Dock
      _textureBrowserDock = new QDockWidget("Texture Browser", mv);
      _textureBrowserDock->setObjectName("mapViewTextureBrowserDock");
      _textureBrowserDock->setFeatures(QDockWidget::DockWidgetMovable
        | QDockWidget::DockWidgetFloatable
        | QDockWidget::DockWidgetClosable);
      _textureBrowserDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea
        | Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
      mv->mainWindow()->addDockWidget(Qt::BottomDockWidgetArea, _textureBrowserDock);
      _textureBrowserDock->hide();

      QObject::connect(_textureBrowserDock, &QDockWidget::visibilityChanged,
        [=](bool visible)
        {
          if (mv->isUiHidden())
            return;

          mv->settings()->setValue("map_view/texture_browser", visible);
          mv->settings()->sync();
        });

      QObject::connect(mv, &QObject::destroyed, _textureBrowserDock, &QObject::deleteLater);
      // End Dock

      _textureBrowser = new Noggit::Ui::tileset_chooser(mv);
      _textureBrowserDock->setWidget(_textureBrowser);
      QObject::connect(mv, &QObject::destroyed, _textureBrowser, &QObject::deleteLater);

      QObject::connect(_textureBrowser, &Noggit::Ui::tileset_chooser::selected
        , [=](std::string const& filename)
        {
          mv->makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, mv->context());

          Noggit::Ui::selected_texture::set({ filename, mv->getRenderContext() });
          _texturingTool->_current_texture->set_texture(filename);
          _texturePicker->setMainTexture(_texturingTool->_current_texture);
          _texturePicker->updateSelection();
        }
      );

      QObject::connect(&_show_texture_browser_window, &Noggit::BoolToggleProperty::changed
        , [this, mv]
        {
          if (!(mv->get_editing_mode() == editing_mode::paint || mv->get_editing_mode() == editing_mode::stamp)
            || mv->isUiHidden())
          {
            QSignalBlocker const _(_show_texture_browser_window);
            _show_texture_browser_window.set(false);
            return;
          }

          QSignalBlocker const _(_textureBrowser);
          _textureBrowserDock->setVisible(_show_texture_browser_window.get());
        }
      );
    }

    void TexturingTool::setupTexturePalette(MapView* mv)
    {
      _texturePalette = new Noggit::Ui::texture_palette_small(mv->project(), mv->getWorld()->getMapID(), mv);

      // Dock
      _texturePaletteDock = new QDockWidget("Texture Palette", mv);
      _texturePaletteDock->setObjectName("mapViewTexturePaletteDock");
      _texturePaletteDock->setFeatures(QDockWidget::DockWidgetMovable
        | QDockWidget::DockWidgetFloatable
        | QDockWidget::DockWidgetClosable
      );

      _texturePaletteDock->setWidget(_texturePalette);
      _texturePaletteDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea
        | Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
      _texturePaletteDock->hide();

      QObject::connect(mv, &QObject::destroyed, _texturePaletteDock, &QObject::deleteLater);

      mv->mainWindow()->addDockWidget(Qt::BottomDockWidgetArea, _texturePaletteDock);
      // End Dock

      QObject::connect(_texturePaletteDock, &QDockWidget::visibilityChanged,
        [=](bool visible)
        {
          if (mv->isUiHidden())
            return;

          mv->settings()->setValue("map_view/texture_palette", visible);
          mv->settings()->sync();
        });

      QObject::connect(_texturePalette, &Noggit::Ui::texture_palette_small::selected
        , [=](std::string const& filename)
        {
          mv->makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, mv->context());

          Noggit::Ui::selected_texture::set({ filename, mv->getRenderContext() });
          _texturingTool->_current_texture->set_texture(filename);
        }
      );
      QObject::connect(mv, &QObject::destroyed, _texturePalette, &QObject::deleteLater);

      QObject::connect(&_show_texture_palette_window, &Noggit::BoolToggleProperty::changed
        , _texturePaletteDock, [=]
        {
          if (mv->get_editing_mode() != editing_mode::paint || mv->isUiHidden())
          {
            QSignalBlocker const _(_show_texture_palette_window);
            _show_texture_palette_window.set(false);
            return;
          }

          QSignalBlocker const _(_texturePalette);
          _texturePaletteDock->setVisible(_show_texture_palette_window.get());
        }
      );

      QObject::connect(_texturingTool->_current_texture, &Noggit::Ui::current_texture::texture_updated
        , [=]()
        {
          mv->getWorld()->notifyTileRendererOnSelectedTextureChange();
          _texturingTool->getGroundEffectsTool()->TextureChanged();
        }
      );
    }

    void TexturingTool::setupTexturePicker(MapView* mv)
    {
      // Dock
      _texturePickerDock = new QDockWidget("Texture picker", mv);
      _texturePickerDock->setObjectName("mapViewTexturePickerDock");
      _texturePickerDock->setFeatures(QDockWidget::DockWidgetMovable
        | QDockWidget::DockWidgetFloatable
        | QDockWidget::DockWidgetClosable);
      mv->mainWindow()->addDockWidget(Qt::BottomDockWidgetArea, _texturePickerDock);
      _texturePickerDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea
        | Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
      _texturePickerDock->hide();
      QObject::connect(mv, &QObject::destroyed, _texturePickerDock, &QObject::deleteLater);
      // End Dock

      _texturePicker = new Noggit::Ui::texture_picker(_texturingTool->_current_texture, mv);
      _texturePickerDock->setWidget(_texturePicker);
      QObject::connect(mv, &QObject::destroyed, _texturePicker, &QObject::deleteLater);

      QObject::connect(_texturePicker
        , &Noggit::Ui::texture_picker::set_texture
        , [=](scoped_blp_texture_reference texture)
        {
          mv->makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, mv->context());
          Noggit::Ui::selected_texture::set(std::move(texture));
        }
      );
      QObject::connect(_texturePicker, &Noggit::Ui::texture_picker::shift_left
        , [=]
        {
          mv->makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, mv->context());
          _texturePicker->shiftSelectedTextureLeft();
        }
      );
      QObject::connect(_texturePicker, &Noggit::Ui::texture_picker::shift_right
        , [=]
        {
          mv->makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, mv->context());
          _texturePicker->shiftSelectedTextureRight();
        }
      );
    }

    void TexturingTool::registerMenuItems(QMenu* menu)
    {
        addMenuTitle(menu, "Texture Painter");
        addMenuItem(menu, "Texture Browser", _show_texture_browser_window);
        addMenuItem(menu, "Texture palette", _show_texture_palette_window);
    }

    ToolDrawParameters TexturingTool::drawParameters() const
    {
        auto cursorType = CursorType::CIRCLE;
        bool const road_mode = _texturingTool->roadModeEnabled();
        float cursor_radius = _texturingTool->brush_radius();
        glm::vec3 cursor_position_override{};
        bool use_cursor_position_override = false;
        glm::vec4 cursor_color{1.f, 1.f, 1.f, _texturingTool->brushOpacity()};
        if (road_mode)
        {
            if ((_road_session == road_session_state::routing
                 || _road_session == road_session_state::reference_ready) && _road_style)
            {
                float cursor_path_distance = 0.0f;
                for (std::size_t index = 1; index < _road_preview_centerline.size(); ++index)
                {
                    glm::vec2 const delta{
                      _road_preview_centerline[index].x - _road_preview_centerline[index - 1].x,
                      _road_preview_centerline[index].z - _road_preview_centerline[index - 1].z
                    };
                    cursor_path_distance += glm::length(delta);
                }
                auto const [left_width, right_width] = sampled_road_widths_at(*_road_style,
                  cursor_path_distance, _texturingTool->roadWidthScale());
                cursor_radius = (left_width + right_width) * 0.5f;

                glm::vec2 direction = _road_style->outward_direction;
                if (_road_preview_centerline.size() >= 2)
                {
                    glm::vec3 const& previous = _road_preview_centerline[
                      _road_preview_centerline.size() - 2];
                    glm::vec3 const& current = _road_preview_centerline.back();
                    direction = {current.x - previous.x, current.z - previous.z};
                    if (glm::length(direction) > 0.001f)
                    {
                        direction = glm::normalize(direction);
                    }
                    else
                    {
                        direction = _road_style->outward_direction;
                    }
                    glm::vec2 const left_normal{-direction.y, direction.x};
                    cursor_position_override = current;
                    cursor_position_override.y -= 0.25f;
                    float const lateral_center = (left_width - right_width) * 0.5f;
                    cursor_position_override.x += left_normal.x * lateral_center;
                    cursor_position_override.z += left_normal.y * lateral_center;
                    use_cursor_position_override = true;
                }
            }
            else
            {
                cursor_radius = _texturingTool->roadReferenceRadius();
                cursor_color = {0.08f, 0.3f, 1.0f, 0.95f};
            }
        }
        if (_texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint && _texturingTool->getImageMaskSelector()->isEnabled())
            cursorType = CursorType::STAMP;
        else if (_texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
                 && _texturingTool->brushShape() == BrushShape::SQUARE)
            cursorType = CursorType::SQUARE;

        return
        {
            .radius = cursor_radius,
            .inner_radius = _texturingTool->hardness(),
            .cursor_position_override = cursor_position_override,
            .use_cursor_position_override = use_cursor_position_override,
            .show_unpaintable_chunks = _texturingTool->show_unpaintable_chunks(),
            .cursor_type = cursorType,
            .cursor_color = cursor_color,
            .road_preview_centerline = road_mode ? _road_preview_centerline : std::vector<glm::vec3>{},
            .road_preview_left_edge = road_mode ? _road_preview_left_edge : std::vector<glm::vec3>{},
            .road_preview_right_edge = road_mode ? _road_preview_right_edge : std::vector<glm::vec3>{},
            .road_preview_blocked = road_mode && _road_preview_blocked,
            .road_reference_centerline = road_mode ? _road_reference_centerline : std::vector<glm::vec3>{},
            .road_reference_left_edge = road_mode ? _road_reference_left_edge : std::vector<glm::vec3>{},
            .road_reference_right_edge = road_mode ? _road_reference_right_edge : std::vector<glm::vec3>{},
            .road_reference_mask_lines = road_mode ? _road_reference_mask_lines
                                                   : std::vector<std::vector<glm::vec3>>{},
        };
    }

    void TexturingTool::onSelected()
    {
        auto mv = mapView();
        if (_texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint && _texturingTool->getImageMaskSelector()->isEnabled())
        {
            _texturingTool->updateMaskImage();
        }
        else if (_texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::ground_effect)
        {
            _texturingTool->getGroundEffectsTool()->updateTerrainUniformParams();
        }

        bool use_classic_ui = mv->settings()->value("classicUI", false).toBool();
        if (use_classic_ui)
        {
            if (_texturingTool->show_unpaintable_chunks())
            {
                mv->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = true;
            }
        }
        else
        {
            if (mv->getLeftSecondaryViewToolbar()->showUnpaintableChunk())
            {
                mv->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = true;
            }
        }

        auto showTextureBrowser = _show_texture_browser_window.get() || mv->settings()->value("map_view/texture_browser", false).toBool();
        auto showTexturePalette = _show_texture_palette_window.get() || mv->settings()->value("map_view/texture_palette", false).toBool();
        _textureBrowserDock->setVisible(!mv->isUiHidden() && showTextureBrowser);
        _texturePaletteDock->setVisible(!mv->isUiHidden() && showTexturePalette);
    }

    void TexturingTool::onDeselected()
    {
        _texturingTool->texture_swap_tool()->cancel_viewport_adt_selection();
        _texturingTool->getGroundEffectsTool()->hide();

        QSignalBlocker const blocker1(_show_texture_palette_window);
        QSignalBlocker const blocker2(_show_texture_browser_window);
        QSignalBlocker const blocker3(_textureBrowserDock);
        _textureBrowserDock->setVisible(false);
        _texturePaletteDock->setVisible(false);
        _texturePickerDock->setVisible(false);
        _texturePickerNeedUpdate = false;
    }

    void TexturingTool::onTick(float deltaTime, TickParameters const& params)
    {
        auto mv = mapView();
        if (_texturingTool->texture_swap_tool()->viewport_adt_selection_active())
        {
            _texturingTool->texture_swap_tool()->refresh_viewport_adt_selection();
            return;
        }

        if (_texturingTool->roadModeEnabled())
        {
            if (_road_session == road_session_state::selecting_reference)
            {
                if (params.left_mouse && _road_reference_stroke_active && !params.underMap)
                {
                    paintRoadReferenceMask(mv->cursorPosition(), _road_reference_erase_stroke);
                }
            }
            else if (_road_session == road_session_state::routing && _road_style)
            {
                rebuildRoadPreview(mv->cursorPosition(), true);
                mv->invalidate();
            }
            return;
        }

        if (!params.left_mouse)
        {
            return;
        }

        if (_texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::ground_effect)
        {
            auto ge_tool = _texturingTool->getGroundEffectsTool();

            // Ctrl+Left-click is Noggit's established texture-layer picker in
            // every texturing mode. Ground Effects must never intercept it.
            if (params.mod_ctrl_down && !mv->isUiHidden())
            {
                _texturePickerNeedUpdate = true;
            }
            else if (params.mod_shift_down)
            {
                if (ge_tool->brush_mode() == Noggit::Ui::ground_effect_brush_mode::exclusion)
                {
                    NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNK_DOODADS_EXCLUSION,
                        Noggit::ActionModalityControllers::eSHIFT
                        | Noggit::ActionModalityControllers::eLMB);
                    mv->getWorld()->paintGroundEffectExclusion(mv->cursorPosition(), ge_tool->radius(), true);
                    // mv->getWorld()->setHole(mv->cursorPosition(), holeTool->brushRadius(), _mod_alt_down, false);
                }
                else if (ge_tool->brush_mode() == Noggit::Ui::ground_effect_brush_mode::effect)
                {
                    auto effect = ge_tool->getSelectedGroundEffect();
                    std::string const texture = _texturingTool->_current_texture->filename();

                    // the paint targets the selected texture's layers and silently
                    // does nothing without one; say so instead (once per stroke,
                    // onTick fires every frame the button is held)
                    if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
                    {
                        if (!_ge_brush_warning_shown)
                        {
                            mv->mainWindow()->statusBar()->showMessage("Ground effect brush: select a texture first - the effect is painted onto that texture's layers.", 2000);
                            _ge_brush_warning_shown = true;
                        }
                    }
                    // unsaved sets have id 0; painting that would clear instead
                    else if (!effect.has_value() || !effect->ID)
                    {
                        if (!_ge_brush_warning_shown)
                        {
                            mv->mainWindow()->statusBar()->showMessage("Ground effect brush: select a saved set first (unsaved sets have no id to paint).", 2000);
                            _ge_brush_warning_shown = true;
                        }
                    }
                    else if (!params.underMap)
                    {
                        NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNKS_LAYERINFO,
                            Noggit::ActionModalityControllers::eSHIFT
                            | Noggit::ActionModalityControllers::eLMB);
                        mv->getWorld()->paintGroundEffect(mv->cursorPosition(), ge_tool->radius(),
                            texture, effect->ID);
                        ge_tool->refreshOverlayForChunksInRange(mv->cursorPosition(), ge_tool->radius());
                    }
                }
                else if (ge_tool->brush_mode() == Noggit::Ui::ground_effect_brush_mode::erase_exclusion)
                {
                    NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNK_DOODADS_EXCLUSION,
                        Noggit::ActionModalityControllers::eSHIFT
                        | Noggit::ActionModalityControllers::eLMB);
                    mv->getWorld()->paintGroundEffectExclusion(mv->cursorPosition(), ge_tool->radius(), false);
                }
                else if (ge_tool->brush_mode() == Noggit::Ui::ground_effect_brush_mode::erase_effect)
                {
                    std::string const texture = _texturingTool->_current_texture->filename();

                    if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
                    {
                        if (!_ge_brush_warning_shown)
                        {
                            mv->mainWindow()->statusBar()->showMessage("Ground effect brush: select a texture first - the clear removes the effect from that texture's layers.", 2000);
                            _ge_brush_warning_shown = true;
                        }
                    }
                    else
                    {
                        NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNKS_LAYERINFO,
                            Noggit::ActionModalityControllers::eSHIFT
                            | Noggit::ActionModalityControllers::eLMB);
                        mv->getWorld()->paintGroundEffect(mv->cursorPosition(), ge_tool->radius(),
                            texture, 0);
                        ge_tool->refreshOverlayForChunksInRange(mv->cursorPosition(), ge_tool->radius());
                    }
                }
            }
        }
        else
        {
            if (params.mod_shift_down && params.mod_ctrl_down && params.mod_alt_down)
            {
                // clear chunk texture
                if (!params.underMap)
                {
                    NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNKS_TEXTURE,
                        Noggit::ActionModalityControllers::eSHIFT
                        | Noggit::ActionModalityControllers::eCTRL
                        | Noggit::ActionModalityControllers::eALT
                        | Noggit::ActionModalityControllers::eLMB);

                    mv->getWorld()->eraseTextures(mv->cursorPosition());
                }
            }
            else if (params.mod_ctrl_down && !mv->isUiHidden())
            {
                _texturePickerNeedUpdate = true;
                // Pick texture
                // _texturePickerDock->setVisible(true);
                // _texturePicker->setMainTexture(_texturingTool->_current_texture);
                // _texturePicker->getTextures(selection);
            }
            else  if (params.mod_shift_down && !!Noggit::Ui::selected_texture::get())
            {
                if ((params.displayMode == display_mode::in_3D && !params.underMap) || params.displayMode == display_mode::in_2D)
                {
                    auto image_mask_selector = _texturingTool->getImageMaskSelector();

                    if (NOGGIT_CUR_ACTION
                        && _texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
                        && image_mask_selector->isEnabled()
                        && !image_mask_selector->getBrushMode())
                    {
                        return;
                    }

                    auto action = NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNKS_TEXTURE,
                        Noggit::ActionModalityControllers::eSHIFT
                        | Noggit::ActionModalityControllers::eLMB);

                    action->setPostCallback([this] { randomizeTexturingRotation(); });

                    if (_texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
                        && image_mask_selector->isEnabled()
                        && !image_mask_selector->getBrushMode())
                        action->setBlockCursor(true);

                    _texture_stroke_active = true;
                    _texturingTool->paint(mv->getWorld(), mv->cursorPosition(), deltaTime,
                        *Noggit::Ui::selected_texture::get());
                }
            }
        }
    }

    void TexturingTool::onMousePress(MousePressParameters const& params)
    {
        if (params.button != Qt::MouseButton::LeftButton)
        {
            return;
        }

        if (_texturingTool->texture_swap_tool()->viewport_adt_selection_active())
        {
            _texturingTool->texture_swap_tool()->refresh_viewport_adt_selection();
            _texturingTool->texture_swap_tool()->toggle_viewport_adt(mapView()->cursorPosition());
            return;
        }

        if (_texturingTool->roadModeEnabled())
        {
            if (_road_session == road_session_state::selecting_reference
                && !params.mod_shift_down && !params.mod_alt_down)
            {
                _road_reference_stroke_active = true;
                _road_reference_erase_stroke = params.mod_ctrl_down;
                _road_reference_last_brush_position.reset();
                paintRoadReferenceMask(mapView()->cursorPosition(), _road_reference_erase_stroke);
            }
            else if (_road_session == road_session_state::reference_ready
                     && !params.mod_ctrl_down && !params.mod_shift_down && !params.mod_alt_down)
            {
                setRoadStart(mapView()->cursorPosition());
            }
            else if (params.mod_ctrl_down)
            {
                sampleRoadAtCursor();
            }
            else if (_road_session == road_session_state::routing
                     && params.mod_shift_down && _road_style)
            {
                addRoadControlPoint(mapView()->cursorPosition());
            }
            return;
        }

        if (!params.mod_ctrl_down)
        {
            return;
        }

        mapView()->doSelection(false, false);
    }

    void TexturingTool::onMouseRelease(MouseReleaseParameters const& params)
    {
        if (params.button == Qt::MouseButton::LeftButton)
        {
            _road_reference_stroke_active = false;
            _road_reference_erase_stroke = false;
            _road_reference_last_brush_position.reset();
            _ge_brush_warning_shown = false;
            if (_texture_stroke_active)
            {
                // Alphamap changes alter which texture supplies the visible
                // ground effect. Rebuild the placement cache immediately at
                // stroke end instead of waiting for a later terrain edit.
                Noggit::DetailDoodads::bumpDbcStamp();
                mapView()->invalidate();
                _texture_stroke_active = false;
            }
        }
    }

    void TexturingTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (_texturingTool->texture_swap_tool()->viewport_adt_selection_active())
        {
            return;
        }

        if (params.right_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _texturingTool->change_hardness(params.relative_movement.dx() / 300.0f);
            }

            if (params.mod_space_down)
            {
                if (_texturingTool->getImageMaskSelector()->isEnabled())
                {
                    auto action = NOGGIT_ACTION_MGR->beginAction(mapView(), Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                        Noggit::ActionModalityControllers::eRMB
                        | Noggit::ActionModalityControllers::eSPACE);
                    _texturingTool->getImageMaskSelector()->setRotation(-params.relative_movement.dx() / XSENS * 10.f);
                    action->setBlockCursor(true);
                }
            }
        }

        if (params.left_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _texturingTool->change_radius(params.relative_movement.dx() / XSENS);
            }

            if (params.mod_space_down)
            {
                _texturingTool->change_pressure(params.relative_movement.dx() / 300.0f);
            }
        }
    }

    void TexturingTool::onMouseWheel(MouseWheelParameters const& params)
    {
        if (_texturingTool->roadModeEnabled() && params.mod_alt_down)
        {
            float const precision = params.mod_ctrl_down ? 0.1f : 0.5f;
            float const notches = static_cast<float>(params.event.angleDelta().y()) / 120.0f;
            _texturingTool->change_radius(notches * precision);
            mapView()->invalidate();
            return;
        }

        auto&& delta_for_range
        ([&](float range)
            {
                //! \note / 8.f for degrees, / 40.f for smoothness
                return (params.mod_ctrl_down ? 0.01f : 0.1f)
                    * range
                    // alt = horizontal delta
                    * (params.mod_alt_down ? params.event.angleDelta().x() : params.event.angleDelta().y())
                    / 320.f
                    ;
            }
        );

        if (params.mod_space_down)
        {
            _texturingTool->change_brush_level(delta_for_range(255.f));
        }
        else if (params.mod_alt_down)
        {
            _texturingTool->change_spray_size(delta_for_range(39.f));
        }
        else if (params.mod_shift_down)
        {
            _texturingTool->change_spray_pressure(delta_for_range(10.f));
        }
    }

    void TexturingTool::hidePopups()
    {
        _texturePalette->hide();
        _texturePickerDock->hide();
        _textureBrowserDock->hide();
    }

    void TexturingTool::randomizeTexturingRotation()
    {
        auto image_mask_selector = _texturingTool->getImageMaskSelector();
        if (!image_mask_selector->getRandomizeRotation())
            return;

        unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
        std::mt19937 gen(ms);
        std::uniform_int_distribution<> uid(0, 360);

        image_mask_selector->setRotation(uid(gen));
    }

    void TexturingTool::beginRoadReferenceSelection()
    {
        _road_session = road_session_state::selecting_reference;
        _road_reference_stroke_active = false;
        _road_reference_erase_stroke = false;
        _road_reference_last_brush_position.reset();
        _road_reference_effective_radius = _texturingTool->roadReferenceRadius();
        _road_style.reset();
        _road_control_points.clear();
        _road_preview_centerline.clear();
        _road_preview_left_edge.clear();
        _road_preview_right_edge.clear();
        _road_preview_blocked = false;
        _road_reference_centerline.clear();
        _road_reference_left_edge.clear();
        _road_reference_right_edge.clear();
        _road_reference_mask.clear();
        _road_reference_mask_lines.clear();
        _texturingTool->showRoadReferenceSelectionStatus(0);
        mapView()->mainWindow()->statusBar()->showMessage(
          "Road Builder: paint the road reference blue. Ctrl+paint erases; choose OK when finished.",
          4500);
        mapView()->invalidate();
    }

    void TexturingTool::paintRoadReferenceMask(glm::vec3 const& point, bool erase)
    {
        if (_road_session != road_session_state::selecting_reference)
        {
            return;
        }

        constexpr float grid_step = TEXDETAILSIZE;
        if (_road_reference_last_brush_position
            && glm::distance(glm::vec2{point.x, point.z},
                 glm::vec2{_road_reference_last_brush_position->x,
                           _road_reference_last_brush_position->z}) < grid_step * 0.2f)
        {
            return;
        }
        float const radius = std::max(grid_step, _texturingTool->roadReferenceRadius());
        glm::vec3 const stroke_start = _road_reference_last_brush_position.value_or(point);
        float const stroke_length = glm::length(glm::vec2{
          point.x - stroke_start.x, point.z - stroke_start.z});
        float const stamp_spacing = std::max(grid_step, radius * 0.22f);
        int const stamp_count = std::max(1,
          static_cast<int>(std::ceil(stroke_length / stamp_spacing)));

        for (int stamp_index = 0; stamp_index <= stamp_count; ++stamp_index)
        {
            float const fraction = static_cast<float>(stamp_index)
              / static_cast<float>(stamp_count);
            glm::vec3 const stamp = stroke_start * (1.0f - fraction) + point * fraction;
            int const min_x = static_cast<int>(std::floor((stamp.x - radius) / grid_step));
            int const max_x = static_cast<int>(std::floor((stamp.x + radius) / grid_step));
            int const min_z = static_cast<int>(std::floor((stamp.z - radius) / grid_step));
            int const max_z = static_cast<int>(std::floor((stamp.z + radius) / grid_step));
            for (int grid_z = min_z; grid_z <= max_z; ++grid_z)
            {
                for (int grid_x = min_x; grid_x <= max_x; ++grid_x)
                {
                    glm::vec2 const cell_center{
                      (static_cast<float>(grid_x) + 0.5f) * grid_step,
                      (static_cast<float>(grid_z) + 0.5f) * grid_step
                    };
                    if (glm::distance(cell_center, glm::vec2{stamp.x, stamp.z}) > radius)
                    {
                        continue;
                    }
                    std::pair<int, int> const key{grid_z, grid_x};
                    if (erase)
                    {
                        _road_reference_mask.erase(key);
                    }
                    else
                    {
                        _road_reference_mask[key] = 1.0f;
                    }
                }
            }
        }

        _road_reference_last_brush_position = point;
        _road_reference_centerline.clear();
        rebuildRoadReferenceMaskPreview();
        _texturingTool->showRoadReferenceSelectionStatus(_road_reference_mask.size());
        mapView()->invalidate();
    }

    void TexturingTool::rebuildRoadReferenceMaskPreview()
    {
        _road_reference_mask_lines.clear();
        _road_reference_left_edge.clear();
        _road_reference_right_edge.clear();
        if (_road_reference_mask.empty())
        {
            return;
        }

        constexpr float grid_step = TEXDETAILSIZE;
        auto* world = mapView()->getWorld();
        auto append_point = [&](std::vector<glm::vec3>& line, int grid_x, int grid_z)
        {
            glm::vec3 position{
              (static_cast<float>(grid_x) + 0.5f) * grid_step,
              0.0f,
              (static_cast<float>(grid_z) + 0.5f) * grid_step
            };
            position.y = world->get_ground_height(position).y + 0.35f;
            line.push_back(position);
        };
        auto flush_line = [&](std::vector<glm::vec3>& line)
        {
            if (line.size() == 1)
            {
                glm::vec3 left = line.front();
                glm::vec3 right = line.front();
                left.x -= grid_step * 0.45f;
                right.x += grid_step * 0.45f;
                line = {left, right};
            }
            if (line.size() >= 2)
            {
                _road_reference_mask_lines.push_back(line);
            }
            line.clear();
        };

        std::vector<glm::vec3> current_line;
        int current_z = std::numeric_limits<int>::min();
        int previous_x = std::numeric_limits<int>::min();
        for (auto const& [key, strength] : _road_reference_mask)
        {
            int const grid_z = key.first;
            int const grid_x = key.second;
            if (grid_z != current_z || (previous_x != std::numeric_limits<int>::min()
                && grid_x != previous_x + 1))
            {
                flush_line(current_line);
            }
            append_point(current_line, grid_x, grid_z);
            current_z = grid_z;
            previous_x = grid_x;
        }
        flush_line(current_line);
    }

    bool TexturingTool::extractRoadReferenceCenterline()
    {
        using mask_cell = std::pair<int, int>; // z, x
        auto* mv = mapView();
        if (_road_reference_mask.empty())
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: paint a road reference before choosing OK.", 3500);
            return false;
        }

        std::set<mask_cell> remaining;
        for (auto const& [cell, strength] : _road_reference_mask)
        {
            if (strength >= 0.25f)
            {
                remaining.insert(cell);
            }
        }
        auto neighbors = [](mask_cell const& cell)
        {
            std::array<mask_cell, 8> result{};
            std::size_t index = 0;
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if (dx || dz)
                    {
                        result[index++] = {cell.first + dz, cell.second + dx};
                    }
                }
            }
            return result;
        };

        std::set<mask_cell> selected_component;
        std::size_t discarded_cells = 0;
        while (!remaining.empty())
        {
            std::set<mask_cell> component;
            std::queue<mask_cell> pending;
            pending.push(*remaining.begin());
            remaining.erase(remaining.begin());
            while (!pending.empty())
            {
                mask_cell const cell = pending.front();
                pending.pop();
                component.insert(cell);
                for (mask_cell const& neighbor : neighbors(cell))
                {
                    auto found = remaining.find(neighbor);
                    if (found != remaining.end())
                    {
                        pending.push(*found);
                        remaining.erase(found);
                    }
                }
            }
            if (component.size() > selected_component.size())
            {
                discarded_cells += selected_component.size();
                selected_component = std::move(component);
            }
            else
            {
                discarded_cells += component.size();
            }
        }
        if (selected_component.size() < 12
            || discarded_cells > std::max<std::size_t>(16, selected_component.size() / 8))
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: selection is too small or split into disconnected painted areas.", 4500);
            return false;
        }

        // Zhang-Suen thinning converts the painted corridor into a one-cell-wide
        // medial skeleton without depending on the order of the user's strokes.
        std::set<mask_cell> skeleton = selected_component;
        auto occupied = [&](int z, int x) { return skeleton.count({z, x}) != 0; };
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (int sub_iteration = 0; sub_iteration < 2; ++sub_iteration)
            {
                std::vector<mask_cell> remove;
                for (mask_cell const& cell : skeleton)
                {
                    int const z = cell.first;
                    int const x = cell.second;
                    std::array<int, 8> const p{
                      occupied(z - 1, x), occupied(z - 1, x + 1), occupied(z, x + 1),
                      occupied(z + 1, x + 1), occupied(z + 1, x), occupied(z + 1, x - 1),
                      occupied(z, x - 1), occupied(z - 1, x - 1)
                    };
                    int const count = std::accumulate(p.begin(), p.end(), 0);
                    int transitions = 0;
                    for (std::size_t index = 0; index < p.size(); ++index)
                    {
                        transitions += !p[index] && p[(index + 1) % p.size()];
                    }
                    bool const first_triplet = sub_iteration == 0
                      ? p[0] * p[2] * p[4] == 0 : p[0] * p[2] * p[6] == 0;
                    bool const second_triplet = sub_iteration == 0
                      ? p[2] * p[4] * p[6] == 0 : p[0] * p[4] * p[6] == 0;
                    if (count >= 2 && count <= 6 && transitions == 1
                        && first_triplet && second_triplet)
                    {
                        remove.push_back(cell);
                    }
                }
                for (mask_cell const& cell : remove)
                {
                    skeleton.erase(cell);
                }
                changed = changed || !remove.empty();
            }
        }
        if (skeleton.size() < 2)
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: painted area does not form a usable road-shaped strip.", 4000);
            return false;
        }

        using queue_entry = std::pair<float, mask_cell>;
        auto farthest_from = [&](mask_cell const& start, std::map<mask_cell, mask_cell>* parents)
        {
            std::priority_queue<queue_entry, std::vector<queue_entry>, std::greater<queue_entry>> pending;
            std::map<mask_cell, float> distances;
            distances[start] = 0.0f;
            pending.push({0.0f, start});
            mask_cell farthest = start;
            while (!pending.empty())
            {
                auto const [distance, cell] = pending.top();
                pending.pop();
                if (distance > distances[cell] + 0.0001f)
                {
                    continue;
                }
                if (distance > distances[farthest])
                {
                    farthest = cell;
                }
                for (mask_cell const& neighbor : neighbors(cell))
                {
                    if (!skeleton.count(neighbor))
                    {
                        continue;
                    }
                    bool const diagonal = neighbor.first != cell.first
                      && neighbor.second != cell.second;
                    float const candidate = distance + (diagonal ? 1.41421356f : 1.0f);
                    auto existing = distances.find(neighbor);
                    if (existing == distances.end() || candidate < existing->second)
                    {
                        distances[neighbor] = candidate;
                        if (parents)
                        {
                            (*parents)[neighbor] = cell;
                        }
                        pending.push({candidate, neighbor});
                    }
                }
            }
            return std::pair<mask_cell, float>{farthest, distances[farthest]};
        };

        mask_cell const first_end = farthest_from(*skeleton.begin(), nullptr).first;
        std::map<mask_cell, mask_cell> parents;
        auto const [second_end, skeleton_length] = farthest_from(first_end, &parents);
        std::vector<mask_cell> path;
        for (mask_cell cell = second_end;;)
        {
            path.push_back(cell);
            if (cell == first_end)
            {
                break;
            }
            auto parent = parents.find(cell);
            if (parent == parents.end())
            {
                mv->mainWindow()->statusBar()->showMessage(
                  "Road Builder: could not determine a continuous centerline.", 4000);
                return false;
            }
            cell = parent->second;
        }
        std::reverse(path.begin(), path.end());

        constexpr float grid_step = TEXDETAILSIZE;
        float const world_length = skeleton_length * grid_step;
        if (world_length < 10.0f)
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: painted road reference is too short.", 3500);
            return false;
        }

        _road_reference_centerline.clear();
        _road_reference_centerline.reserve(path.size());
        for (mask_cell const& cell : path)
        {
            glm::vec3 position{
              (static_cast<float>(cell.second) + 0.5f) * grid_step,
              0.0f,
              (static_cast<float>(cell.first) + 0.5f) * grid_step
            };
            position.y = mv->getWorld()->get_ground_height(position).y;
            _road_reference_centerline.push_back(position);
        }
        for (int pass = 0; pass < 3 && _road_reference_centerline.size() >= 3; ++pass)
        {
            std::vector<glm::vec3> smoothed = _road_reference_centerline;
            for (std::size_t index = 1; index + 1 < smoothed.size(); ++index)
            {
                smoothed[index] = _road_reference_centerline[index - 1] * 0.25f
                  + _road_reference_centerline[index] * 0.5f
                  + _road_reference_centerline[index + 1] * 0.25f;
                smoothed[index].y = mv->getWorld()->get_ground_height(smoothed[index]).y;
            }
            _road_reference_centerline = std::move(smoothed);
        }

        constexpr float pi = 3.14159265358979323846f;
        float const selected_area = static_cast<float>(selected_component.size())
          * grid_step * grid_step;
        _road_reference_effective_radius = std::clamp(
          (-world_length + std::sqrt(world_length * world_length + pi * selected_area)) / pi,
          TEXDETAILSIZE, 32.0f);
        return true;
    }

    void TexturingTool::acceptRoadReferenceSelection()
    {
        auto* mv = mapView();
        if (_road_session != road_session_state::selecting_reference
            || _road_reference_mask.empty())
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: choose Select Reference and paint over a road first.", 3500);
            return;
        }
        if (!extractRoadReferenceCenterline())
        {
            return;
        }

        float selected_length = 0.0f;
        for (std::size_t index = 1; index < _road_reference_centerline.size(); ++index)
        {
            glm::vec2 const delta{
              _road_reference_centerline[index].x - _road_reference_centerline[index - 1].x,
              _road_reference_centerline[index].z - _road_reference_centerline[index - 1].z
            };
            selected_length += glm::length(delta);
        }
        float const minimum_length = std::max(10.0f,
          _road_reference_effective_radius * 1.5f);
        if (selected_length < minimum_length)
        {
            mv->mainWindow()->statusBar()->showMessage(QString(
              "Road Builder: highlighted section is too short. Select at least %1 world units.")
              .arg(minimum_length, 0, 'f', 1), 4000);
            return;
        }

        // Resample at the native alphamap texel spacing. A coarser centerline
        // discarded longitudinal details such as wheel tracks before the
        // exemplar was even captured.
        float const sample_step = TEXDETAILSIZE;
        std::vector<glm::vec3> resampled;
        resampled.push_back(_road_reference_centerline.front());
        float distance_to_next = sample_step;
        for (std::size_t index = 1; index < _road_reference_centerline.size(); ++index)
        {
            glm::vec3 segment_start = _road_reference_centerline[index - 1];
            glm::vec3 const segment_end = _road_reference_centerline[index];
            glm::vec2 segment_delta{segment_end.x - segment_start.x,
                                    segment_end.z - segment_start.z};
            float remaining = glm::length(segment_delta);
            while (remaining >= distance_to_next && remaining > 0.001f)
            {
                float const t = distance_to_next / remaining;
                segment_start += (segment_end - segment_start) * t;
                resampled.push_back(segment_start);
                segment_delta = {segment_end.x - segment_start.x, segment_end.z - segment_start.z};
                remaining = glm::length(segment_delta);
                distance_to_next = sample_step;
            }
            distance_to_next -= remaining;
        }
        if (glm::length(glm::vec2{
              resampled.back().x - _road_reference_centerline.back().x,
              resampled.back().z - _road_reference_centerline.back().z}) > 0.25f)
        {
            resampled.push_back(_road_reference_centerline.back());
        }
        if (resampled.size() < 8)
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: highlighted section does not contain enough usable samples.", 3500);
            return;
        }

        glm::vec3 const sample_center = resampled[resampled.size() / 2];
        if (!sampleRoadAt(sample_center, &resampled))
        {
            _road_session = road_session_state::selecting_reference;
            rebuildRoadReferenceMaskPreview();
            _texturingTool->showRoadReferenceSelectionStatus(_road_reference_mask.size());
        }
    }

    void TexturingTool::cancelRoadSession()
    {
        clearRoadPreview(true);
        mapView()->mainWindow()->statusBar()->showMessage(
          "Road Builder: selection cleared. Select Reference to begin again.", 2500);
    }

    void TexturingTool::setRoadStart(glm::vec3 const& point)
    {
        if (_road_session != road_session_state::reference_ready || !_road_style)
        {
            return;
        }
        _road_style->start_position = point;
        _road_control_points = {point};
        _road_preview_centerline.clear();
        _road_preview_left_edge.clear();
        _road_preview_right_edge.clear();
        _road_preview_blocked = false;
        _road_session = road_session_state::routing;
        mapView()->mainWindow()->statusBar()->showMessage(
          "Road Builder: start placed. Shift+click route points, then Commit Road.", 4500);
        mapView()->invalidate();
    }

    bool TexturingTool::sampleRoadAtCursor()
    {
        return sampleRoadAt(mapView()->cursorPosition());
    }

    bool TexturingTool::sampleRoadAt(glm::vec3 const& click,
                                     std::vector<glm::vec3> const* reference_centerline)
    {
        auto* mv = mapView();
        World* world = mv->getWorld();
        MapChunk* chunk = world->getChunkAt(click);
        if (!chunk || !chunk->getTextureSet())
        {
            mv->mainWindow()->statusBar()->showMessage("Road Builder: no loaded terrain under the cursor.", 2500);
            return false;
        }

        auto seed_sample = chunk->getTextureSet()->samplePaintedTexture(click.x, click.z);
        if (!seed_sample)
        {
            mv->mainWindow()->statusBar()->showMessage("Road Builder: no terrain texture could be sampled.", 2500);
            return false;
        }

        scoped_blp_texture_reference const seed_texture = seed_sample->texture;
        std::string const seed_filename = seed_texture->file_key().filepath();
        auto sample_layers_nearest = [world](glm::vec2 const& point)
        {
            MapChunk* sample_chunk = world->getChunkAt({point.x, 0.0f, point.y});
            return sample_chunk && sample_chunk->getTextureSet()
                ? sample_chunk->getTextureSet()->sampleTextureLayersAt(point.x, point.y)
                : std::vector<sampled_texture_layer>{};
        };
        // Reference capture must retain the faint alpha values which form a
        // Blizzard-style dirt-to-grass transition. The underlying terrain
        // sampler is intentionally nearest-texel, so interpolate the four
        // surrounding alphamap texel centres here. Resolving every centre back
        // through World also makes the filter continuous across chunk edges.
        auto sample_layers = [world, &sample_layers_nearest](glm::vec2 const& point)
        {
            MapChunk* center_chunk = world->getChunkAt({point.x, 0.0f, point.y});
            if (!center_chunk || !center_chunk->getTextureSet())
            {
                return std::vector<sampled_texture_layer>{};
            }

            float const texel_x = (point.x - center_chunk->xbase) / TEXDETAILSIZE - 0.5f;
            float const texel_z = (point.y - center_chunk->zbase) / TEXDETAILSIZE - 0.5f;
            int const lower_x = static_cast<int>(std::floor(texel_x));
            int const lower_z = static_cast<int>(std::floor(texel_z));
            float const fraction_x = texel_x - static_cast<float>(lower_x);
            float const fraction_z = texel_z - static_cast<float>(lower_z);

            std::vector<sampled_texture_layer> blended;
            for (int dz = 0; dz < 2; ++dz)
            {
                for (int dx = 0; dx < 2; ++dx)
                {
                    float const blend_weight = (dx ? fraction_x : 1.0f - fraction_x)
                      * (dz ? fraction_z : 1.0f - fraction_z);
                    if (blend_weight <= 0.0f)
                    {
                        continue;
                    }
                    glm::vec2 const sample_position{
                      center_chunk->xbase
                        + (static_cast<float>(lower_x + dx) + 0.5f) * TEXDETAILSIZE,
                      center_chunk->zbase
                        + (static_cast<float>(lower_z + dz) + 0.5f) * TEXDETAILSIZE
                    };
                    for (sampled_texture_layer const& layer : sample_layers_nearest(sample_position))
                    {
                        auto existing = std::find_if(blended.begin(), blended.end(),
                          [&](sampled_texture_layer const& candidate)
                          {
                              return candidate.texture == layer.texture;
                          });
                        if (existing == blended.end())
                        {
                            blended.push_back(layer);
                            blended.back().weight = layer.weight * blend_weight;
                        }
                        else
                        {
                            existing->weight += layer.weight * blend_weight;
                        }
                    }
                }
            }
            return blended;
        };
        auto weight_for = [&](glm::vec2 const& point, std::string const& filename)
        {
            for (auto const& layer : sample_layers(point))
            {
                if (layer.texture->file_key().filepath() == filename)
                {
                    return layer.weight;
                }
            }
            return 0.0f;
        };

        // Estimate the local centerline axis from the selected material's alpha
        // distribution. The major covariance axis follows a normal road strip.
        constexpr float analysis_radius = 24.0f;
        constexpr float analysis_step = 1.0f;
        double total_weight = 0.0;
        glm::dvec2 weighted_mean{};
        struct weighted_point { glm::dvec2 offset; double weight; };
        std::vector<weighted_point> points;
        for (float z = -analysis_radius; z <= analysis_radius; z += analysis_step)
        {
            for (float x = -analysis_radius; x <= analysis_radius; x += analysis_step)
            {
                float const weight = weight_for({click.x + x, click.z + z}, seed_filename);
                if (weight < 24.0f)
                {
                    continue;
                }
                points.push_back({{x, z}, weight});
                weighted_mean += glm::dvec2{x, z} * static_cast<double>(weight);
                total_weight += weight;
            }
        }
        if (points.size() < 12 || total_weight <= 0.0)
        {
            mv->mainWindow()->statusBar()->showMessage(
                "Road Builder: the selected texture does not form a clear road-sized region here.", 3500);
            return false;
        }
        weighted_mean /= total_weight;
        double covariance_xx = 0.0;
        double covariance_xz = 0.0;
        double covariance_zz = 0.0;
        for (auto const& point : points)
        {
            glm::dvec2 const centered = point.offset - weighted_mean;
            covariance_xx += centered.x * centered.x * point.weight;
            covariance_xz += centered.x * centered.y * point.weight;
            covariance_zz += centered.y * centered.y * point.weight;
        }
        float const angle = 0.5f * std::atan2(static_cast<float>(2.0 * covariance_xz),
                                              static_cast<float>(covariance_xx - covariance_zz));
        glm::vec2 tangent = glm::normalize(glm::vec2{std::cos(angle), std::sin(angle)});
        bool const has_reference_path = reference_centerline && reference_centerline->size() >= 2;
        if (has_reference_path)
        {
            glm::vec3 const& first = reference_centerline->front();
            glm::vec3 const& last = reference_centerline->back();
            glm::vec2 const selected_direction{last.x - first.x, last.z - first.z};
            if (glm::length(selected_direction) > 0.001f)
            {
                tangent = glm::normalize(selected_direction);
            }
        }
        glm::vec2 const normal{-tangent.y, tangent.x};

        constexpr int lateral_sample_count = 129;
        constexpr int longitudinal_sample_count = 9;
        // Discover material identities only inside the blue selection corridor.
        // The exemplar later follows those materials beyond the corridor to
        // capture their fade, but unrelated nearby terrain must not become part
        // of the road style merely because it shares a texture name.
        float const lateral_extent = has_reference_path
          ? std::clamp(_road_reference_effective_radius, TEXDETAILSIZE, 32.0f)
          : 32.0f;
        constexpr float exemplar_capture_extent = 48.0f;
        float const lateral_step = lateral_extent * 2.0f / static_cast<float>(lateral_sample_count - 1);
        float const longitudinal_step = TEXDETAILSIZE * 1.5f;
        struct candidate_profile
        {
            scoped_blp_texture_reference texture;
            std::array<float, lateral_sample_count> weights{};
            std::uint32_t flags = 0;
            std::uint32_t effect_id = 0xFFFFFFFF;
            float contrast = 0.0f;
            float association_score = 0.0f;
            float core_mean = 0.0f;
            float shoulder_mean = 0.0f;
            float outside_mean = 0.0f;
        };
        std::map<std::string, candidate_profile> candidates;

        for (int longitudinal = 0; longitudinal < longitudinal_sample_count; ++longitudinal)
        {
            float const along = (static_cast<float>(longitudinal)
              - static_cast<float>(longitudinal_sample_count - 1) * 0.5f) * longitudinal_step;
            glm::vec2 section_origin{click.x, click.z};
            glm::vec2 section_normal = normal;
            if (has_reference_path)
            {
                std::size_t const reference_index = static_cast<std::size_t>(std::round(
                  static_cast<float>(longitudinal)
                  / static_cast<float>(longitudinal_sample_count - 1)
                  * static_cast<float>(reference_centerline->size() - 1)));
                glm::vec3 const& reference_point = (*reference_centerline)[reference_index];
                section_origin = {reference_point.x, reference_point.z};
                std::size_t const previous_index = reference_index ? reference_index - 1 : reference_index;
                std::size_t const next_index = std::min(reference_index + 1,
                  reference_centerline->size() - 1);
                glm::vec2 local_direction{
                  (*reference_centerline)[next_index].x - (*reference_centerline)[previous_index].x,
                  (*reference_centerline)[next_index].z - (*reference_centerline)[previous_index].z
                };
                if (glm::length(local_direction) > 0.001f)
                {
                    local_direction = glm::normalize(local_direction);
                    section_normal = {-local_direction.y, local_direction.x};
                }
            }
            for (int lateral = 0; lateral < lateral_sample_count; ++lateral)
            {
                float const across = -lateral_extent + static_cast<float>(lateral) * lateral_step;
                glm::vec2 const position = has_reference_path
                  ? section_origin + section_normal * across
                  : section_origin + tangent * along + normal * across;
                for (auto const& layer : sample_layers(position))
                {
                    std::string const filename = layer.texture->file_key().filepath();
                    auto candidate = candidates.find(filename);
                    if (candidate == candidates.end())
                    {
                        candidate = candidates.emplace(filename, candidate_profile{
                          layer.texture, {}, layer.flags, layer.effect_id
                        }).first;
                    }
                    candidate->second.weights[lateral] += layer.weight
                      / static_cast<float>(longitudinal_sample_count);
                }
            }
        }

        auto seed_candidate = candidates.find(seed_filename);
        if (seed_candidate == candidates.end())
        {
            return false;
        }
        float seed_peak = 0.0f;
        float center_numerator = 0.0f;
        float center_denominator = 0.0f;
        float seed_left = 0.0f;
        float seed_right = 0.0f;
        bool found_seed_edge = false;
        for (int lateral = 0; lateral < lateral_sample_count; ++lateral)
        {
            float const across = -lateral_extent + static_cast<float>(lateral) * lateral_step;
            float const weight = seed_candidate->second.weights[lateral];
            seed_peak = std::max(seed_peak, weight);
            if (weight >= 24.0f)
            {
                if (!found_seed_edge)
                {
                    seed_left = across;
                    found_seed_edge = true;
                }
                seed_right = across;
                center_numerator += across * weight;
                center_denominator += weight;
            }
        }
        if (!found_seed_edge || center_denominator <= 0.0f)
        {
            return false;
        }
        float const center_offset = center_numerator / center_denominator;
        float const seed_half_width = std::max(TEXDETAILSIZE,
          (seed_right - seed_left) * 0.5f);
        float const road_band = std::max(6.0f, seed_half_width * 2.2f);

        for (auto& [filename, candidate] : candidates)
        {
            float const outside_start = has_reference_path
              ? std::max(seed_half_width * 1.35f, lateral_extent * 0.78f)
              : std::max(road_band * 1.35f, 25.0f);
            float core_sum = 0.0f;
            int core_count = 0;
            float shoulder_sum = 0.0f;
            int shoulder_count = 0;
            float outside_sum = 0.0f;
            int outside_count = 0;
            for (int lateral = 0; lateral < lateral_sample_count; ++lateral)
            {
                float const across = -lateral_extent + static_cast<float>(lateral) * lateral_step;
                float const centered_distance = std::abs(across - center_offset);
                if (centered_distance <= seed_half_width * 0.85f)
                {
                    core_sum += candidate.weights[lateral];
                    ++core_count;
                }
                else if (centered_distance <= road_band)
                {
                    shoulder_sum += candidate.weights[lateral];
                    ++shoulder_count;
                }
                else if (centered_distance >= outside_start)
                {
                    outside_sum += candidate.weights[lateral];
                    ++outside_count;
                }
            }
            candidate.core_mean = core_count ? core_sum / static_cast<float>(core_count) : 0.0f;
            candidate.shoulder_mean = shoulder_count
              ? shoulder_sum / static_cast<float>(shoulder_count) : 0.0f;
            candidate.outside_mean = outside_count
              ? outside_sum / static_cast<float>(outside_count) : 0.0f;
            candidate.contrast = candidate.core_mean - candidate.outside_mean;
            candidate.association_score = std::max(candidate.core_mean, candidate.shoulder_mean)
              - candidate.outside_mean;
        }

        std::vector<candidate_profile const*> selected_candidates;
        selected_candidates.push_back(&seed_candidate->second);
        std::vector<candidate_profile const*> additional_candidates;
        for (auto const& [filename, candidate] : candidates)
        {
            if (filename == seed_filename)
            {
                continue;
            }
            float const peak = *std::max_element(candidate.weights.begin(), candidate.weights.end());
            float const feature_mean = std::max(candidate.core_mean, candidate.shoulder_mean);
            bool const ambient_context = candidate.outside_mean >= feature_mean * 0.72f
              && candidate.association_score < 24.0f;
            // Terrain that remains strong well outside the detected road is
            // environmental context (usually grass), not a material the road
            // should stamp into every destination chunk. Retaining it only as
            // destination background is what allows the road to blend into the
            // grass already beneath it.
            if (!ambient_context && candidate.association_score >= 5.0f && peak >= 14.0f)
            {
                additional_candidates.push_back(&candidate);
            }
        }
        std::sort(additional_candidates.begin(), additional_candidates.end(),
          [](candidate_profile const* lhs, candidate_profile const* rhs)
          {
              return lhs->association_score > rhs->association_score;
          });
        for (candidate_profile const* candidate : additional_candidates)
        {
            if (selected_candidates.size() == 3)
            {
                break;
            }
            selected_candidates.push_back(candidate);
        }

        float road_left = center_offset - seed_half_width;
        float road_right = center_offset + seed_half_width;
        for (int lateral = 0; lateral < lateral_sample_count; ++lateral)
        {
            float combined_weight = 0.0f;
            for (candidate_profile const* candidate : selected_candidates)
            {
                combined_weight += candidate->weights[lateral];
            }
            if (combined_weight >= 16.0f)
            {
                float const across = -lateral_extent + static_cast<float>(lateral) * lateral_step;
                road_left = std::min(road_left, across);
                road_right = std::max(road_right, across);
            }
        }
        float const half_width = std::clamp(
          std::max(center_offset - road_left, road_right - center_offset),
          TEXDETAILSIZE, lateral_extent);

        auto raw_profile_weight = [&](candidate_profile const& candidate, float across)
        {
            float const position = std::clamp((across + lateral_extent) / (lateral_extent * 2.0f), 0.0f, 1.0f)
              * static_cast<float>(lateral_sample_count - 1);
            std::size_t const lower = static_cast<std::size_t>(std::floor(position));
            std::size_t const upper = std::min(lower + 1, static_cast<std::size_t>(lateral_sample_count - 1));
            float const fraction = position - static_cast<float>(lower);
            return candidate.weights[lower] * (1.0f - fraction) + candidate.weights[upper] * fraction;
        };

        sampled_road_style style;
        style.half_width = half_width;
        style.representative_half_width = half_width;
        for (candidate_profile const* candidate : selected_candidates)
        {
            road_material_profile material{candidate->texture, {}, candidate->flags, candidate->effect_id};
            // Every selected material is part of the captured road style. A
            // destination must use the same set across the entire path instead
            // of silently dropping shoulder/detail layers at chunk borders.
            material.required = true;
            material.structural = candidate == &seed_candidate->second;
            for (std::size_t profile_index = 0; profile_index < ROAD_PROFILE_SAMPLE_COUNT; ++profile_index)
            {
                float const normalized = static_cast<float>(profile_index)
                  / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1) * 2.0f - 1.0f;
                material.weights[profile_index] = raw_profile_weight(
                  *candidate, center_offset + normalized * half_width);
            }
            style.materials.push_back(std::move(material));
        }

        glm::vec2 const centered_click = glm::vec2{click.x, click.z} + normal * center_offset;
        auto scan_along = [&](glm::vec2 const& direction)
        {
            float last_inside = 0.0f;
            int outside_count = 0;
            for (float distance = 0.0f; distance <= 48.0f; distance += 1.0f)
            {
                if (weight_for(centered_click + direction * distance, seed_filename) >= 24.0f)
                {
                    last_inside = distance;
                    outside_count = 0;
                }
                else if (++outside_count >= 2)
                {
                    break;
                }
            }
            return last_inside;
        };
        float forward_span = 0.0f;
        float backward_span = 0.0f;
        float direction_confidence = 0.0f;
        if (has_reference_path)
        {
            glm::vec3 const& last = reference_centerline->back();
            std::size_t const direction_start = reference_centerline->size() > 4
              ? reference_centerline->size() - 4 : 0;
            glm::vec3 const& previous = (*reference_centerline)[direction_start];
            glm::vec2 selected_outward{last.x - previous.x, last.z - previous.z};
            if (glm::length(selected_outward) > 0.001f)
            {
                tangent = glm::normalize(selected_outward);
            }
            style.outward_direction = tangent;
            style.start_position = last;
            direction_confidence = 1.0f;
        }
        else
        {
            forward_span = scan_along(tangent);
            backward_span = scan_along(-tangent);
            float endpoint_distance = forward_span;
            if (backward_span < forward_span)
            {
                tangent = -tangent;
                endpoint_distance = backward_span;
            }
            style.outward_direction = tangent;
            style.start_position = {
              centered_click.x + tangent.x * endpoint_distance,
              click.y,
              centered_click.y + tangent.y * endpoint_distance
            };
            direction_confidence = std::clamp(
              std::abs(forward_span - backward_span) / (forward_span + backward_span + 1.0f),
              0.0f, 1.0f);
        }
        style.confidence = std::clamp(seed_peak / 255.0f * (0.65f + direction_confidence * 0.35f), 0.0f, 1.0f);

        // Capture a longer 2D alpha exemplar behind the detected endpoint. The
        // old road builder collapsed a few nearby cross-sections into one fixed
        // profile, producing a mechanically uniform ribbon. This strip retains
        // real longitudinal opacity, asymmetric shoulders, and width changes.
        glm::vec2 const outward = style.outward_direction;
        glm::vec2 const inward = -outward;
        style.exemplar_step = TEXDETAILSIZE;
        if (has_reference_path && reference_centerline->size() >= 2)
        {
            float selected_length = 0.0f;
            for (std::size_t index = 1; index < reference_centerline->size(); ++index)
            {
                glm::vec2 const delta{
                  (*reference_centerline)[index].x - (*reference_centerline)[index - 1].x,
                  (*reference_centerline)[index].z - (*reference_centerline)[index - 1].z
                };
                selected_length += glm::length(delta);
            }
            style.exemplar_step = selected_length
              / static_cast<float>(reference_centerline->size() - 1);
        }
        float const source_inset = std::clamp(style.half_width * 0.35f, 2.0f, 8.0f);
        glm::vec2 section_center{style.start_position.x, style.start_position.z};
        if (!has_reference_path)
        {
            section_center += inward * source_inset;
        }
        int consecutive_missing_sections = 0;
        style.exemplar_lateral_extent = exemplar_capture_extent;
        std::size_t const exemplar_section_limit = has_reference_path
          ? std::min(ROAD_EXEMPLAR_SAMPLE_COUNT, reference_centerline->size())
          : ROAD_EXEMPLAR_SAMPLE_COUNT;

        for (std::size_t section = 0; section < exemplar_section_limit; ++section)
        {
            glm::vec2 exemplar_normal{-outward.y, outward.x};
            if (has_reference_path)
            {
                std::size_t const reference_index = reference_centerline->size() - 1 - section;
                glm::vec3 const& reference_point = (*reference_centerline)[reference_index];
                section_center = {reference_point.x, reference_point.z};
                std::size_t const previous_index = reference_index ? reference_index - 1 : reference_index;
                std::size_t const next_index = std::min(reference_index + 1,
                  reference_centerline->size() - 1);
                glm::vec2 local_direction{
                  (*reference_centerline)[next_index].x - (*reference_centerline)[previous_index].x,
                  (*reference_centerline)[next_index].z - (*reference_centerline)[previous_index].z
                };
                if (glm::length(local_direction) > 0.001f)
                {
                    local_direction = glm::normalize(local_direction);
                    exemplar_normal = {-local_direction.y, local_direction.x};
                }
            }
            else if (section)
            {
                section_center += inward * style.exemplar_step;
            }

            float const trace_extent = style.exemplar_lateral_extent;
            float const recenter_extent = std::min(trace_extent,
              std::max(lateral_extent, style.half_width * 1.25f));
            float seed_offset_sum = 0.0f;
            float seed_weight_sum = 0.0f;
            for (float across = -recenter_extent; across <= recenter_extent; across += 0.5f)
            {
                float const seed_weight = weight_for(section_center + exemplar_normal * across,
                  seed_filename);
                if (seed_weight >= 12.0f)
                {
                    seed_offset_sum += across * seed_weight;
                    seed_weight_sum += seed_weight;
                }
            }
            if (seed_weight_sum <= 0.0f)
            {
                if (++consecutive_missing_sections >= 3)
                {
                    break;
                }
                continue;
            }
            consecutive_missing_sections = 0;
            float const recenter = std::clamp(seed_offset_sum / seed_weight_sum, -2.0f, 2.0f);
            section_center += exemplar_normal * recenter;

            std::size_t const capture_index = style.exemplar_sample_count;
            std::vector<std::array<float, ROAD_PROFILE_SAMPLE_COUNT>> raw_material_weights(
              style.materials.size());
            for (std::size_t profile_index = 0; profile_index < ROAD_PROFILE_SAMPLE_COUNT;
                 ++profile_index)
            {
                float const normalized = static_cast<float>(profile_index)
                  / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1) * 2.0f - 1.0f;
                // Store each row in one fixed source-space coordinate system.
                // The prior per-row normalization stretched every detected
                // shoulder edge to +/-1, turning irregular dirt into parallel
                // bands when the exemplar was replayed.
                float const across = normalized * style.exemplar_lateral_extent;
                auto const layers = sample_layers(section_center + exemplar_normal * across);
                for (std::size_t material_index = 0; material_index < style.materials.size();
                     ++material_index)
                {
                    float weight = 0.0f;
                    for (auto const& layer : layers)
                    {
                        if (layer.texture == style.materials[material_index].texture)
                        {
                            weight = layer.weight;
                            break;
                        }
                    }
                    raw_material_weights[material_index][profile_index] = weight;
                }
            }

            // A texture used by the road can also exist faintly in the ambient
            // terrain. Estimate that far-field value independently on each side
            // and use the road-induced contribution only for finding the road's
            // connected boundary. Do not store these reduced values: subtracting
            // ambient alpha from the exemplar made copied cobble and wheel tracks
            // visibly fainter than the selected source road.
            constexpr std::size_t ambient_sample_count = 9;
            std::vector<float> left_ambient(style.materials.size(), 0.0f);
            std::vector<float> right_ambient(style.materials.size(), 0.0f);
            for (std::size_t material_index = 0; material_index < style.materials.size();
                 ++material_index)
            {
                for (std::size_t ambient_index = 0; ambient_index < ambient_sample_count;
                     ++ambient_index)
                {
                    left_ambient[material_index]
                      += raw_material_weights[material_index][ambient_index];
                    right_ambient[material_index]
                      += raw_material_weights[material_index][ROAD_PROFILE_SAMPLE_COUNT - 1
                        - ambient_index];
                }
                left_ambient[material_index] /= static_cast<float>(ambient_sample_count);
                right_ambient[material_index] /= static_cast<float>(ambient_sample_count);
            }

            std::vector<std::array<float, ROAD_PROFILE_SAMPLE_COUNT>> adjusted_material_weights(
              style.materials.size());
            std::array<float, ROAD_PROFILE_SAMPLE_COUNT> combined_weights{};
            std::array<float, ROAD_PROFILE_SAMPLE_COUNT> structural_weights{};
            for (std::size_t profile_index = 0; profile_index < ROAD_PROFILE_SAMPLE_COUNT;
                 ++profile_index)
            {
                float const fraction = static_cast<float>(profile_index)
                  / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1);
                for (std::size_t material_index = 0; material_index < style.materials.size();
                     ++material_index)
                {
                    float const ambient = left_ambient[material_index] * (1.0f - fraction)
                      + right_ambient[material_index] * fraction;
                    float const road_weight = std::max(0.0f,
                      raw_material_weights[material_index][profile_index] - ambient);
                    adjusted_material_weights[material_index][profile_index] = road_weight;
                    combined_weights[profile_index] += road_weight;
                    if (style.materials[material_index].structural)
                    {
                        structural_weights[profile_index] += road_weight;
                    }
                }
            }

            // Anchor the captured row to the structural road core nearest the
            // highlighted centerline. A fixed wide strip can contain other bends,
            // paths, or isolated uses of the same cobblestone texture; those are
            // disconnected components and must never be replayed as side branches.
            std::size_t const center_profile = ROAD_PROFILE_SAMPLE_COUNT / 2;
            float const profile_step = trace_extent * 2.0f
              / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1);
            std::size_t const maximum_anchor_offset = std::min(center_profile,
              static_cast<std::size_t>(std::ceil(recenter_extent / profile_step)));
            std::size_t anchor_profile = center_profile;
            bool found_anchor = false;
            for (std::size_t offset = 0; offset <= maximum_anchor_offset; ++offset)
            {
                std::size_t const right_candidate = center_profile + offset;
                if (right_candidate < ROAD_PROFILE_SAMPLE_COUNT
                    && structural_weights[right_candidate] >= 12.0f)
                {
                    anchor_profile = right_candidate;
                    found_anchor = true;
                    break;
                }
                if (offset <= center_profile)
                {
                    std::size_t const left_candidate = center_profile - offset;
                    if (structural_weights[left_candidate] >= 12.0f)
                    {
                        anchor_profile = left_candidate;
                        found_anchor = true;
                        break;
                    }
                }
            }
            if (!found_anchor)
            {
                continue;
            }

            // Follow only the component attached to that anchor. Short gaps are
            // tolerated so hand-painted dirt remains irregular, but a sustained
            // clean-terrain run terminates the road before a remote same-texture
            // island can be admitted.
            constexpr std::size_t clean_gap_samples = 3;
            constexpr float connected_alpha_threshold = 2.0f;
            std::size_t left_profile = anchor_profile;
            std::size_t clean_samples = 0;
            for (int profile_index = static_cast<int>(anchor_profile); profile_index >= 0;
                 --profile_index)
            {
                if (combined_weights[static_cast<std::size_t>(profile_index)]
                    >= connected_alpha_threshold)
                {
                    left_profile = static_cast<std::size_t>(profile_index);
                    clean_samples = 0;
                }
                else if (++clean_samples >= clean_gap_samples)
                {
                    break;
                }
            }
            std::size_t right_profile = anchor_profile;
            clean_samples = 0;
            for (std::size_t profile_index = anchor_profile;
                 profile_index < ROAD_PROFILE_SAMPLE_COUNT; ++profile_index)
            {
                if (combined_weights[profile_index] >= connected_alpha_threshold)
                {
                    right_profile = profile_index;
                    clean_samples = 0;
                }
                else if (++clean_samples >= clean_gap_samples)
                {
                    break;
                }
            }

            for (std::size_t profile_index = left_profile; profile_index <= right_profile;
                 ++profile_index)
            {
                float captured_road_total = 0.0f;
                for (std::size_t material_index = 0; material_index < style.materials.size();
                     ++material_index)
                {
                    // The cleaned component above decides what belongs to the
                    // road. Within that component, reproduce the source alpha
                    // exactly instead of the ambient-subtracted detection value.
                    float const road_weight = raw_material_weights[material_index][profile_index];
                    style.materials[material_index]
                      .exemplar_weights[capture_index][profile_index] = road_weight;
                    captured_road_total += road_weight;
                }
                style.exemplar_coverage[capture_index][profile_index] = std::clamp(
                  captured_road_total / 255.0f, 0.0f, 1.0f);
            }

            float const left_edge = (static_cast<float>(left_profile)
              / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1) * 2.0f - 1.0f)
              * trace_extent;
            float const right_edge = (static_cast<float>(right_profile)
              / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1) * 2.0f - 1.0f)
              * trace_extent;
            // Positive profile coordinates lie on the geometric left side of
            // the local road tangent; negative coordinates lie on its right.
            float const right_width = std::clamp(-left_edge + TEXDETAILSIZE * 0.5f,
              TEXDETAILSIZE, trace_extent);
            float const left_width = std::clamp(right_edge + TEXDETAILSIZE * 0.5f,
              TEXDETAILSIZE, trace_extent);
            style.left_half_widths[capture_index] = left_width;
            style.right_half_widths[capture_index] = right_width;
            ++style.exemplar_sample_count;
        }

        if (has_reference_path && style.exemplar_sample_count < 12)
        {
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: the highlighted area is too short or leaves the road texture. Refine the selection.",
              4500);
            return false;
        }

        if (style.exemplar_sample_count >= 12)
        {
            style.has_longitudinal_exemplar = true;

            // The painted reference is the authoritative corridor. Alpha traces
            // farther away are ambient uses of the same texture, not part of the
            // road. Leave a modest allowance for a faint dirt shoulder that the
            // user may not have covered exactly with the blue mask.
            float const contour_limit = has_reference_path
              ? std::clamp(std::max(_road_reference_effective_radius * 1.25f,
                  half_width + TEXDETAILSIZE * 2.0f), TEXDETAILSIZE,
                  exemplar_capture_extent)
              : exemplar_capture_extent;
            for (std::size_t section = 0; section < style.exemplar_sample_count; ++section)
            {
                style.left_half_widths[section] = std::min(
                  style.left_half_widths[section], contour_limit);
                style.right_half_widths[section] = std::min(
                  style.right_half_widths[section], contour_limit);
            }

            // Width samples come from half-unit alpha traces. A short median
            // filter removes individual dirt flecks; the bidirectional slope
            // limiter removes saw-tooth jumps without flattening genuine broad
            // changes in the source road.
            auto clean_width_contour = [&](auto& widths)
            {
                std::vector<float> filtered(style.exemplar_sample_count);
                for (std::size_t section = 0; section < style.exemplar_sample_count; ++section)
                {
                    std::vector<float> window;
                    std::size_t const begin = section > 2 ? section - 2 : 0;
                    std::size_t const end = std::min(style.exemplar_sample_count, section + 3);
                    window.reserve(end - begin);
                    for (std::size_t neighbor = begin; neighbor < end; ++neighbor)
                    {
                        window.push_back(widths[neighbor]);
                    }
                    std::sort(window.begin(), window.end());
                    filtered[section] = window[window.size() / 2];
                }

                float const maximum_step = std::max(TEXDETAILSIZE * 1.5f,
                  style.exemplar_step);
                for (std::size_t section = 1; section < filtered.size(); ++section)
                {
                    filtered[section] = std::clamp(filtered[section],
                      filtered[section - 1] - maximum_step,
                      filtered[section - 1] + maximum_step);
                }
                for (std::size_t section = filtered.size() - 1; section > 0; --section)
                {
                    filtered[section - 1] = std::clamp(filtered[section - 1],
                      filtered[section] - maximum_step,
                      filtered[section] + maximum_step);
                }
                for (std::size_t section = 0; section < filtered.size(); ++section)
                {
                    widths[section] = std::clamp(filtered[section], TEXDETAILSIZE,
                      contour_limit);
                }
            };
            clean_width_contour(style.left_half_widths);
            clean_width_contour(style.right_half_widths);

            // Clip the captured pixels to the same cleaned contour consumed by
            // preview and painting. This makes the green edges and cursor a
            // truthful description of the terrain pixels that can be changed.
            for (std::size_t section = 0; section < style.exemplar_sample_count; ++section)
            {
                float const left_width = style.left_half_widths[section];
                float const right_width = style.right_half_widths[section];
                for (std::size_t profile_index = 0; profile_index < ROAD_PROFILE_SAMPLE_COUNT;
                     ++profile_index)
                {
                    float const normalized = static_cast<float>(profile_index)
                      / static_cast<float>(ROAD_PROFILE_SAMPLE_COUNT - 1) * 2.0f - 1.0f;
                    float const across = normalized * style.exemplar_lateral_extent;
                    if (across < -right_width || across > left_width)
                    {
                        style.exemplar_coverage[section][profile_index] = 0.0f;
                        for (road_material_profile& material : style.materials)
                        {
                            material.exemplar_weights[section][profile_index] = 0.0f;
                        }
                    }
                }
            }

            std::vector<float> representative_half_widths;
            representative_half_widths.reserve(style.exemplar_sample_count);
            float maximum_half_width = TEXDETAILSIZE;
            for (std::size_t section = 0; section < style.exemplar_sample_count; ++section)
            {
                float const left_width = style.left_half_widths[section];
                float const right_width = style.right_half_widths[section];
                representative_half_widths.push_back((left_width + right_width) * 0.5f);
                maximum_half_width = std::max(maximum_half_width,
                  std::max(left_width, right_width));
            }
            std::sort(representative_half_widths.begin(), representative_half_widths.end());
            style.half_width = maximum_half_width;
            style.representative_half_width = representative_half_widths[
              representative_half_widths.size() / 2];

            for (road_material_profile& material : style.materials)
            {
                for (std::size_t profile_index = 0; profile_index < ROAD_PROFILE_SAMPLE_COUNT;
                     ++profile_index)
                {
                    float total = 0.0f;
                    for (std::size_t section = 0; section < style.exemplar_sample_count; ++section)
                    {
                        total += material.exemplar_weights[section][profile_index];
                    }
                    material.weights[profile_index] = total
                      / static_cast<float>(style.exemplar_sample_count);
                }
            }
        }

        _road_style = std::move(style);
        _road_control_points.clear();
        _road_preview_centerline.clear();
        _road_preview_left_edge.clear();
        _road_preview_right_edge.clear();
        _road_preview_blocked = false;
        _road_session = road_session_state::reference_ready;
        _road_reference_stroke_active = false;
        _road_reference_erase_stroke = false;
        _road_reference_last_brush_position.reset();
        _road_reference_centerline.clear();
        _road_reference_left_edge.clear();
        _road_reference_right_edge.clear();
        _road_reference_mask.clear();
        _road_reference_mask_lines.clear();

        Noggit::Ui::selected_texture::set(seed_texture);
        _texturingTool->_current_texture->set_texture(seed_filename);
        _texturingTool->applyRoadSample(_road_style->representative_half_width,
          _road_style->confidence, _road_style->materials.size(),
          "reference captured - click a start point");
        mv->mainWindow()->statusBar()->showMessage(
            QString("Road Builder: captured %1 material(s), width %2. Click anywhere to place the road start.")
                .arg(_road_style->materials.size())
                .arg(_road_style->representative_half_width * 2.0f, 0, 'f', 1), 5000);
        mv->invalidate();
        return true;
    }

    void TexturingTool::addRoadControlPoint(glm::vec3 const& point)
    {
        if (!_road_style || _road_control_points.empty())
        {
            return;
        }
        glm::vec2 const delta{point.x - _road_control_points.back().x,
                              point.z - _road_control_points.back().z};
        if (glm::length(delta) < std::max(1.0f, _road_style->half_width * 0.25f))
        {
            return;
        }
        _road_control_points.push_back(point);
        rebuildRoadPreview(point, false);
        mapView()->mainWindow()->statusBar()->showMessage(
          QString("Road Builder: %1 route point(s). Add more or click Commit Road.")
            .arg(_road_control_points.size() - 1), 2500);
        mapView()->invalidate();
    }

    void TexturingTool::rebuildRoadPreview(glm::vec3 const& live_endpoint, bool include_live_endpoint)
    {
        _road_preview_centerline.clear();
        _road_preview_left_edge.clear();
        _road_preview_right_edge.clear();
        _road_preview_blocked = false;
        if (!_road_style || _road_control_points.empty())
        {
            return;
        }

        std::vector<glm::vec3> controls = _road_control_points;
        glm::vec2 const live_delta{live_endpoint.x - controls.back().x,
                                   live_endpoint.z - controls.back().z};
        if (include_live_endpoint && glm::length(live_delta) > 0.5f)
        {
            controls.push_back(live_endpoint);
        }
        if (controls.size() < 2)
        {
            return;
        }

        for (std::size_t segment_index = 0; segment_index + 1 < controls.size(); ++segment_index)
        {
            glm::vec3 const p1 = controls[segment_index];
            glm::vec3 const p2 = controls[segment_index + 1];
            float const segment_length = glm::distance(glm::vec2{p1.x, p1.z}, glm::vec2{p2.x, p2.z});
            glm::vec3 const p0 = segment_index == 0
              ? p1 - glm::vec3{_road_style->outward_direction.x, 0.0f,
                               _road_style->outward_direction.y} * std::max(segment_length, _road_style->half_width)
              : controls[segment_index - 1];
            glm::vec3 const p3 = segment_index + 2 < controls.size()
              ? controls[segment_index + 2]
              : p2 + (p2 - p1);
            int const samples = std::max(6, static_cast<int>(std::ceil(segment_length / 1.5f)));
            for (int sample_index = segment_index == 0 ? 0 : 1; sample_index <= samples; ++sample_index)
            {
                float const t = static_cast<float>(sample_index) / static_cast<float>(samples);
                float const t2 = t * t;
                float const t3 = t2 * t;
                glm::vec3 point = 0.5f * ((2.0f * p1)
                  + (-p0 + p2) * t
                  + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                  + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
                point.y += 0.25f;
                _road_preview_centerline.push_back(point);
            }
        }

        float preview_path_distance = 0.0f;
        for (std::size_t index = 0; index < _road_preview_centerline.size(); ++index)
        {
            if (index)
            {
                glm::vec2 const path_delta{
                  _road_preview_centerline[index].x - _road_preview_centerline[index - 1].x,
                  _road_preview_centerline[index].z - _road_preview_centerline[index - 1].z
                };
                preview_path_distance += glm::length(path_delta);
            }
            glm::vec3 const& previous = _road_preview_centerline[index ? index - 1 : index];
            glm::vec3 const& next = _road_preview_centerline[
              index + 1 < _road_preview_centerline.size() ? index + 1 : index];
            glm::vec2 direction{next.x - previous.x, next.z - previous.z};
            if (glm::length(direction) < 0.001f)
            {
                direction = _road_style->outward_direction;
            }
            else
            {
                direction = glm::normalize(direction);
            }
            auto const [left_width, right_width] = sampled_road_widths_at(*_road_style,
              preview_path_distance, _texturingTool->roadWidthScale());
            glm::vec3 const left_offset{-direction.y * left_width, 0.0f,
                                         direction.x * left_width};
            glm::vec3 const right_offset{-direction.y * right_width, 0.0f,
                                          direction.x * right_width};
            _road_preview_left_edge.push_back(_road_preview_centerline[index] + left_offset);
            _road_preview_right_edge.push_back(_road_preview_centerline[index] - right_offset);
        }

        for (std::size_t index = 0; index + 1 < controls.size(); ++index)
        {
            if (!mapView()->getWorld()->canPaintRoadSegment(controls[index], controls[index + 1],
                *_road_style, _texturingTool->roadWidthScale(),
                _texturingTool->roadReplaceConflictingTextures()))
            {
                _road_preview_blocked = true;
                break;
            }
        }
    }

    void TexturingTool::clearRoadPreview(bool clear_style)
    {
        _road_preview_centerline.clear();
        _road_preview_left_edge.clear();
        _road_preview_right_edge.clear();
        _road_preview_blocked = false;
        if (clear_style)
        {
            _road_session = road_session_state::idle;
            _road_reference_stroke_active = false;
            _road_reference_erase_stroke = false;
            _road_reference_last_brush_position.reset();
            _road_style.reset();
            _road_control_points.clear();
            _road_reference_centerline.clear();
            _road_reference_left_edge.clear();
            _road_reference_right_edge.clear();
            _road_reference_mask.clear();
            _road_reference_mask_lines.clear();
            if (_texturingTool)
            {
                _texturingTool->clearRoadSampleStatus();
            }
        }
        else if (_road_style)
        {
            _road_session = road_session_state::reference_ready;
            _road_control_points.clear();
            mapView()->mainWindow()->statusBar()->showMessage(
              "Road Builder: route reset. Click anywhere to place a new road start.", 3000);
        }
        mapView()->invalidate();
    }

    void TexturingTool::commitRoadPreview()
    {
        auto* mv = mapView();
        if (!_road_style || _road_control_points.size() < 2)
        {
            mv->mainWindow()->statusBar()->showMessage(
              _road_session == road_session_state::reference_ready
                ? "Road Builder: click anywhere to place the road start first."
                : "Road Builder: Shift+click at least one route point before committing.", 3000);
            return;
        }
        rebuildRoadPreview(_road_control_points.back(), false);
        if (_road_preview_centerline.size() < 2)
        {
            return;
        }
        if (!mv->getWorld()->canPaintRoadPath(_road_preview_centerline,
            *_road_style, _texturingTool->roadWidthScale(),
            _texturingTool->roadReplaceConflictingTextures()))
        {
            _road_preview_blocked = true;
            mv->mainWindow()->statusBar()->showMessage(
              "Road Builder: route is blocked by a four-texture chunk. No terrain was changed.", 4500);
            mv->invalidate();
            return;
        }

        NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        road_paint_result const result = mv->getWorld()->paintRoadPath(
          _road_preview_centerline, *_road_style, _texturingTool->roadWidthScale(),
          _texturingTool->roadOpacityScale(), _texturingTool->roadReplaceConflictingTextures());
        NOGGIT_ACTION_MGR->endAction();

        // Preview vertices are lifted slightly to keep the guide visible above terrain.
        // Keep the continuation anchor on the sampled terrain so repeated commits do not
        // accumulate that render-only offset.
        glm::vec3 new_start = _road_preview_centerline.back();
        new_start.y -= 0.25f;
        if (_road_preview_centerline.size() >= 2)
        {
            glm::vec3 const previous = _road_preview_centerline[_road_preview_centerline.size() - 2];
            glm::vec2 direction{new_start.x - previous.x, new_start.z - previous.z};
            if (glm::length(direction) > 0.001f)
            {
                _road_style->outward_direction = glm::normalize(direction);
            }
        }
        _road_style->start_position = new_start;
        _road_control_points = {new_start};
        _road_preview_centerline.clear();
        _road_preview_left_edge.clear();
        _road_preview_right_edge.clear();
        _road_preview_blocked = false;
        if (result.changed)
        {
            Noggit::DetailDoodads::bumpDbcStamp();
            QString message = "Road Builder: multi-material road committed as one undo action.";
            if (result.replaced_texture_layers)
            {
                message += QString(" Replaced %1 conflicting chunk texture layer(s).")
                  .arg(result.replaced_texture_layers);
            }
            mv->mainWindow()->statusBar()->showMessage(message, 5000);
        }
        mv->invalidate();
    }
}
