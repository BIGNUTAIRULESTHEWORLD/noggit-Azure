// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/MapHeaders.h>
#include <noggit/TileIndex.hpp>
#include <noggit/texture_set.hpp>

#include <blizzard-archive-library/include/Listfile.hpp>
#include <math/trig.hpp>

#include <QObject>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

class World;
class MapChunk;
class MapView;
class liquid_layer;
class QString;

namespace Noggit::Ui::Tools::ChunkManipulator
{
  enum class ChunkCopyFlags : std::uint32_t
  {
    NONE = 0,
    TERRAIN = 1u << 0,
    LIQUID = 1u << 1,
    WMOs = 1u << 2,
    MODELS = 1u << 3,
    SHADOWS = 1u << 4,
    TEXTURES = 1u << 5,
    ALPHAMAPS = 1u << 6,
    VERTEX_COLORS = 1u << 7,
    HOLES = 1u << 8,
    FLAGS = 1u << 9,
    AREA_ID = 1u << 10,
    GROUND_EFFECTS = 1u << 11,
    GROUND_EFFECT_EXCLUSION = 1u << 12
  };

  constexpr ChunkCopyFlags operator|(ChunkCopyFlags lhs, ChunkCopyFlags rhs)
  {
    return static_cast<ChunkCopyFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
  }

  constexpr ChunkCopyFlags& operator|=(ChunkCopyFlags& lhs, ChunkCopyFlags rhs)
  {
    lhs = lhs | rhs;
    return lhs;
  }

  constexpr bool hasFlag(ChunkCopyFlags value, ChunkCopyFlags flag)
  {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
  }

  enum class ChunkSelectionMode
  {
    SELECT,
    DESELECT
  };

  enum class ChunkManipulatorObjectTypes
  {
    WMO,
    M2
  };

  struct SelectedChunkIndex
  {
    TileIndex tile_index{0, 0};
    unsigned x = 0;
    unsigned z = 0;

    friend bool operator<(SelectedChunkIndex const& lhs, SelectedChunkIndex const& rhs)
    {
      return std::tie(lhs.tile_index, lhs.x, lhs.z) < std::tie(rhs.tile_index, rhs.x, rhs.z);
    }

    friend bool operator==(SelectedChunkIndex const& lhs, SelectedChunkIndex const& rhs)
    {
      return lhs.tile_index == rhs.tile_index && lhs.x == rhs.x && lhs.z == rhs.z;
    }
  };

  struct ChunkTextureCache
  {
    std::vector<std::string> textures;
    std::vector<std::array<float, 64 * 64>> weights;
    std::vector<layer_info> layers;
  };

  struct ChunkObjectCacheEntry
  {
    BlizzardArchive::Listfile::FileKey file_key;
    ChunkManipulatorObjectTypes type;
    glm::vec3 relative_pos{};
    math::degrees::vec3 dir{};
    float scale = 1.f;
    std::uint16_t wmo_nameset = 0;
    std::uint16_t wmo_doodadset = 0;
  };

  struct ChunkLiquidLayerCache
  {
    int liquid_id = 0;
    std::uint64_t subchunks = 0;
    std::array<float, 9 * 9> heights{};
    std::array<float, 9 * 9> depths{};
    std::array<glm::vec2, 9 * 9> uvs{};
  };

  struct ChunkCache
  {
    SelectedChunkIndex index;
    float xbase = 0.f;
    float zbase = 0.f;
    std::array<float, 145> heights{};
    std::array<glm::vec3, 145> normals{};
    std::array<glm::vec3, 145> vertex_colors{};
    std::array<std::uint8_t, 64 * 64> shadows{};
    ChunkTextureCache textures;
    std::vector<ChunkLiquidLayerCache> liquids;
    std::array<std::uint8_t, 8> ground_effect_exclusion{};
    int holes = 0;
    unsigned area_id = 0;
    mcnk_flags flags{};
  };

  enum class ChunkHeightMode
  {
    NORMAL,
    MINIMUM,
    MAXIMUM,
    ADD,
    SUBTRACT
  };

  struct ChunkPasteOptions
  {
    ChunkCopyFlags components = ChunkCopyFlags::NONE;
    int rotation_quarter_turns = 0;
    float rotation_degrees = 0.f;
    bool mirror_horizontal = false;
    bool mirror_vertical = false;
    float height_offset = 0.f;
    ChunkHeightMode height_mode = ChunkHeightMode::NORMAL;
    bool automatic_seams = true;
  };

  struct ChunkPreviewOptions
  {
    bool enabled = true;
    bool m2s = true;
    bool wmos = true;
    bool heightmap = true;
    bool textures = true;

    friend bool operator==(ChunkPreviewOptions const&, ChunkPreviewOptions const&) = default;
  };

  struct ChunkPasteResult
  {
    int chunks_changed = 0;
    int objects_added = 0;
    int objects_removed = 0;
    int textures_dropped = 0;
  };

  class ChunkClipboard : public QObject
  {
    Q_OBJECT
  public:
    explicit ChunkClipboard(MapView* map_view, QObject* parent = nullptr);
    explicit ChunkClipboard(World* world, QObject* parent = nullptr);
    ~ChunkClipboard() override;

    bool selectRange(glm::vec3 const& cursor_pos, float radius, bool square, ChunkSelectionMode mode);
    void selectChunk(glm::vec3 const& pos, ChunkSelectionMode mode);
    void selectChunk(TileIndex const& tile_index, unsigned x, unsigned z, ChunkSelectionMode mode);
    bool copySelected();
    void setSourcePivot(glm::vec2 pivot);
    void setWorldForPaste(World* world);
    bool copyTileAt(glm::vec3 const& position, QString* error = nullptr);
    bool saveAsset(QString const& path, QString* error = nullptr) const;
    bool saveAdtAsset(QString const& path, QString* error = nullptr) const;
    bool loadAsset(QString const& path, QString* error = nullptr);
    void clearSelection();
    void clearPreview();
    void clearPreviewForHistoryChange(glm::vec3 const& destination);
    void setOverlaysVisible(bool visible);
    // Updates only the cheap cyan destination footprint. Returns true when the
    // detailed preview target changed and its debounce timer should restart.
    bool updatePreviewFootprint(glm::vec3 const& destination, ChunkPasteOptions const& options,
                                ChunkPreviewOptions const& preview_options);
    void updatePreview(glm::vec3 const& destination, ChunkPasteOptions const& options,
                       ChunkPreviewOptions const& preview_options);
    ChunkPasteResult pasteSelection(glm::vec3 const& destination, ChunkPasteOptions const& options);

    [[nodiscard]] std::set<SelectedChunkIndex> const& selectedChunks() const;
    [[nodiscard]] std::size_t cachedChunkCount() const;
    [[nodiscard]] bool hasCopy() const;
    [[nodiscard]] bool isAdtAsset() const;

  signals:
    void selectionChanged(std::set<SelectedChunkIndex> const& selected_chunks);
    void selectionCleared();
    void copied(std::size_t chunk_count, std::size_t m2_count, std::size_t wmo_count);
    void pasted(ChunkPasteResult result);

  private:
    bool saveAssetWithMagic(QString const& path, char const* magic, QString* error) const;
    ChunkCache const* sourceAt(glm::vec2 const& source_pos) const;
    float sampleHeight(ChunkCache const& cache, glm::vec2 const& source_pos) const;
    std::optional<chunk_mover_texture_preview> buildTexturePreview(
        MapChunk* destination_chunk, glm::vec2 const& destination_pivot,
        ChunkPasteOptions const& options) const;
    std::vector<liquid_layer> buildLiquidPreview(MapChunk* destination_chunk,
                                                 glm::vec2 const& destination_pivot,
                                                 ChunkPasteOptions const& options) const;
    glm::vec2 inverseTransform(glm::vec2 const& destination_pos, glm::vec2 const& destination_pivot,
                               ChunkPasteOptions const& options) const;
    void setOverlay(SelectedChunkIndex const& index, int value, bool force_upload = false);
    std::vector<MapChunk*> destinationChunks(glm::vec2 const& destination_pivot,
                                             ChunkPasteOptions const& options,
                                             bool load_missing_tiles = false) const;
    void sewTerrain(std::vector<MapChunk*> const& changed_chunks);
    void recalcNormalsAroundTerrain(std::vector<MapChunk*> const& changed_chunks);

    std::set<SelectedChunkIndex> _selected_chunks;
    std::vector<ChunkCache> _cached_chunks;
    std::unordered_map<std::uint64_t, std::size_t> _cached_chunk_lookup;
    std::vector<ChunkObjectCacheEntry> _cached_objects;
    std::vector<SelectedChunkIndex> _preview_chunks;
    std::vector<SelectedChunkIndex> _preview_overlay_chunks;
    std::unordered_map<std::size_t, std::uint32_t> _preview_object_uids;
    ChunkPasteOptions _preview_options;
    ChunkPreviewOptions _preview_display_options;
    std::unordered_map<std::uint64_t, std::optional<chunk_mover_texture_preview>>
        _texture_preview_cache;
    std::optional<std::uint8_t> _texture_preview_cache_transform;
    glm::vec2 _preview_pivot{};
    bool _preview_state_valid = false;
    ChunkPasteOptions _preview_overlay_options;
    ChunkPreviewOptions _preview_overlay_display_options;
    glm::vec2 _preview_overlay_pivot{};
    bool _preview_overlay_state_valid = false;
    std::optional<glm::vec2> _history_blocked_preview_pivot;
    World* _world = nullptr;
    MapView* _map_view = nullptr;
    glm::vec2 _source_pivot{};
    bool _is_adt_asset = false;
  };
}
