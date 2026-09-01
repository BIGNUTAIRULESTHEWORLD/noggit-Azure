#include <noggit/ActionManager.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/DBC.h>
#include <noggit/DetailDoodads.hpp>
#include <noggit/MapChunk.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/texture_set.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/GroundEffectsTool.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/tools/AssetBrowser/Ui/AssetBrowser.hpp>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/Log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <unordered_set>

#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMenu>
#include <QMessageBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressDialog>
#include <QSettings>
#include <QSignalBlocker>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QVBoxLayout>

namespace
{
    constexpr unsigned int wotlk_max_ground_effect_id = 73186;

    std::string normalizedTextureName(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
            { return static_cast<char>(std::tolower(c)); });
        std::replace(name.begin(), name.end(), '\\', '/');
        return name;
    }
}

namespace Noggit
{
    namespace Ui
    {
        GroundEffectsTool::GroundEffectsTool(texturing_tool* texturing_tool, MapView* map_view, QWidget* parent)
            : QWidget(parent, Qt::Window), _texturing_tool(texturing_tool), _map_view(map_view)
        {
            setWindowTitle("Ground Effects Tool");
            setMinimumSize(750, 600);
            setWindowFlags(Qt::Window
                | Qt::WindowTitleHint
                | Qt::WindowSystemMenuHint
                | Qt::WindowMinimizeButtonHint
                | Qt::WindowCloseButtonHint
                | Qt::WindowStaysOnTopHint);
            setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            QHBoxLayout* main_layout = new QHBoxLayout(this);
            QVBoxLayout* left_side_layout = new QVBoxLayout(this);
            QVBoxLayout* right_side_layout = new QVBoxLayout(this);
            main_layout->addLayout(left_side_layout);
            main_layout->addLayout(right_side_layout);

            // Render modes.
            {
                _render_group_box = new QGroupBox("Render Mode", this);
                _render_group_box->setCheckable(true);
                _render_group_box->setChecked(true);
                left_side_layout->addWidget(_render_group_box);

                auto render_layout(new QGridLayout(_render_group_box));
                _render_group_box->setLayout(render_layout);

                _render_type_group = new QButtonGroup(_render_group_box);

                _render_active_sets = new QRadioButton("Effect Id/Set", this);
                _render_active_sets->setToolTip("Render all the loaded effect sets for this texture in matching colors");
                _render_type_group->addButton(_render_active_sets);
                render_layout->addWidget(_render_active_sets, 0, 0);

                _render_exclusion_map = new QRadioButton("Doodads Disabled", this);
                _render_exclusion_map->setToolTip("Render 8x8 cells where detail doodads are disabled as white, and enabled cells as black.");
                _render_type_group->addButton(_render_exclusion_map);
                render_layout->addWidget(_render_exclusion_map, 1, 0);

                // If chunk contains Texture/Effect : Render as green or red if the effect layer is active or not.
                _render_placement_map = new QRadioButton("Selected Texture state", this);
                _render_placement_map->setToolTip("Render chunk unit as red if texture is present in the chunk and NOT the current \
                active layer, render as green if it's active. \nThis defines which of the 4 textures' set is currently active,\
                this is determined by which has the highest opacity.");
                _render_type_group->addButton(_render_placement_map);
                render_layout->addWidget(_render_placement_map, 0, 1);

                _render_active_sets->setChecked(true);
            }

            _chkbox_merge_duplicates = new QCheckBox("Ignore duplicates", this);
            _chkbox_merge_duplicates->setChecked(true);
            left_side_layout->addWidget(_chkbox_merge_duplicates);

            auto button_scan_adt = new QPushButton("Scan for sets in curr tile", this);
            left_side_layout->addWidget(button_scan_adt);

            auto button_scan_adt_loaded = new QPushButton("Scan for sets in loaded Tiles", this);
            left_side_layout->addWidget(button_scan_adt_loaded);

            auto button_load_all_dbc = new QPushButton("Load all sets from DBC", this);
            button_load_all_dbc->setToolTip("Adds every GroundEffectTexture.dbc record to the list, not just the ones found by scanning.");
            left_side_layout->addWidget(button_load_all_dbc);

            // Selection.
            auto selection_group = new QGroupBox("Effect Set Selection", this);
            selection_group->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            left_side_layout->addWidget(selection_group);
            auto selection_layout(new QVBoxLayout(selection_group));
            selection_group->setLayout(selection_layout);

            selection_layout->addWidget(new QLabel("Blizzard Ground Effects", this));
            _blizzard_assignments_combo = new QComboBox(this);
            _blizzard_assignments_combo->addItem("Choose a Blizzard ground effect...");
            _blizzard_assignments_combo->setEnabled(false);
            _blizzard_assignments_combo->setToolTip(
                "Choose an effect Blizzard used with the current terrain texture. "
                "This only changes the editor selection; it does not apply anything to terrain.");
            selection_layout->addWidget(_blizzard_assignments_combo);

            auto set_buttons_widget = new QWidget(this);
            auto set_buttons_layout = new QHBoxLayout(set_buttons_widget);
            set_buttons_layout->setContentsMargins(0, 0, 0, 0);
            selection_layout->addWidget(set_buttons_widget);

            auto button_create_new = new QPushButton("Create New", this);
            set_buttons_layout->addWidget(button_create_new);

            auto button_duplicate = new QPushButton("Duplicate", this);
            button_duplicate->setToolTip("Copies the selected set into a new unsaved set to edit from.");
            set_buttons_layout->addWidget(button_duplicate);

            auto button_delete = new QPushButton("Delete", this);
            button_delete->setToolTip("Removes the selected set's GroundEffectTexture record from the dbc.\
            \nChunks still referencing the id simply spawn nothing, like the client with a missing record.");
            set_buttons_layout->addWidget(button_delete);

            connect(button_create_new, &QPushButton::clicked, [this]() { createNewSet(); });
            connect(button_duplicate, &QPushButton::clicked, [this]() { duplicateSelectedSet(); });
            connect(button_delete, &QPushButton::clicked, [this]() { deleteSelectedSet(); });
            connect(_blizzard_assignments_combo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &GroundEffectsTool::selectBlizzardGroundEffect);

            _effect_sets_list = new QListWidget(this);
            selection_layout->addWidget(_effect_sets_list);
            _effect_sets_list->setViewMode(QListView::ListMode);
            _effect_sets_list->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
            _effect_sets_list->setSelectionBehavior(QAbstractItemView::SelectItems);
            _effect_sets_list->setUniformItemSizes(true);
            _effect_sets_list->setFixedHeight(160);
            _effect_sets_list->setIconSize(QSize(20, 20));

            // Effect settings.
            {
                auto settings_group = new QGroupBox("Selected Set Settings", this);
                settings_group->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                right_side_layout->addWidget(settings_group);
                auto settings_layout(new QFormLayout(settings_group));
                settings_group->setLayout(settings_layout);

                auto set_help_label = new QLabel("A ground Effect Set contains up to 4 different doodads.\nTerrain Type is used for footprints and sounds.");
                settings_layout->addRow(set_help_label);

                _object_list = new QListWidget(this);
                _object_list->setItemAlignment(Qt::AlignCenter);
                _object_list->setViewMode(QListView::IconMode);
                _object_list->setWrapping(false);
                _object_list->setIconSize(QSize(100, 100));
                _object_list->setFlow(QListWidget::LeftToRight);
                _object_list->setSelectionMode(QAbstractItemView::SingleSelection);
                _object_list->setAcceptDrops(false);
                _object_list->setMovement(QListView::Movement::Static);
                _object_list->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
                _object_list->setFixedWidth(_object_list->iconSize().width() * 4 + 40); //  padding-right: 10px * 4
                _object_list->setFixedHeight(_object_list->iconSize().height() + 20);

                settings_layout->addRow(_object_list);
                for (int i = 0; i < 4; i++)
                {
                    QListWidgetItem* list_item = new QListWidgetItem(_object_list);
                    list_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                    list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
                    list_item->setText(STRING_EMPTY_DISPLAY);
                    list_item->setToolTip("");
                    _object_list->addItem(list_item);
                }

                // One weight per doodad slot, aligned under the icons.
                auto weights_widget = new QWidget(this);
                auto weights_layout = new QHBoxLayout(weights_widget);
                weights_layout->setContentsMargins(0, 0, 0, 0);
                for (int i = 0; i < 4; i++)
                {
                    _weight_spinboxes[i] = new QSpinBox(this);
                    _weight_spinboxes[i]->setRange(0, 100);
                    _weight_spinboxes[i]->setValue(1);
                    _weight_spinboxes[i]->setPrefix("Weight : ");
                    _weight_spinboxes[i]->setToolTip("Relative chance of this doodad being picked for a placement.");
                    _weight_spinboxes[i]->setMinimumWidth(90);
                    weights_layout->addWidget(_weight_spinboxes[i]);

                    connect(_weight_spinboxes[i], qOverload<int>(&QSpinBox::valueChanged),
                        [this](int) { updateWeightShares(); updateLivePreview(); });
                }
                weights_layout->addStretch();
                settings_layout->addRow(weights_widget);

                // The share of the client's 16-slot spawn table each slot really
                // gets - weights are not straight percentages.
                auto shares_widget = new QWidget(this);
                auto shares_layout = new QHBoxLayout(shares_widget);
                shares_layout->setContentsMargins(0, 0, 0, 0);
                for (int i = 0; i < 4; i++)
                {
                    _weight_share_labels[i] = new QLabel("-", this);
                    _weight_share_labels[i]->setAlignment(Qt::AlignHCenter);
                    _weight_share_labels[i]->setToolTip("The slot's actual share of the client's 16-entry spawn table\
                    \nbuilt from the weights. \"empty\" table entries spawn nothing.");
                    _weight_share_labels[i]->setFixedWidth(_object_list->iconSize().width());
                    shares_layout->addWidget(_weight_share_labels[i]);
                }
                shares_layout->addStretch();
                settings_layout->addRow(shares_widget);

                _preview_renderer = new Tools::PreviewRenderer(_object_list->iconSize().width(),
                    _object_list->iconSize().height(),
                    Noggit::NoggitRenderContext::GROUND_EFFECT_PREVIEW, this);
                _preview_renderer->setVisible(false);
                // Initialize renderer.
                auto const preview_model = std::string("world/wmo/azeroth/buildings/human_farm/farm.wmo");

                try
                {
                  _preview_renderer->setModelOffscreen(preview_model);
                  _preview_renderer->renderToPixmap();
                }
                catch (std::exception const& e)
                {
                  LogError << "GroundEffectsTool preview render failed for " << preview_model
                    << ": " << e.what() << std::endl;
                }
                catch (...)
                {
                  LogError << "GroundEffectsTool preview render failed for " << preview_model
                    << " with unknown exception" << std::endl;
                }

                // Disable this if no active doodad. 
                // Density: 0 → 8. > 24 → 24. This value is for the amount of doodads and on higher values for coverage.
                // Till an amount of around 24 it just increases the amount.After this the doodads begin to group.
                // In WOTLK, only 4 entries out of 25k use more than 20.In retail only 5 use more than 25. 16 or less seems standard
                // TODO : If we end up limiting, a slider could be more apropriate.
                _spinbox_doodads_amount = new QSpinBox(this);
                _spinbox_doodads_amount->setRange(0, 24);
                _spinbox_doodads_amount->setValue(8);
                settings_layout->addRow("Doodads amount : ", _spinbox_doodads_amount);
                _cbbox_terrain_type = new QComboBox(this);
                settings_layout->addRow("Terrain Type", _cbbox_terrain_type);

                for (auto it = gTerrainTypeDB.begin(); it != gTerrainTypeDB.end(); ++it)
                {
                    auto terrain_type_record = *it;

                    _cbbox_terrain_type->addItem(QString(terrain_type_record.getString(TerrainTypeDB::TerrainDesc)),
                        QVariant(terrain_type_record.getUInt(TerrainTypeDB::TerrainId)));
                }

                auto button_save_settings = new QPushButton("Save Set", this);
                settings_layout->addRow(button_save_settings);
                button_save_settings->setBaseSize(button_save_settings->size() / 2.0);

                connect(button_save_settings, &QPushButton::clicked, [this]() { saveSelectedSet(); });
            }


            // Apply group.
            auto apply_group = new QGroupBox("Apply Ground Effect", this);
            right_side_layout->addWidget(apply_group);

            auto apply_layout(new QVBoxLayout(apply_group));
            apply_group->setLayout(apply_layout);

            // Generate modes.
            {
                auto buttons_layout(new QGridLayout(this));
                apply_layout->addLayout(buttons_layout);

                _generate_type_group = new QButtonGroup(apply_group);

                auto generate_effect_zone = new QRadioButton("Current Zone", this);
                generate_effect_zone->setToolTip("Only affects currently LOADED tiles of the zone; undoable.\nCheck \"Include unloaded tiles\" to sweep the whole zone on disk.");
                _generate_type_group->addButton(generate_effect_zone, 0);
                buttons_layout->addWidget(generate_effect_zone, 0, 0);

                auto generate_effect_area = new QRadioButton("Current Area (Subzone)", this);
                generate_effect_area->setToolTip("Only affects currently LOADED tiles of the area; undoable.\nCheck \"Include unloaded tiles\" to sweep the whole area on disk.");
                _generate_type_group->addButton(generate_effect_area, 1);
                buttons_layout->addWidget(generate_effect_area, 0, 1);

                auto generate_effect_adt = new QRadioButton("Current ADT (Tile)", this);
                generate_effect_adt->setToolTip("The tile under the camera; undoable.");
                _generate_type_group->addButton(generate_effect_adt, 2);
                buttons_layout->addWidget(generate_effect_adt, 1, 0);

                auto generate_effect_global = new QRadioButton("Global (Entire Map)", this);
                generate_effect_global->setToolTip("Sweeps all 64x64 ADTs on disk. Changed tiles are written immediately; NOT undoable.");
                _generate_type_group->addButton(generate_effect_global, 3);
                buttons_layout->addWidget(generate_effect_global, 1, 1);

                generate_effect_zone->setChecked(true);
                generate_effect_zone->setAutoExclusive(true);
            }

            _apply_override_cb = new QCheckBox("Override", this);
            _apply_override_cb->setToolTip("If the texture already had a ground effect, replace it.");
            _apply_override_cb->setChecked(true);
            apply_layout->addWidget(_apply_override_cb);

            _scope_disk_sweep_cb = new QCheckBox("Include unloaded tiles (disk sweep)", this);
            _scope_disk_sweep_cb->setToolTip("Zone/Area scope: sweeps every ADT of the map on disk instead of only loaded tiles,\
            \nkeeping chunks whose area matches. Changed tiles are written immediately; NOT undoable.");
            _scope_disk_sweep_cb->setChecked(false);
            apply_layout->addWidget(_scope_disk_sweep_cb);

            // In-world preview of the detail doodads the client would generate.
            {
                auto preview_group = new QGroupBox("Render Detail Doodads", this);
                preview_group->setCheckable(true);
                preview_group->setChecked(_map_view->_draw_ground_effects.get());
                preview_group->setToolTip("Renders the detail doodads exactly where the client will place them.\
                \nThis is the same visibility setting as View > Ground Effects (F5).");
                right_side_layout->addWidget(preview_group);

                auto preview_layout(new QFormLayout(preview_group));
                preview_group->setLayout(preview_layout);

                _live_preview_cb = new QCheckBox("Preview working set on selected texture", this);
                _live_preview_cb->setChecked(true);
                _live_preview_cb->setToolTip("Temporarily renders the values currently shown above wherever the selected texture is active. The ADT and DBC are not changed.");
                preview_layout->addRow(_live_preview_cb);

                auto preview_density_spin = new QSpinBox(this);
                preview_density_spin->setRange(16, 256);
                preview_density_spin->setValue(QSettings().value("groundEffects/previewDensity", 16).toInt());
                preview_density_spin->setToolTip("The client's groundEffectDensity CVar: cell picks per chunk. Client default is 16.");
                preview_layout->addRow("Density : ", preview_density_spin);

                auto preview_distance_spin = new QSpinBox(this);
                preview_distance_spin->setRange(0, 2000);
                preview_distance_spin->setValue(QSettings().value("groundEffects/previewDistance", 140).toInt());
                preview_distance_spin->setToolTip("Draw distance in yards. The client uses groundEffectDist, default 70, max 140.");
                preview_layout->addRow("Draw distance : ", preview_distance_spin);

                _map_view->getWorld()->renderer()->_detail_doodad_density = preview_density_spin->value();
                _map_view->getWorld()->renderer()->_detail_doodad_distance = static_cast<float>(preview_distance_spin->value());
                _map_view->getWorld()->renderer()->_draw_detail_doodads = _map_view->_draw_ground_effects.get();

                connect(preview_group, &QGroupBox::clicked, [this](bool checked)
                    {
                        _map_view->_draw_ground_effects.set(checked);
                    });
                connect(&_map_view->_draw_ground_effects, &Noggit::BoolToggleProperty::changed,
                    preview_group, [this, preview_group](bool checked)
                    {
                        preview_group->setChecked(checked);
                        _map_view->getWorld()->renderer()->_draw_detail_doodads = checked;
                        updateLivePreview();
                    });
                connect(_live_preview_cb, &QCheckBox::toggled, this, [this](bool) { updateLivePreview(); });
                connect(preview_density_spin, qOverload<int>(&QSpinBox::valueChanged), [this](int value)
                    {
                        _map_view->getWorld()->renderer()->_detail_doodad_density = value;
                        QSettings().setValue("groundEffects/previewDensity", value);
                    });
                connect(preview_distance_spin, qOverload<int>(&QSpinBox::valueChanged), [this](int value)
                    {
                        _map_view->getWorld()->renderer()->_detail_doodad_distance = static_cast<float>(value);
                        QSettings().setValue("groundEffects/previewDistance", value);
                    });
                connect(_spinbox_doodads_amount, qOverload<int>(&QSpinBox::valueChanged), this,
                    [this](int) { updateLivePreview(); });
            }

            auto button_generate = new QPushButton("Apply to Texture", this);
            apply_layout->addWidget(button_generate);

            connect(button_generate, &QPushButton::clicked, [this]() { applySelectedSet(); });

            auto button_clear = new QPushButton("Clear Effects", this);
            button_clear->setToolTip("Removes ground-effect assignments at the selected scope using the clear target below.\
            \nThis does not delete GroundEffectTexture or GroundEffectDoodad DBC records.");

            _clear_target_combo = new QComboBox(this);
            _clear_target_combo->addItem("Selected effect ID (all textures)", 0);
            _clear_target_combo->addItem("Selected terrain texture (all IDs)", 1);
            _clear_target_combo->addItem("All effects", 2);
            _clear_target_combo->setToolTip("Selected effect ID removes only the set highlighted in Effect Set Selection.\
            \nSelected terrain texture removes every effect ID assigned to the currently selected terrain texture.\
            \nAll effects clears every texture layer in the selected scope.");
            apply_layout->addWidget(_clear_target_combo);
            apply_layout->addWidget(button_clear);

            connect(button_clear, &QPushButton::clicked, [this]() { clearEffectsAtScope(); });

            // Brush modes.
            {
                _brush_grup_box = new QGroupBox("Brush Mode", this);
                _brush_grup_box->setCheckable(true);
                _brush_grup_box->setChecked(false);
                left_side_layout->addWidget(_brush_grup_box);

                QVBoxLayout* brush_layout = new QVBoxLayout(_brush_grup_box);
                _brush_grup_box->setLayout(brush_layout);

                QHBoxLayout* brush_buttons_layout = new QHBoxLayout(_brush_grup_box);
                brush_layout->addLayout(brush_buttons_layout);
                _brush_type_group = new QButtonGroup(_brush_grup_box);

                _paint_effect = new QRadioButton("Paint Effect", this);
                _brush_type_group->addButton(_paint_effect);
                brush_buttons_layout->addWidget(_paint_effect);
                _paint_exclusion = new QRadioButton("Paint Exclusion", this);
                _brush_type_group->addButton(_paint_exclusion);
                brush_buttons_layout->addWidget(_paint_exclusion);

                _erase_effect = new QRadioButton("Erase Effect", this);
                _erase_effect->setToolTip("Shift+Left-click removes the ground-effect assignment from the selected terrain texture.");
                _brush_type_group->addButton(_erase_effect);
                brush_buttons_layout->addWidget(_erase_effect);

                _erase_exclusion = new QRadioButton("Remove Exclusion", this);
                _erase_exclusion->setToolTip("Shift+Left-click enables detail doodads again in the painted cells.");
                _brush_type_group->addButton(_erase_exclusion);
                brush_buttons_layout->addWidget(_erase_exclusion);

                _paint_effect->setChecked(true);
                _paint_effect->setAutoExclusive(true);

                brush_layout->addWidget(new QLabel("Radius:", _brush_grup_box));
                _effect_radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(_brush_grup_box);
                _effect_radius_slider->setPrefix("");
                _effect_radius_slider->setRange(0, 1000);
                _effect_radius_slider->setDecimals(2);
                _effect_radius_slider->setValue(_texturing_tool->texture_brush().getRadius());
                brush_layout->addWidget(_effect_radius_slider);
            }
            left_side_layout->addSpacerItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

            connect(_render_group_box, &QGroupBox::clicked,
                [this](bool checked)
                {
                    // Checks if it is checked.
                    updateTerrainUniformParams(); 
                });

            connect(_render_active_sets, &QRadioButton::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                });

            connect(_render_placement_map, &QRadioButton::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                });

            connect(_render_exclusion_map, &QRadioButton::clicked,
                [this](bool)
                {
                    updateTerrainUniformParams();
                });

            connect(_brush_grup_box, &QGroupBox::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                    emit brushSettingsChanged();
                });

            connect(_paint_effect, &QRadioButton::clicked,
                [this](bool checked)
                {
                    updateTerrainUniformParams();
                    emit brushSettingsChanged();
                });

            connect(_paint_exclusion, &QRadioButton::clicked,
                 [this](bool checked)
                 {
                     updateTerrainUniformParams();
                     emit brushSettingsChanged();
                 });

            connect(_erase_effect, &QRadioButton::clicked, this, [this](bool)
                {
                    updateTerrainUniformParams();
                    emit brushSettingsChanged();
                });

            connect(_erase_exclusion, &QRadioButton::clicked, this, [this](bool)
                {
                    updateTerrainUniformParams();
                    emit brushSettingsChanged();
                });

            connect(_effect_radius_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged,
                this, [this](double) { emit brushSettingsChanged(); });

            // Get list of ground effect id this texture uses in this ADT.
            connect(button_scan_adt, &QPushButton::clicked
                , [=]()
                {
                    _loaded_effects.clear();
                    scanTileForEffects(TileIndex(_map_view->getCamera()->position));
                    updateSetsList();
                }
            );

            connect(button_scan_adt_loaded, &QPushButton::clicked
                , [=]()
                {
                    _loaded_effects.clear();

                    for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
                    {
                        scanTileForEffects(TileIndex(tile->index));
                    }
                    updateSetsList();
                }
            );

            // appends to whatever is listed instead of replacing scan results
            connect(button_load_all_dbc, &QPushButton::clicked
                , [=]()
                {
                    std::unordered_set<unsigned int> known_ids;
                    known_ids.reserve(_loaded_effects.size());
                    for (auto const& effect : _loaded_effects)
                    {
                        known_ids.insert(effect.ID);
                    }

                    for (auto it = gGroundEffectTextureDB.begin(); it != gGroundEffectTextureDB.end(); ++it)
                    {
                        unsigned int const id = it->getUInt(GroundEffectTextureDB::ID);
                        if (known_ids.contains(id))
                        {
                            continue;
                        }

                        ground_effect_set set;
                        set.load_from_id(id);
                        if (!set.empty())
                        {
                            _loaded_effects.push_back(set);
                        }
                    }
                    updateSetsList();
                }
            );

            QObject::connect(_effect_sets_list, &QListWidget::itemSelectionChanged, [this]()
              {
                    auto effect = getSelectedGroundEffect();
                    if (!effect.has_value())
                    {
                        updateLivePreview();
                        return;
                    }
                    setActiveGroundEffect(effect.value());
                    _last_selected_set_id = effect->ID;
                    saveProjectSetRegistry();
                    updateLivePreview();
                    emit activeEffectChanged(effect->ID, QString::fromStdString(effect->Name));
                });

            // TODO fix this shit
            // for (int i = 0; i < 4; i++)
            // {
            //     connect(_button_effect_doodad[i], &QPushButton::clicked
            //         , [=]()
            //         {
            //             active_doodad_widget = i;
            //             _map_view->getAssetBrowserWidget()->set_browse_mode(Tools::AssetBrowser::asset_browse_mode::detail_doodads);
            //             _map_view->getAssetBrowser()->setVisible(true);
            //         }
            //     );
            // }

            connect(_object_list, &QListWidget::itemClicked, this, [=](QListWidgetItem* item)
                {
                    _map_view->getAssetBrowserWidget()->set_browse_mode(Tools::AssetBrowser::asset_browse_mode::detail_doodads);
                    _map_view->getAssetBrowser()->setVisible(true);
                }
            );

            _object_list->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(_object_list, &QWidget::customContextMenuRequested, this, [=](QPoint const& pos)
                {
                    QListWidgetItem* item = _object_list->itemAt(pos);
                    if (!item)
                        return;

                    QMenu menu(_object_list);
                    QAction* clear_action = menu.addAction("Clear slot");
                    if (menu.exec(_object_list->mapToGlobal(pos)) == clear_action)
                    {
                        item->setText(STRING_EMPTY_DISPLAY);
                        item->setToolTip("");
                        updateDoodadPreviewRender(_object_list->row(item));
                        updateWeightShares();
                        updateLivePreview();
                    }
                }
            );

            using AssetBrowser = Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget;
            connect(map_view->getAssetBrowserWidget(), &AssetBrowser::selectionChanged, this, [=](std::string const& path) {
                if (isVisible()) setDoodadSlotFromBrowser(path.c_str());
                });

            loadBlizzardGroundEffectIds();
            loadProjectSetRegistry();
            updateSetsList();
            updateBlizzardAssignmentsCombo();
        }

        void GroundEffectsTool::updateTerrainUniformParams()
        {
            if (_map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay != render_active_sets_overlay())
            {
                _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay = render_active_sets_overlay();
                _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
            }
            if (_map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay != render_placement_map_overlay())
            {
                _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay = render_placement_map_overlay();
                _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
            }
            if (_map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay != render_exclusion_map_overlay())
            {
                _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay = render_exclusion_map_overlay();
                _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
            }
        }

        void GroundEffectsTool::scanTileForEffects(TileIndex tile_index)
        {
            std::string active_texture = _texturing_tool->_current_texture->filename();

            if (active_texture.empty() || active_texture == "tileset\\generic\\black.blp")
            {
                return;
            }    

            // could use a map to store number of users.
            // std::unordered_set<unsigned int> texture_effect_ids;
            // std::unordered_map<unsigned int, int> texture_effect_ids;

            MapTile* tile(_map_view->getWorld()->mapIndex.getTile(tile_index));
            if (!tile || !tile->finishedLoading())
            {
                return;
            }

            for (int x = 0; x < 16; x++)
            {
                for (int y = 0; y < 16; y++)
                {
                    auto chunk = tile->getChunk(x, y);
                    for (int layer_id = 0; layer_id < chunk->getTextureSet()->num(); layer_id++)
                    {
                        auto texture_name = chunk->getTextureSet()->filename(layer_id);
                        if (texture_name == active_texture)
                        {
                            unsigned int const effect_id = chunk->getTextureSet()->getEffectForLayer(layer_id);

                            if (effect_id && !(effect_id == 0xFFFFFFFF))
                            {
                                ground_effect_set ground_effect;

                                if (_ground_effect_cache.contains(effect_id)) {
                                    ground_effect = _ground_effect_cache.at(effect_id);
                                }
                                else {
                                    ground_effect.load_from_id(effect_id);
                                    _ground_effect_cache[effect_id] = ground_effect;
                                }

                                if (ground_effect.empty())
                                    continue;

                                bool is_duplicate = false;

                                for (int i = 0; i < _loaded_effects.size(); i++)
                                    // for (auto& effect_set : _loaded_effects)
                                {
                                    auto effect_set = &_loaded_effects[i];
                                    // always filter identical ids
                                    if (effect_id == effect_set->ID
                                        || (_chkbox_merge_duplicates->isChecked() && ground_effect == effect_set))
                                    {
                                        is_duplicate = true;
                                        // _duplicate_effects[i].push_back(ground_effect); // mapped by loaded index, could use effect id ?
                                        break;
                                    }
                                }
                                if (!is_duplicate)
                                {
                                    _loaded_effects.push_back(ground_effect);
                                    // give it a name
                                    // Area is probably useless if we merge since duplictes are per area.
                                    _loaded_effects.back().Name += " - " + gAreaDB.getAreaFullName(chunk->getAreaID());
                                }

                                // _texture_effect_ids[effect_id]++;
                            }
                        }
                    }
                }
            }
        }

        void GroundEffectsTool::loadBlizzardGroundEffectIds()
        {
            _blizzard_ground_effect_ids.clear();

            QString const definitions_path = QString::fromStdString(
                Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath)
                + "\\GroundEffectIDs.json";
            QFile file(definitions_path);
            if (!file.open(QIODevice::ReadOnly))
            {
                LogError << "Couldn't load Blizzard ground-effect assignments from "
                    << definitions_path.toStdString() << std::endl;
                return;
            }

            QJsonParseError parse_error;
            QJsonDocument const document = QJsonDocument::fromJson(file.readAll(), &parse_error);
            if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            {
                LogError << "Couldn't parse Blizzard ground-effect assignments from "
                    << definitions_path.toStdString() << ": "
                    << parse_error.errorString().toStdString() << std::endl;
                return;
            }

            QJsonObject const root = document.object();
            for (auto it = root.constBegin(); it != root.constEnd(); ++it)
            {
                if (!it.value().isArray())
                    continue;

                std::vector<unsigned int> effect_ids;
                for (QJsonValue const& value : it.value().toArray())
                {
                    int const effect_id = value.toInt();
                    if (effect_id > 0)
                        effect_ids.push_back(static_cast<unsigned int>(effect_id));
                }
                _blizzard_ground_effect_ids[normalizedTextureName(it.key().toStdString())]
                    = std::move(effect_ids);
            }
        }

        void GroundEffectsTool::updateBlizzardAssignmentsCombo()
        {
            if (!_blizzard_assignments_combo)
                return;

            QSignalBlocker const blocker(_blizzard_assignments_combo);
            _blizzard_assignments_combo->clear();
            _blizzard_assignments_combo->setEnabled(false);

            if (!_texturing_tool->_current_texture)
            {
                _blizzard_assignments_combo->addItem("Choose a Blizzard ground effect...");
                return;
            }

            std::string const texture = normalizedTextureName(
                _texturing_tool->_current_texture->filename());
            auto const assignments = _blizzard_ground_effect_ids.find(texture);
            if (assignments == _blizzard_ground_effect_ids.end() || assignments->second.empty())
            {
                _blizzard_assignments_combo->addItem("No Blizzard assignments found");
                _blizzard_assignments_combo->setToolTip(
                    "No Blizzard ground-effect assignments are listed for this texture.");
                return;
            }

            _blizzard_assignments_combo->addItem("Choose a Blizzard ground effect...");
            bool const modern_features = Noggit::Application::NoggitApplication::instance()
                ->getConfiguration()->modern_features;
            int compatible_assignments = 0;
            for (unsigned int const effect_id : assignments->second)
            {
                if (!modern_features && effect_id > wotlk_max_ground_effect_id)
                    continue;
                if (!gGroundEffectTextureDB.CheckIfIdExists(effect_id))
                    continue;

                ground_effect_set effect;
                effect.load_from_id(effect_id);
                if (effect.empty())
                    continue;

                _blizzard_assignments_combo->addItem(
                    QString::fromStdString(effect.Name), QVariant::fromValue(effect_id));
                ++compatible_assignments;
            }

            if (!compatible_assignments)
            {
                _blizzard_assignments_combo->clear();
                _blizzard_assignments_combo->addItem("No compatible assignments for this client");
                _blizzard_assignments_combo->setToolTip(
                    "Assignments exist for this texture, but none exist in the current client's "
                    "GroundEffectTexture.dbc.");
                return;
            }

            _blizzard_assignments_combo->setEnabled(true);
            _blizzard_assignments_combo->setToolTip(
                "Choose an effect Blizzard used with the current terrain texture. "
                "This only changes the editor selection; it does not apply anything to terrain.");
        }

        void GroundEffectsTool::selectBlizzardGroundEffect(int combo_index)
        {
            if (!_blizzard_assignments_combo || combo_index <= 0)
                return;

            bool valid_effect = false;
            unsigned int const effect_id = _blizzard_assignments_combo->itemData(combo_index)
                .toUInt(&valid_effect);
            if (!valid_effect || !effect_id || !gGroundEffectTextureDB.CheckIfIdExists(effect_id))
                return;

            auto loaded = std::find_if(_loaded_effects.begin(), _loaded_effects.end(),
                [effect_id](ground_effect_set const& effect) { return effect.ID == effect_id; });
            if (loaded == _loaded_effects.end())
            {
                ground_effect_set effect;
                effect.load_from_id(effect_id);
                if (effect.empty())
                {
                    QMessageBox::warning(this, "Blizzard ground effect unavailable",
                        QString("Blizzard ground-effect ID %1 is missing "
                                "from the current GroundEffectTexture.dbc.")
                            .arg(effect_id));
                    return;
                }
                _loaded_effects.push_back(effect);
                updateSetsList();
            }

            int selected_row = -1;
            for (int row = 0; row < static_cast<int>(_loaded_effects.size()); ++row)
            {
                if (_loaded_effects[row].ID == effect_id)
                {
                    selected_row = row;
                    break;
                }
            }
            if (selected_row < 0)
                return;

            QSignalBlocker const blocker(_effect_sets_list);
            _effect_sets_list->setCurrentRow(selected_row);
            setActiveGroundEffect(_loaded_effects[selected_row]);
            _last_selected_set_id = effect_id;
            saveProjectSetRegistry();
            updateLivePreview();
            emit activeEffectChanged(effect_id,
                QString::fromStdString(_loaded_effects[selected_row].Name));
        }

        void GroundEffectsTool::loadProjectSetRegistry()
        {
            _project_set_ids.clear();

            QFile file(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath + "/ground_effect_sets.json"));
            if (!file.open(QIODevice::ReadOnly))
            {
                return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            QJsonObject const root = doc.object();
            _last_selected_set_id = static_cast<unsigned int>(root["selected_set"].toInt());
            for (auto const& value : root["sets"].toArray())
            {
                unsigned int const id = static_cast<unsigned int>(value.toInt());
                if (id)
                {
                    _project_set_ids.push_back(id);
                }
            }
        }

        void GroundEffectsTool::saveProjectSetRegistry()
        {
            QJsonArray sets;
            for (unsigned int id : _project_set_ids)
            {
                sets.append(static_cast<int>(id));
            }
            QJsonObject root;
            root["sets"] = sets;
            root["selected_set"] = static_cast<int>(_last_selected_set_id);

            QFile file(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath + "/ground_effect_sets.json"));
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                file.write(QJsonDocument(root).toJson());
            }
            else
            {
                LogError << "Couldn't write the ground effect set registry to the project folder." << std::endl;
            }
        }

        void GroundEffectsTool::mergeProjectSetsIntoLoaded()
        {
            std::unordered_set<unsigned int> known_ids;
            known_ids.reserve(_loaded_effects.size());
            for (auto const& effect : _loaded_effects)
            {
                known_ids.insert(effect.ID);
            }

            for (unsigned int id : _project_set_ids)
            {
                if (known_ids.contains(id))
                {
                    continue;
                }

                ground_effect_set set;
                set.load_from_id(id);
                if (set.empty()) // record no longer in the dbc
                {
                    continue;
                }
                set.Name += " [saved]";
                _loaded_effects.push_back(set);
                known_ids.insert(id);
            }
        }

        void GroundEffectsTool::updateSetsList()
        {
            // project-saved sets stay listed whether or not a scanned chunk uses them
            mergeProjectSetsIntoLoaded();

            _effect_sets_list->clear();
            genEffectColors();

            int count = 0;
            for (auto& effect_set : _loaded_effects)
            {
                // We already check for id validity earlier
                unsigned int tex_ge_id = effect_set.ID;
                QColor color = QColor::fromRgbF(_effects_colors[count].r, _effects_colors[count].g, _effects_colors[count].b);
                QListWidgetItem* list_item = new QListWidgetItem(effect_set.Name.c_str());
                _effect_sets_list->addItem(list_item);
                list_item->setBackgroundColor(color);
                QPixmap pixmap(_effect_sets_list->iconSize());
                pixmap.fill(color);
                QIcon icon(pixmap);
                list_item->setIcon(icon);
                count++;
            }

            if (_effect_sets_list->count())
            {
                int selected_row = 0;
                if (_last_selected_set_id)
                {
                    for (int row = 0; row < static_cast<int>(_loaded_effects.size()); ++row)
                    {
                        if (_loaded_effects[row].ID == _last_selected_set_id)
                        {
                            selected_row = row;
                            break;
                        }
                    }
                }
                _effect_sets_list->setCurrentRow(selected_row);
                auto effect = getSelectedGroundEffect();
                if (!effect.has_value())
                {
                    return;
                }
                setActiveGroundEffect(effect.value());
            }
        }

        void GroundEffectsTool::genEffectColors()
        {
            _effects_colors.clear();

            for (auto& effect : _loaded_effects)
            {
                // Stable by record id so the legend, newly loaded tiles and
                // undo/redo-driven renderer refreshes all agree.
                float partr, partg, partb;
                float const id = static_cast<float>(effect.ID);
                float r = std::abs(modf(sin(id * 12.9898f + 78.233f) * 43758.5453f, &partr));
                float g = std::abs(modf(sin(id * 11.5591f + 70.233f) * 43569.5451f, &partg));
                float b = std::abs(modf(sin(id * 13.1234f + 76.234f) * 43765.5452f, &partb));
                _effects_colors.push_back(glm::vec3(r, g, b));
            }

            std::string active_texture = _texturing_tool->_current_texture->filename();
            // Check in loop instead to clear data everytime.
            if (active_texture.empty() || active_texture == "tileset\\generic\\black.blp")
            {
                return;
            } 

            for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
            {
                tile->renderer()->setActiveRenderGEffectTexture(active_texture);
                for (int x = 0; x < 16; x++)
                {
                    for (int y = 0; y < 16; y++)
                    {
                        refreshChunkOverlayColor(tile, tile->getChunk(x, y));
                    }
                }
            }
        }

        void GroundEffectsTool::refreshChunkOverlayColor(MapTile* tile, MapChunk* chunk)
        {
            int chunk_index = chunk->px * 16 + chunk->py;

            // reset to black by default
            tile->renderer()->setChunkGroundEffectColor(chunk_index, glm::vec3(0.0, 0.0, 0.0));

            // ! Set the chunk active layer data.
            // new system : just update the active texture and mark dirty to the renderer
            // tile->renderer()->setChunkGroundEffectActiveData(chunk);

            std::string active_texture = _texturing_tool->_current_texture->filename();
            if (active_texture.empty() || active_texture == "tileset\\generic\\black.blp" || _loaded_effects.empty())
                return;

            for (int layer_id = 0; layer_id < chunk->getTextureSet()->num(); layer_id++)
            {
                auto texture_name = chunk->getTextureSet()->filename(layer_id);

                if (texture_name == active_texture)
                {
                    unsigned int const effect_id = chunk->getTextureSet()->getEffectForLayer(layer_id);

                    if (effect_id && !(effect_id == 0xFFFFFFFF))
                    {
                        ground_effect_set ground_effect;

                        if (_ground_effect_cache.contains(effect_id)) {
                            ground_effect = _ground_effect_cache.at(effect_id);
                        }
                        else {
                            ground_effect.load_from_id(effect_id);
                            _ground_effect_cache[effect_id] = ground_effect;
                        }

                        int count = -1;
                        bool found_debug = false;
                        for (auto& effect_set : _loaded_effects)
                        {
                            count++;
                            if (effect_id == effect_set.ID)
                            {
                                tile->renderer()->setChunkGroundEffectColor(chunk_index, _effects_colors[count]);
                                found_debug = true;
                                break;
                            }
                            if (_chkbox_merge_duplicates->isChecked() && (ground_effect == &effect_set)) // do deep comparison, find those that have the same effect as loaded effects, but diff id.
                            {
                                if (ground_effect.empty())
                                    continue;
                                // same color
                                tile->renderer()->setChunkGroundEffectColor(chunk_index, _effects_colors[count]);
                                found_debug = true;
                                break;
                            }
                        }
                        // in case some chunks couldn't be resolved, paint them in pure red
                        if (!found_debug)
                            tile->renderer()->setChunkGroundEffectColor(chunk_index, glm::vec3(1.0, 0.0, 0.0));
                    }
                    break;
                }
            }
        }

        void GroundEffectsTool::refreshOverlayForChunksInRange(glm::vec3 const& pos, float radius)
        {
            _map_view->getWorld()->for_all_chunks_in_range(pos, radius
                , [&](MapChunk* chunk)
                {
                    refreshChunkOverlayColor(chunk->mt, chunk);
                    return false;
                });
        }

        void GroundEffectsTool::TextureChanged()
        {
            _loaded_effects.clear();
            _ground_effect_cache.clear();

            // Establish empty-editor defaults before selecting a scanned set.
            // updateSetsList() then loads the selected record's real values.
            _spinbox_doodads_amount->setValue(8);
            _cbbox_terrain_type->setCurrentIndex(0);
            for (int i = 0; i < 4; i++)
            {
                _weight_spinboxes[i]->setValue(1);
                updateDoodadPreviewRender(i);
            }

            // Keep the set list in sync with what the user can currently see.
            // Project sets are merged by updateSetsList().
            for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
            {
                if (tile && tile->finishedLoading())
                {
                    scanTileForEffects(tile->index);
                }
            }
            updateSetsList();
            updateBlizzardAssignmentsCombo();

            updateWeightShares();
            updateLivePreview();
        }

        bool GroundEffectsTool::render_active_sets_overlay() const
        {
          return _texturing_tool->getTexturingMode() == texturing_mode::ground_effect
            && !render_exclusion_map_overlay() && _render_active_sets->isChecked() && render_mode();
        }

        bool GroundEffectsTool::render_placement_map_overlay() const
        {
          return _texturing_tool->getTexturingMode() == texturing_mode::ground_effect
            && !render_exclusion_map_overlay() && _render_placement_map->isChecked() && render_mode();
        }

        bool GroundEffectsTool::render_exclusion_map_overlay() const
        {
          return _texturing_tool->getTexturingMode() == texturing_mode::ground_effect && render_mode()
            && (_render_exclusion_map->isChecked()
                || brush_mode() == ground_effect_brush_mode::exclusion
                || brush_mode() == ground_effect_brush_mode::erase_exclusion);
        }

        void GroundEffectsTool::change_radius(float change)
        {
          _effect_radius_slider->setValue(static_cast<float>(_effect_radius_slider->value()) + change);
        }


        //Close event triggers, hide event.
        void GroundEffectsTool::hideEvent(QHideEvent* event)
        {
          if (_map_view->_world)
          {
            _map_view->getWorld()->renderer()->clearDetailDoodadPreview();
            _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay = false;
            _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay = false;
            _map_view->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay = false;
            _map_view->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          }

          QWidget::hideEvent(event);
        }

        void GroundEffectsTool::setDoodadSlotFromBrowser(QString doodad_path)
        {
            // the dbc stores a bare filename that the client resolves under
            // world/nodxt/detail/ (hardcoded prefix) - a model anywhere else
            // could never render, in noggit or in game
            QString normalized = doodad_path;
            normalized.replace('\\', '/');
            if (!normalized.startsWith("world/nodxt/detail/", Qt::CaseInsensitive))
            {
                QMessageBox::warning(this, "Ground Effect Doodad"
                    , "Detail doodads must be models under world\\nodxt\\detail\\.\nThe client resolves the dbc filename relative to that folder, so this model would never render.");
                return;
            }

            const QFileInfo info(doodad_path);
            const QString filename(info.fileName());

            if (_object_list->currentItem())
                _object_list->currentItem()->setText(filename);

            updateDoodadPreviewRender(_object_list->currentRow());
            updateWeightShares();
            updateLivePreview();
        }

        void GroundEffectsTool::updateDoodadPreviewRender(int slot_index)
        {
            QListWidgetItem* list_item = _object_list->item(slot_index);
            if (!list_item) // item(-1) when nothing is selected
            {
                return;
            }

            QString filename = list_item->text();

            if (filename.isEmpty() || filename == STRING_EMPTY_DISPLAY)
            {
                list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
            }
            else
            {
                // Load preview render. A few stock records reference models that
                // no longer ship (dead Tirisfal/Silverpine/Arathi entries) - a
                // failed render must not abort filling the other slots
                QString filepath(("world/nodxt/detail/" + filename.toStdString()).c_str());
                try
                {
                    _preview_renderer->setModelOffscreen(filepath.toStdString());
                    list_item->setIcon(*_preview_renderer->renderToPixmap());
                    list_item->setToolTip(filepath);
                }
                catch (...)
                {
                    list_item->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::exclamationtriangle));
                    list_item->setToolTip(filepath + " (preview render failed)");
                    LogError << "Ground effect doodad preview render failed for "
                        << filepath.toStdString() << std::endl;
                }
            }
        }

        GroundEffectsTool::~GroundEffectsTool()
        {
            unload();
        }

        void GroundEffectsTool::unload()
        {
          if (!_preview_renderer)
          {
            return;
          }

          _preview_renderer->unloadOpenglData();
          delete _preview_renderer;
          _preview_renderer = nullptr;
        }

        float GroundEffectsTool::radius() const
        {
          return _effect_radius_slider->value();
        }

        ground_effect_brush_mode GroundEffectsTool::brush_mode() const
        {
            if (!_brush_grup_box->isChecked())
            {
                return ground_effect_brush_mode::none;
            }
            else if (_paint_effect->isChecked())
            {
                return ground_effect_brush_mode::effect;
            }
            else if (_paint_exclusion->isChecked())
            {
                return ground_effect_brush_mode::exclusion;
            }
            else if (_erase_effect->isChecked())
            {
                return ground_effect_brush_mode::erase_effect;
            }
            else if (_erase_exclusion->isChecked())
            {
                return ground_effect_brush_mode::erase_exclusion;
            }
            return ground_effect_brush_mode::none;
        }

        void GroundEffectsTool::set_brush_mode(ground_effect_brush_mode mode)
        {
            _brush_grup_box->setChecked(mode != ground_effect_brush_mode::none);
            switch (mode)
            {
            case ground_effect_brush_mode::effect: _paint_effect->setChecked(true); break;
            case ground_effect_brush_mode::exclusion: _paint_exclusion->setChecked(true); break;
            case ground_effect_brush_mode::erase_effect: _erase_effect->setChecked(true); break;
            case ground_effect_brush_mode::erase_exclusion: _erase_exclusion->setChecked(true); break;
            case ground_effect_brush_mode::none: break;
            }
            updateTerrainUniformParams();
            emit brushSettingsChanged();
        }

        bool GroundEffectsTool::render_mode() const
        {
          return _render_group_box->isChecked();
        }

        void GroundEffectsTool::delete_renderer()
        {
          // unload() nulls the pointer so the destructor doesn't free it twice
          unload();
        }

        void GroundEffectsTool::showEvent(QShowEvent* event)
        {
          QWidget::showEvent(event);
          updateTerrainUniformParams();
          updateLivePreview();
        }

        std::optional<ground_effect_set> GroundEffectsTool::getSelectedGroundEffect()
        {
            int index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || !_effect_sets_list->count() || index == -1)
            {
                return std::nullopt;
            }
            auto effect = _loaded_effects[index];
            return effect;
        }

        std::optional<unsigned int> GroundEffectsTool::sampleEffectAt(glm::vec3 const& pos)
        {
            MapChunk* chunk = _map_view->getWorld()->getChunkAt(pos);
            if (!chunk || !chunk->getTextureSet() || !chunk->getTextureSet()->num())
            {
                return std::nullopt;
            }

            TextureSet* texture_set = chunk->getTextureSet();
            if (chunk->doodadMappingNeedsUpdate())
            {
                texture_set->updateDoodadMapping();
                chunk->clearDoodadMappingNeedsUpdate();
            }

            unsigned int const column = static_cast<unsigned int>(std::clamp(
                static_cast<int>(std::floor((pos.x - chunk->xbase) / UNITSIZE)), 0, 7));
            unsigned int const row = static_cast<unsigned int>(std::clamp(
                static_cast<int>(std::floor((pos.z - chunk->zbase) / UNITSIZE)), 0, 7));
            unsigned int const layer = texture_set->getDoodadActiveLayerIdAt(column, row);
            if (layer >= texture_set->num())
            {
                return std::nullopt;
            }

            unsigned int const effect_id = texture_set->getEffectForLayer(layer);
            if (!effect_id || effect_id == 0xFFFFFFFF || !gGroundEffectTextureDB.CheckIfIdExists(effect_id))
            {
                return std::nullopt;
            }

            auto found = std::find_if(_loaded_effects.begin(), _loaded_effects.end(),
                [effect_id](ground_effect_set const& effect) { return effect.ID == effect_id; });
            if (found == _loaded_effects.end())
            {
                ground_effect_set effect;
                effect.load_from_id(effect_id);
                if (effect.empty())
                {
                    return std::nullopt;
                }
                _loaded_effects.push_back(effect);
                updateSetsList();
            }

            for (int i = 0; i < static_cast<int>(_loaded_effects.size()); ++i)
            {
                if (_loaded_effects[i].ID == effect_id)
                {
                    _effect_sets_list->setCurrentRow(i);
                    setActiveGroundEffect(_loaded_effects[i]);
                    emit activeEffectChanged(effect_id, QString::fromStdString(_loaded_effects[i].Name));
                    return effect_id;
                }
            }
            return std::nullopt;
        }

        std::optional<glm::vec3> GroundEffectsTool::getSelectedEffectColor()
        {
            int index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || !_effect_sets_list->count() || index == -1)
            {
                return std::nullopt;
            }
            glm::vec3 effect_color = _effects_colors[index];
            return effect_color;
        }

        namespace
        {
            // shared tail of every disk-sweep confirmation prompt
            constexpr char const* STRING_DISK_SWEEP_WARNING = "Affected ADTs are written to disk immediately and this cannot be undone.";

            QString normalizedDoodadFilename(QString name)
            {
                name = name.toLower();
                name.replace(".mdx", ".m2");
                name.replace(".mdl", ".m2");
                return name;
            }

            // Returns the id of the GroundEffectDoodad record for this filename,
            // creating the record if no existing one matches.
            unsigned int findOrCreateGroundEffectDoodad(std::string const& filename)
            {
                QString const wanted = normalizedDoodadFilename(QString(filename.c_str()));

                for (auto it = gGroundEffectDoodadDB.begin(); it != gGroundEffectDoodadDB.end(); ++it)
                {
                    if (normalizedDoodadFilename(QString(it->getString(GroundEffectDoodadDB::Filename))) == wanted)
                    {
                        return it->getUInt(GroundEffectDoodadDB::ID);
                    }
                }

                int const new_id = gGroundEffectDoodadDB.getEmptyRecordID();
                auto record = gGroundEffectDoodadDB.addRecord(new_id);
                record.writeString(GroundEffectDoodadDB::Filename, filename);
                return static_cast<unsigned int>(new_id);
            }
        }

        void GroundEffectsTool::createNewSet()
        {
            ground_effect_set new_set;
            new_set.Name = "New Set (unsaved)";
            new_set.Amount = 8;
            _loaded_effects.push_back(new_set);
            updateSetsList();
            _effect_sets_list->setCurrentRow(_effect_sets_list->count() - 1);
        }

        void GroundEffectsTool::duplicateSelectedSet()
        {
            int const index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || index < 0 || index >= static_cast<int>(_loaded_effects.size()))
            {
                QMessageBox::information(this, "Duplicate Ground Effect Set", "Select a set to duplicate first.");
                return;
            }

            ground_effect_set copy = _loaded_effects[index];
            copy.Name = (copy.ID ? std::to_string(copy.ID) : copy.Name) + " copy (unsaved)";
            copy.ID = 0;
            _loaded_effects.push_back(copy);
            updateSetsList();
            _effect_sets_list->setCurrentRow(_effect_sets_list->count() - 1);
        }

        void GroundEffectsTool::deleteSelectedSet()
        {
            int const index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || index < 0 || index >= static_cast<int>(_loaded_effects.size()))
            {
                QMessageBox::information(this, "Delete Ground Effect Set", "Select a set to delete first.");
                return;
            }

            ground_effect_set const& set = _loaded_effects[index];

            if (set.ID)
            {
                if (QMessageBox::question(this
                    , "Delete Ground Effect Set"
                    , QString("Delete GroundEffectTexture record %1 from the dbc?\nEvery map using this id loses its effect (chunks referencing it spawn nothing).\nThis cannot be undone.").arg(set.ID)
                    , QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                {
                    return;
                }

                gGroundEffectTextureDB.removeRecord(set.ID);
                gGroundEffectTextureDB.save();

                // cached per-chunk placements rebuild without the record
                DetailDoodads::bumpDbcStamp();

                // the overlay must stop resolving the deleted id
                _ground_effect_cache.erase(set.ID);

                auto it = std::find(_project_set_ids.begin(), _project_set_ids.end(), set.ID);
                if (it != _project_set_ids.end())
                {
                    _project_set_ids.erase(it);
                    if (_last_selected_set_id == set.ID)
                    {
                        _last_selected_set_id = 0;
                    }
                    saveProjectSetRegistry();
                }

                // the shared GroundEffectDoodad records stay: other sets may use them
            }

            _loaded_effects.erase(_loaded_effects.begin() + index);
            updateSetsList();
            updateLivePreview();
        }

        void GroundEffectsTool::updateWeightShares()
        {
            // spinbox valueChanged fires during construction, before the labels exist
            if (!_weight_share_labels[3])
            {
                return;
            }

            // replicates the client's stride-13, 16-slot spawn table build (see
            // DetailDoodads.cpp): later writes win, the padding cycles all id
            // slots including empty ones
            int spawn_table[16];
            std::fill(spawn_table, spawn_table + 16, -1);

            int write_pos = 0;
            int total = 0;
            for (int k = 0; k < 4; ++k)
            {
                int weight = static_cast<int>(_weight_spinboxes[k]->value());
                if (weight <= 0)
                {
                    continue;
                }
                total += weight;
                while (weight--)
                {
                    spawn_table[write_pos & 15] = k;
                    write_pos += 13;
                }
            }
            while (total < 16)
            {
                spawn_table[write_pos & 15] = total & 3;
                ++total;
                write_pos += 13;
            }

            int counts[4] = { 0, 0, 0, 0 };
            for (int s = 0; s < 16; ++s)
            {
                if (spawn_table[s] >= 0)
                {
                    counts[spawn_table[s]]++;
                }
            }

            for (int k = 0; k < 4; ++k)
            {
                QListWidgetItem* item = _object_list->item(k);
                bool const has_doodad = item && !item->text().isEmpty() && item->text() != STRING_EMPTY_DISPLAY;
                int const percent = counts[k] * 100 / 16;

                if (!counts[k])
                {
                    _weight_share_labels[k]->setText("0%");
                }
                else if (has_doodad)
                {
                    _weight_share_labels[k]->setText(QString::number(percent) + "%");
                }
                else
                {
                    // table entries pointing at an empty slot spawn nothing
                    _weight_share_labels[k]->setText(QString::number(percent) + "% empty");
                }
            }
        }

        void GroundEffectsTool::updateLivePreview()
        {
            if (!_map_view || !_map_view->_world || !_live_preview_cb
                || !_live_preview_cb->isChecked() || !isVisible())
            {
                if (_map_view && _map_view->_world)
                {
                    _map_view->getWorld()->renderer()->clearDetailDoodadPreview();
                }
                return;
            }

            std::string const texture = _texturing_tool->_current_texture->filename();
            if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
            {
                _map_view->getWorld()->renderer()->clearDetailDoodadPreview();
                return;
            }

            DetailDoodadPreview preview;
            preview.enabled = true;
            preview.texture = texture;
            preview.amount = static_cast<std::uint32_t>(_spinbox_doodads_amount->value());
            for (int i = 0; i < 4; ++i)
            {
                QString const filename = _object_list->item(i)->text();
                if (!filename.isEmpty() && filename != STRING_EMPTY_DISPLAY)
                {
                    preview.filenames[i] = filename.toStdString();
                }
                preview.weights[i] = _weight_spinboxes[i]->value();
            }

            _map_view->getWorld()->renderer()->setDetailDoodadPreview(std::move(preview));
        }

        void GroundEffectsTool::saveSelectedSet()
        {
            int const index = _effect_sets_list->currentIndex().row();
            if (_loaded_effects.empty() || index < 0 || index >= static_cast<int>(_loaded_effects.size()))
            {
                QMessageBox::information(this, "Save Ground Effect Set", "Select or create a set first.");
                return;
            }

            std::string filenames[4];
            bool any_doodad = false;
            for (int i = 0; i < 4; ++i)
            {
                QString const text = _object_list->item(i)->text();
                if (text.isEmpty() || text == STRING_EMPTY_DISPLAY)
                {
                    continue;
                }
                filenames[i] = text.toStdString();
                any_doodad = true;
            }

            if (!any_doodad && QMessageBox::question(this, "Save Ground Effect Set",
                "This set has no doodads. Save it as a terrain-type-only effect?",
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            {
                return;
            }

            int total_weight = 0;
            for (auto* spinbox : _weight_spinboxes)
            {
                total_weight += spinbox->value();
            }
            if (total_weight > 16 && QMessageBox::question(this, "Save Ground Effect Set",
                QString("The weights total %1, but the client has only 16 selection slots. Later writes overwrite earlier ones, so the displayed effective shares—not the raw weights—are what will spawn. Save anyway?").arg(total_weight),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            {
                return;
            }

            ground_effect_set& set = _loaded_effects[index];

            QDialog save_dialog(this);
            save_dialog.setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
            save_dialog.setWindowTitle("Save Ground Effect Set");
            auto dialog_layout = new QVBoxLayout(&save_dialog);
            dialog_layout->addWidget(new QLabel("GroundEffectTexture Id : ", &save_dialog));
            auto id_spinbox = new QSpinBox(&save_dialog);
            id_spinbox->setRange(1, std::numeric_limits<int>::max());
            id_spinbox->setValue(set.ID ? static_cast<int>(set.ID) : gGroundEffectTextureDB.getEmptyRecordID());
            dialog_layout->addWidget(id_spinbox);
            dialog_layout->addWidget(new QLabel("Saving to an id that already exists overwrites that record\nfor every map using it.", &save_dialog));
            auto save_button = new QPushButton("Save", &save_dialog);
            dialog_layout->addWidget(save_button);
            connect(save_button, &QPushButton::clicked, &save_dialog, &QDialog::accept);

            if (save_dialog.exec() != QDialog::Accepted)
            {
                return;
            }

            unsigned int const record_id = static_cast<unsigned int>(id_spinbox->value());

            unsigned int doodad_ids[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < 4; ++i)
            {
                if (!filenames[i].empty())
                {
                    doodad_ids[i] = findOrCreateGroundEffectDoodad(filenames[i]);
                }
            }

            DBCFile::Record record = gGroundEffectTextureDB.CheckIfIdExists(record_id)
                ? gGroundEffectTextureDB.getByID(record_id)
                : gGroundEffectTextureDB.addRecord(record_id);

            for (int i = 0; i < 4; ++i)
            {
                record.write(GroundEffectTextureDB::Doodads + i, doodad_ids[i]);
                record.write(GroundEffectTextureDB::Weights + i, static_cast<unsigned int>(_weight_spinboxes[i]->value()));
            }
            record.write(GroundEffectTextureDB::Amount, static_cast<unsigned int>(_spinbox_doodads_amount->value()));
            record.write(GroundEffectTextureDB::TerrainType, _cbbox_terrain_type->currentData().toUInt());

            gGroundEffectDoodadDB.save();
            gGroundEffectTextureDB.save();

            // cached per-chunk placements rebuild against the new records
            DetailDoodads::bumpDbcStamp();

            _last_selected_set_id = record_id;
            if (std::find(_project_set_ids.begin(), _project_set_ids.end(), record_id) == _project_set_ids.end())
            {
                _project_set_ids.push_back(record_id);
            }
            saveProjectSetRegistry();

            set.ID = record_id;
            set.Amount = _spinbox_doodads_amount->value();
            set.TerrainType = _cbbox_terrain_type->currentData().toUInt();
            for (int i = 0; i < 4; ++i)
            {
                set.Doodads[i].ID = doodad_ids[i];
                set.Doodads[i].filename = filenames[i];
                set.Weights[i] = _weight_spinboxes[i]->value();
            }
            set.rebuild_name();

            _ground_effect_cache[record_id] = set;

            updateSetsList();
            _effect_sets_list->setCurrentRow(index);
        }

        void GroundEffectsTool::applySelectedSet()
        {
            auto effect = getSelectedGroundEffect();
            if (!effect.has_value() || !effect->ID)
            {
                QMessageBox::information(this, "Apply Ground Effect", "Select a saved set first (unsaved sets have no id to apply).");
                return;
            }

            std::string const texture = _texturing_tool->_current_texture->filename();
            if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
            {
                QMessageBox::information(this, "Apply Ground Effect", "Select a texture first.");
                return;
            }

            applyEffectIdAtScope(texture, effect->ID, _apply_override_cb->isChecked()
                , QString("Apply this effect to the texture on every ADT of the map?\n") + STRING_DISK_SWEEP_WARNING);
        }

        void GroundEffectsTool::clearEffectsAtScope()
        {
            std::string texture;
            std::optional<unsigned int> only_effect_id;
            QString confirmation;

            switch (_clear_target_combo->currentData().toInt())
            {
            case 0: // selected effect id, regardless of texture
            {
                auto const effect = getSelectedGroundEffect();
                if (!effect.has_value() || !effect->ID)
                {
                    QMessageBox::information(this, "Clear Ground Effects", "Select a saved effect set first.");
                    return;
                }
                only_effect_id = effect->ID;
                confirmation = QString("Remove only ground effect ID %1 from every ADT of the map?\n").arg(effect->ID)
                             + STRING_DISK_SWEEP_WARNING;
                break;
            }
            case 1: // selected terrain texture, regardless of effect id
                texture = _texturing_tool->_current_texture->filename();
                if (texture.empty() || texture == STRING_EMPTY_TEXTURE)
                {
                    QMessageBox::information(this, "Clear Ground Effects", "Select a terrain texture first.");
                    return;
                }
                confirmation = QString("Remove every ground effect assigned to texture\n%1\nfrom every ADT of the map?\n")
                             .arg(QString::fromStdString(texture)) + STRING_DISK_SWEEP_WARNING;
                break;
            case 2: // every effect on every texture
                confirmation = QString("Remove ALL ground effects from every ADT of the map?\n") + STRING_DISK_SWEEP_WARNING;
                break;
            default:
                return;
            }

            applyEffectIdAtScope(texture, 0, true, confirmation, only_effect_id);
        }

        void GroundEffectsTool::applyEffectIdAtScope(std::string const& texture, unsigned int effect_id,
                                                     bool override_existing, QString const& global_confirm,
                                                     std::optional<unsigned int> only_effect_id)
        {
            World* world = _map_view->getWorld();
            glm::vec3 const camera_pos = _map_view->getCamera()->position;

            switch (_generate_type_group->checkedId())
            {
            case 0: // current zone
            case 1: // current area
            {
                bool const whole_zone = _generate_type_group->checkedId() == 0;

                auto area_id = world->for_maybe_chunk_at(camera_pos, [](MapChunk* chunk) { return chunk->getAreaID(); });
                if (!area_id.has_value())
                {
                    QMessageBox::information(this, "Apply Ground Effect", "The camera is not over a loaded tile.");
                    return;
                }

                int target_area = area_id.value();
                if (whole_zone)
                {
                    target_area = AreaDB::resolve_zone_id(target_area);
                }

                if (_scope_disk_sweep_cb->isChecked())
                {
                    QString const area_name(gAreaDB.getAreaFullName(target_area).c_str());
                    if (QMessageBox::question(this
                        , effect_id ? "Apply ground effect to area on disk" : "Clear ground effects from area on disk"
                        , QString("%1 \"%2\" (area %3) across every ADT of the map?\n%4")
                            .arg(effect_id ? "Apply the effect to" : "Clear ground effects from").arg(area_name).arg(target_area).arg(STRING_DISK_SWEEP_WARNING)
                        , QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                    {
                        return;
                    }
                    QProgressDialog progress(effect_id ? "Applying ground effect across ADTs..." : "Clearing ground effects across ADTs...",
                        "Cancel", 0, 64 * 64, this);
                    progress.setWindowModality(Qt::WindowModal);
                    progress.setMinimumDuration(0);
                    world->applyGroundEffectGlobal(texture, effect_id, override_existing,
                                                   target_area, whole_zone, &progress, only_effect_id);
                }
                else
                {
                    NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_LAYERINFO);
                    world->applyGroundEffectToArea(target_area, whole_zone, texture, effect_id,
                                                   override_existing, only_effect_id);
                    NOGGIT_ACTION_MGR->endAction();
                }
                break;
            }
            case 2: // current tile
            {
                NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_LAYERINFO);
                world->applyGroundEffectToTileAt(camera_pos, texture, effect_id,
                                                 override_existing, only_effect_id);
                NOGGIT_ACTION_MGR->endAction();
                break;
            }
            case 3: // global
            {
                if (QMessageBox::question(this
                    , effect_id ? "Apply ground effect globally" : "Clear ground effects globally"
                    , global_confirm
                    , QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                {
                    return;
                }
                QProgressDialog progress(effect_id ? "Applying ground effect across ADTs..." : "Clearing ground effects across ADTs...",
                    "Cancel", 0, 64 * 64, this);
                progress.setWindowModality(Qt::WindowModal);
                progress.setMinimumDuration(0);
                world->applyGroundEffectGlobal(texture, effect_id, override_existing,
                                               -1, false, &progress, only_effect_id);
                break;
            }
            default:
                return;
            }

            genEffectColors();
        }

        void GroundEffectsTool::setActiveGroundEffect(ground_effect_set const& effect)
        {
            // Sets a ground effect to be actively selected in the UI.
            _spinbox_doodads_amount->setValue(effect.Amount);
            int const terrain_type_index = _cbbox_terrain_type->findData(QVariant(effect.TerrainType));
            _cbbox_terrain_type->setCurrentIndex(terrain_type_index >= 0 ? terrain_type_index : 0);

            for (int i = 0; i < 4; ++i)
            {
                _weight_spinboxes[i]->setValue(effect.Weights[i]);
                QString filename(effect.Doodads[i].filename.c_str());
                // Replace old extensions in the DBC.
                filename = filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
                filename = filename.replace(".mdl", ".m2", Qt::CaseInsensitive);

                // TODO turn this into an array of elements.
                if (filename.isEmpty())
                {
                    _object_list->item(i)->setText(STRING_EMPTY_DISPLAY);
                }

                else
                {
                    _object_list->item(i)->setText(filename);
                }
                updateDoodadPreviewRender(i);
            }

            updateWeightShares();
        }

        void ground_effect_set::load_from_id(unsigned int effect_id)
        {
            if (!effect_id || (effect_id == 0xFFFFFFFF))
            {
                return;
            }
                
            if (!gGroundEffectTextureDB.CheckIfIdExists(effect_id))
            {
                return;
            }

            try
            {
                DBCFile::Record GErecord = gGroundEffectTextureDB.getByID(effect_id);
                ID = GErecord.getUInt(GroundEffectTextureDB::ID);
                Amount = GErecord.getUInt(GroundEffectTextureDB::Amount);
                TerrainType = GErecord.getUInt(GroundEffectTextureDB::TerrainType);

                for (int i = 0; i < 4; ++i)
                {
                    Weights[i] = GErecord.getUInt(GroundEffectTextureDB::Weights + i);
                    unsigned const curDoodadId
                    { 
                        GErecord.getUInt(GroundEffectTextureDB::Doodads + i) 
                    };

                    if (!curDoodadId)
                    {
                        continue;
                    }
                    
                    if (!gGroundEffectDoodadDB.CheckIfIdExists(curDoodadId))
                    {
                        continue;
                    }
                        
                    Doodads[i].ID = curDoodadId;
                    QString filename = gGroundEffectDoodadDB.getByID(curDoodadId).getString(GroundEffectDoodadDB::Filename);

                    filename.replace(".mdx", ".m2", Qt::CaseInsensitive);
                    filename.replace(".mdl", ".m2", Qt::CaseInsensitive);

                    Doodads[i].filename = filename.toStdString();
                }

                rebuild_name();
            }
            catch (GroundEffectTextureDB::NotFound)
            {
                ID = 0;
                LogError << "Couldn't find ground effect Id : " << effect_id << "in GroundEffectTexture.dbc" << std::endl;
            }
        }

        void ground_effect_set::rebuild_name()
        {
            std::string doodad_names;
            int doodad_count = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (Doodads[i].filename.empty())
                {
                    continue;
                }
                doodad_count++;
                if (doodad_count <= 2)
                {
                    std::string stem = Doodads[i].filename;
                    size_t const dot = stem.find_last_of('.');
                    if (dot != std::string::npos)
                    {
                        stem.resize(dot);
                    }
                    if (!doodad_names.empty())
                    {
                        doodad_names += ", ";
                    }
                    doodad_names += stem;
                }
            }

            Name = std::to_string(ID);
            if (doodad_count)
            {
                Name += " - " + doodad_names;
                if (doodad_count > 2)
                {
                    Name += " +" + std::to_string(doodad_count - 2);
                }
            }
            else
            {
                Name += " - no doodads";
            }
        }

        bool ground_effect_set::empty() const
        {
          return !ID;
        }

        bool ground_effect_set::operator== (ground_effect_set* effect2)
        {
          return (TerrainType == effect2->TerrainType && Amount == effect2->Amount
            && Doodads[0] == &effect2->Doodads[0] && Doodads[1] == &effect2->Doodads[1]
            && Doodads[2] == &effect2->Doodads[2] && Doodads[3] == &effect2->Doodads[3]
            && Weights[0] == effect2->Weights[0] && Weights[1] == effect2->Weights[1]
            && Weights[2] == effect2->Weights[2] && Weights[3] == effect2->Weights[3]
            );
        }

        bool ground_effect_doodad::empty() const
        {
          return filename.empty();
        }

        bool ground_effect_doodad::operator== (ground_effect_doodad* doodad2)
        {
          return filename == doodad2->filename;
        }
}
}
