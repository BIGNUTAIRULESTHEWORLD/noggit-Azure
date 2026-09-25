#pragma once

#include <noggit/NpcAppearance.hpp>
#include <noggit/ContextObject.hpp>

#include <QWidget>
#include <QString>
#include <QPoint>

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DBCFile;
class MapView;
class Model;
struct blp_texture;
struct scoped_model_reference;
struct scoped_blp_texture_reference;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QEvent;
class QGroupBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QSpinBox;
class QSqlDatabase;
class QTimer;
class QListView;
class QToolButton;

namespace BlizzardArchive { class ClientData; }

namespace Noggit::Ui::Tools::AssetBrowser { class ModelViewer; }

namespace Noggit::Ui
{
  class NpcThumbnailRenderer;
  class NpcFactionSelector;
  class NpcTemplateTableModel;
  class NpcTemplateFilterModel;
  class NpcPageFilterModel;

  // Browses creature_template without modifying it. Clicking loaded terrain
  // with a selected NPC creates one spawn in the world database.
  class NpcTemplateBrowser : public QWidget
  {
  public:
    explicit NpcTemplateBrowser(std::shared_ptr<BlizzardArchive::ClientData> client_data,
                                MapView* map_view,
                                QWidget* parent = nullptr);
    ~NpcTemplateBrowser() override;

    QWidget* propertiesPanel() const { return _properties_panel; }
    void openEditorFromToolbar();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    static constexpr int EmoteAnimationRole = Qt::UserRole + 1;
    static constexpr int EmoteProcedureRole = Qt::UserRole + 2;
    static constexpr int EmoteParameterRole = Qt::UserRole + 3;

    void loadTemplates();
    void updateFilters();
    void updateBrowserPage();
    void renderVisibleThumbnails();
    void updateDetails();
    bool loadDisplay(unsigned display_id, bool begin_placement, QString* failure = nullptr,
                     Tools::AssetBrowser::ModelViewer* target_preview = nullptr,
                     float* target_scale = nullptr, bool* pending = nullptr);
    std::string npcModelPath(unsigned display_id) const;
    Model* prefetchNpcModel(std::string const& path, Noggit::NoggitRenderContext context);
    blp_texture* prefetchNpcTexture(std::string const& path,
                                    Noggit::NoggitRenderContext context);
    std::pair<unsigned, unsigned> itemVisual(unsigned item_entry,
                                              unsigned database_display = 0,
                                              unsigned database_inventory_type = 0) const;
    unsigned rangedHandAttachment(unsigned item_entry,
                                  unsigned database_subclass = 0) const;
    void startPlacement();
    void cancelPlacement(QString const& message = {});
    void setPlacementPosition(glm::vec3 const& position);
    void rotatePlacement(float degrees);
    void rotateSelectedSpawn(float degrees);
    void updatePlacementSummary();
    void createSpawn();
    bool insertSpawn(std::uint64_t& guid, QString& error);
    void loadNearbySpawns();
    void setActivePhaseMask(unsigned mask);
    void populateEventMenu();
    void updateEventPreview();
    bool eventVisible(std::vector<int> const& direct_events,
                      std::vector<int> const& pool_events) const;
    void reconcileSelectedSpawnVisibility();
    void updateVisibleCachedSpawns();
    void removeCachedSpawn(std::uint64_t guid);
    void updateCachedSpawn(std::uint64_t guid, unsigned entry, glm::vec3 const& position,
                           unsigned display = 0, float yaw = 0.0f,
                           Noggit::NpcAppearance const* appearance = nullptr,
                           float scale = 1.0f,
                           std::optional<unsigned> phase_mask = std::nullopt);
    QString weaponVisibilitySettingsKey(std::uint64_t guid) const;
    void applyWeaponVisibility(std::uint64_t guid);
    void saveSelectedWeaponVisibility();
    void selectSpawn(std::uint64_t guid, unsigned entry);
    void selectGameObjectSpawn(std::uint64_t guid, unsigned entry);
    void showPhaseAssignment(bool gameobject, std::uint64_t guid,
                             unsigned entry, unsigned mask);
    void saveSelectedPhaseMask();
    void saveSelectedSpawn();
    void saveMovementSettings();
    void deleteSelectedSpawn();
    void deleteSelectedCustomNpc();
    void saveServerEmote();
    void saveSelectedWeaponStance();
    void previewSelectedWeaponStance();
    bool writeSpawnWeaponStance(QSqlDatabase& db, std::uint64_t guid,
                                unsigned entry, unsigned stance, QString& error) const;
    void saveUniqueTemplate();
    void openNpcEditor(bool selected_spawn, bool character_creator = false);
    void saveWaypoints();
    void addWaypoint(glm::vec3 const& position);
    QString waypointListText(std::size_t index) const;
    void updateWaypointList();
    bool collapseLegacyConformedWaypoints();
    void updateMovementPreview();
    void updateAnimationAvailability();
    void showPropertiesPanel();
    unsigned emotePreviewAnimation(int index) const;
    unsigned waypointPreviewAnimation(unsigned emote_id) const;
    QString waypointEmoteLabel(unsigned emote_id) const;
    bool refreshSpawnAppearance(unsigned display_id, unsigned entry,
                                bool appearance_already_loaded = false);
    unsigned selectedTemplateEntry() const;
    unsigned selectedTemplateDisplay() const;
    bool generatedNpcAssets(unsigned display_id, unsigned* extra_id = nullptr,
                            QString* texture_stem = nullptr) const;
    bool npcDbcDeploymentConfigured(QString& error) const;
    bool deployNpcDbcs(QString& error) const;
    bool selectTemplate(unsigned entry, bool begin_placement);

    struct WaypointDraft
    {
      glm::vec3 position{};
      unsigned delay_ms = 0;
      bool run = false;
      bool generated = false;
      unsigned emote_id = 0;
    };

    struct CachedNpcSpawn
    {
      std::uint64_t guid = 0;
      unsigned entry = 0;
      unsigned display = 0;
      unsigned phase_mask = 1;
      std::vector<int> direct_events;
      std::vector<int> pool_events;
      glm::vec3 position{};
      float yaw = 0.0f;
      std::array<unsigned, 3> weapon_displays{};
      std::array<unsigned, 3> weapon_inventory_types{};
      unsigned ranged_entry = 0;
      unsigned ranged_subclass = 0;
      std::optional<Noggit::NpcAppearance> appearance;
      float scale = 1.0f;
    };

    struct CachedGameObjectSpawn
    {
      std::uint64_t guid = 0;
      unsigned entry = 0;
      unsigned display = 0;
      unsigned phase_mask = 1;
      std::vector<int> direct_events;
      std::vector<int> pool_events;
      glm::vec3 position{};
      float yaw = 0.0f;
      float scale = 1.0f;
      std::string model_path;
    };

    struct ConformedSegmentCache
    {
      glm::vec3 start{};
      WaypointDraft destination;
      bool include_endpoint = false;
      std::vector<WaypointDraft> points;
    };

    enum NpcEditDomain : unsigned
    {
      EditTransform = 1,
      EditSpawn = 2,
      EditPath = 4,
      EditEmote = 8,
      EditTemplate = 16,
      EditPreview = 32,
      EditAll = EditTransform | EditSpawn | EditPath | EditEmote | EditTemplate | EditPreview
    };

    struct EditorSnapshot
    {
      glm::vec3 position{};
      double yaw = 0;
      int respawn = 0;
      double wander = 0;
      int movement = 0;
      double speed_walk = 1.0;
      double speed_run = 1.14286;
      int display = 0;
      std::vector<WaypointDraft> waypoints;
      int selected_waypoint = -1;
      int behavior = 0;
      bool conform = false;
      unsigned emote = 0;
      QString name;
      QString subname;
      int minlevel = 0;
      int maxlevel = 0;
      int faction = 0;
      int template_display = 0;
      bool preview_movement = false;
      int preview_animation = 0;
      QString preview_animation_text;
    };

    EditorSnapshot captureEditorSnapshot() const;
    void applyEditorSnapshot(EditorSnapshot const& snapshot, unsigned domains);
    void recordNpcEdit(unsigned domains);
    void commitNpcEdits(unsigned domains);
    void finishNpcTransformEdit();

    std::vector<WaypointDraft> patrolWaypoints() const;
    std::vector<WaypointDraft> terrainConformedWaypoints() const;

    QLineEdit* _host = nullptr;
    QSpinBox* _port = nullptr;
    QLineEdit* _database = nullptr;
    QLineEdit* _user = nullptr;
    QLineEdit* _password = nullptr;
    QLineEdit* _client_data_folder = nullptr;
    QLineEdit* _server_data_folder = nullptr;
    QPushButton* _load = nullptr;
    QWidget* _connection_panel = nullptr;
    QToolButton* _connection_toggle = nullptr;
    QLineEdit* _search = nullptr;
    QComboBox* _faction = nullptr;
    QComboBox* _type = nullptr;
    QComboBox* _role = nullptr;
    QListView* _table = nullptr;
    NpcPageFilterModel* _page_filter = nullptr;
    QPushButton* _previous_page = nullptr;
    QPushButton* _next_page = nullptr;
    QLabel* _page_status = nullptr;
    QLabel* _status = nullptr;
    QLabel* _details = nullptr;
    QLabel* _preview_note = nullptr;
    QLabel* _preview_speed_note = nullptr;
    QGroupBox* _placement_panel = nullptr;
    QWidget* _properties_panel = nullptr;
    QWidget* _waypoint_panel = nullptr;
    QLabel* _placement_summary = nullptr;
    QPushButton* _cancel_placement = nullptr;
    QSpinBox* _respawn_seconds = nullptr;
    QPushButton* _load_nearby_spawns = nullptr;
    QComboBox* _phase_mask = nullptr;
    QToolButton* _event_selector = nullptr;
    QMenu* _event_menu = nullptr;
    QGroupBox* _phase_assignment_panel = nullptr;
    QLabel* _phase_assignment_identity = nullptr;
    QComboBox* _phase_assignment_mask = nullptr;
    QTimer* _spawn_visibility_timer = nullptr;
    QToolButton* _create_npc = nullptr;
    QPushButton* _delete_npc_template = nullptr;
    QGroupBox* _spawn_editor = nullptr;
    QLabel* _spawn_identity = nullptr;
    QSpinBox* _edit_respawn = nullptr;
    QLabel* _edit_position_label = nullptr;
    QPushButton* _move_spawn_button = nullptr;
    QDoubleSpinBox* _edit_yaw = nullptr;
    QDoubleSpinBox* _edit_wander = nullptr;
    QComboBox* _edit_movement = nullptr;
    QDoubleSpinBox* _edit_speed_walk = nullptr;
    QDoubleSpinBox* _edit_speed_run = nullptr;
    QLabel* _speed_usage = nullptr;
    QSpinBox* _edit_display = nullptr;
    QComboBox* _edit_emote = nullptr;
    QLineEdit* _edit_name = nullptr;
    QLineEdit* _edit_subname = nullptr;
    QSpinBox* _edit_minlevel = nullptr;
    QSpinBox* _edit_maxlevel = nullptr;
    NpcFactionSelector* _edit_faction = nullptr;
    QSpinBox* _edit_template_display = nullptr;
    QListWidget* _waypoint_list = nullptr;
    QComboBox* _waypoint_behavior = nullptr;
    QSpinBox* _waypoint_delay = nullptr;
    QComboBox* _waypoint_emote = nullptr;
    QCheckBox* _waypoint_run = nullptr;
    QCheckBox* _conform_waypoints = nullptr;
    QPushButton* _capture_waypoint_button = nullptr;
    QCheckBox* _preview_movement = nullptr;
    QComboBox* _preview_animation = nullptr;
    QComboBox* _edit_weapon_stance = nullptr;
    QCheckBox* _show_main_off_hand = nullptr;
    QCheckBox* _show_ranged_weapon = nullptr;
    std::optional<std::uint64_t> _selected_spawn_guid;
    unsigned _selected_spawn_entry = 0;
    unsigned _template_spawn_count = 0;
    double _loaded_speed_walk = 1.0;
    double _loaded_speed_run = 1.14286;
    bool _capturing_waypoints = false;
    bool _moving_spawn = false;
    bool _placement_rotating = false;
    bool _selected_spawn_rotating = false;
    bool _consume_npc_left_release = false;
    bool _restoring_history = false;
    bool _pending_transform_edit = false;
    EditorSnapshot _history_snapshot;
    std::map<std::uint64_t, EditorSnapshot> _draft_snapshots;
    QPoint _placement_rotation_mouse_position;
    QPoint _selected_spawn_rotation_mouse_position;
    glm::vec3 _edit_spawn_position{};
    glm::vec3 _saved_spawn_position{};
    float _saved_spawn_yaw = 0.0f;
    std::vector<WaypointDraft> _waypoints;
    mutable std::vector<WaypointDraft> _conformed_source;
    mutable std::vector<WaypointDraft> _conformed_cache;
    mutable std::vector<ConformedSegmentCache> _conformed_segments;
    NpcTemplateTableModel* _model = nullptr;
    NpcTemplateFilterModel* _filter = nullptr;
    Tools::AssetBrowser::ModelViewer* _preview = nullptr;
    Tools::AssetBrowser::ModelViewer* _cache_preview = nullptr;
    std::unordered_map<std::uint64_t, CachedNpcSpawn> _cached_spawns;
    std::map<std::array<unsigned, 9>, std::pair<Noggit::NpcAppearance, float>>
      _npc_appearance_cache;
    std::map<std::pair<Noggit::NoggitRenderContext, std::string>,
             std::unique_ptr<scoped_model_reference>> _prefetched_npc_models;
    std::map<std::pair<Noggit::NoggitRenderContext, std::string>,
             std::unique_ptr<scoped_blp_texture_reference>> _prefetched_npc_textures;
    std::array<std::vector<std::uint64_t>, 64 * 64> _cached_spawns_by_tile;
    std::unordered_map<std::uint64_t, CachedGameObjectSpawn> _cached_gameobjects;
    std::array<std::vector<std::uint64_t>, 64 * 64> _cached_gameobjects_by_tile;
    std::unordered_set<std::uint64_t> _active_cached_spawns;
    std::unordered_set<std::uint64_t> _active_cached_gameobjects;
    std::unordered_set<std::uint64_t> _failed_cached_spawns;
    std::unordered_set<std::uint64_t> _failed_cached_gameobjects;
    QString _cached_connection;
    unsigned _cached_map = 0;
    unsigned _active_phase_mask = 1;
    std::unordered_set<int> _active_event_ids;
    std::map<int, QString> _event_descriptions;
    bool _spawn_cache_loaded = false;
    std::unique_ptr<NpcThumbnailRenderer> _thumbnail_renderer;
    MapView* _map_view = nullptr;
    std::optional<std::uint64_t> _selected_gameobject_guid;
    std::optional<std::uint64_t> _placement_overlay_guid;
    std::optional<Noggit::NpcAppearance> _placement_appearance;
    glm::vec3 _placement_position{};
    float _placement_yaw = 0.0f;
    float _placement_scale = 1.0f;
    unsigned _placement_entry = 0;
    unsigned _placement_display_id = 0;
    int _placement_equipment_id = 0;
    int _placement_weapon_stance = -1;
    QString _placement_name;
    std::string _selected_model_path;
    float _selected_model_scale = 1.0f;
    std::shared_ptr<BlizzardArchive::ClientData> _client_data;
    std::unique_ptr<DBCFile> _display_info;
    std::unique_ptr<DBCFile> _gameobject_display_info;
    std::unique_ptr<DBCFile> _display_extra;
    std::unique_ptr<DBCFile> _char_sections;
    std::unique_ptr<DBCFile> _hair_geosets;
    std::unique_ptr<DBCFile> _facial_hair_styles;
    std::unique_ptr<DBCFile> _item_display_info;
    std::unique_ptr<DBCFile> _item_data;
    std::unique_ptr<DBCFile> _helmet_geoset_vis;
    std::unique_ptr<DBCFile> _chr_races;
    std::unique_ptr<DBCFile> _model_data;
    std::unique_ptr<DBCFile> _npc_sounds;
    std::unique_ptr<DBCFile> _creature_sound_data;
    std::unique_ptr<DBCFile> _footstep_terrain_lookup;
    std::unique_ptr<DBCFile> _animation_data;
    std::unique_ptr<DBCFile> _emote_data;
  };
}
