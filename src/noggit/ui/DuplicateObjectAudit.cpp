// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/DuplicateObjectAudit.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/SceneObject.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/world_model_instances_storage.hpp>
#include <opengl/context.hpp>

#include <QCheckBox>
#include <QByteArray>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
  struct DuplicateRecord
  {
    bool is_wmo = false;
    std::string asset;
    glm::vec3 position{};
    glm::vec3 rotation{};
    float scale = 1.0f;
    std::uint16_t wmo_flags = 0;
    std::uint16_t doodad_set = 0;
    std::uint16_t name_set = 0;
    std::vector<std::uint32_t> uids;
    std::vector<std::string> adts;
    std::map<std::uint32_t, std::string> uid_sources;
  };

  std::string lowerPath(std::string value)
  {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
      if (ch == '/')
        return '\\';
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  }

  std::optional<TileIndex> tileForAdt(std::string const& adt)
  {
    std::string const stem = std::filesystem::path(adt).stem().string();
    std::size_t const z_separator = stem.rfind('_');
    if (z_separator == std::string::npos)
      return std::nullopt;
    std::size_t const x_separator = stem.rfind('_', z_separator - 1);
    if (x_separator == std::string::npos)
      return std::nullopt;

    unsigned x = 0;
    unsigned z = 0;
    auto const x_result = std::from_chars(stem.data() + x_separator + 1,
                                          stem.data() + z_separator, x);
    auto const z_result = std::from_chars(stem.data() + z_separator + 1,
                                          stem.data() + stem.size(), z);
    if (x_result.ec != std::errc{} || z_result.ec != std::errc{}
        || x_result.ptr != stem.data() + z_separator
        || z_result.ptr != stem.data() + stem.size() || x >= 64 || z >= 64)
      return std::nullopt;
    return TileIndex(x, z);
  }

  std::string placementKey(bool is_wmo, std::string const& asset,
                           glm::vec3 const& position, glm::vec3 const& rotation,
                           std::uint16_t scale, std::uint16_t flags = 0,
                           std::uint16_t doodad_set = 0, std::uint16_t name_set = 0)
  {
    std::string key;
    key.reserve(1 + asset.size() + 1 + sizeof(float) * 6 + sizeof(scale));
    key.push_back(is_wmo ? 'W' : 'M');
    key.append(asset);
    key.push_back('\0');
    key.append(reinterpret_cast<char const*>(&position.x), sizeof(float));
    key.append(reinterpret_cast<char const*>(&position.y), sizeof(float));
    key.append(reinterpret_cast<char const*>(&position.z), sizeof(float));
    key.append(reinterpret_cast<char const*>(&rotation.x), sizeof(float));
    key.append(reinterpret_cast<char const*>(&rotation.y), sizeof(float));
    key.append(reinterpret_cast<char const*>(&rotation.z), sizeof(float));
    key.append(reinterpret_cast<char const*>(&scale), sizeof(scale));
    if (is_wmo)
    {
      key.append(reinterpret_cast<char const*>(&flags), sizeof(flags));
      key.append(reinterpret_cast<char const*>(&doodad_set), sizeof(doodad_set));
      key.append(reinterpret_cast<char const*>(&name_set), sizeof(name_set));
    }
    return key;
  }

  QString joinUids(std::vector<std::uint32_t> const& uids)
  {
    QStringList values;
    values.reserve(static_cast<int>(uids.size()));
    for (std::uint32_t uid : uids)
      values.push_back(QString::number(uid));
    return values.join(';');
  }

  QString joinAdts(std::vector<std::string> const& adts)
  {
    QStringList values;
    values.reserve(static_cast<int>(adts.size()));
    for (std::string const& adt : adts)
      values.push_back(QString::fromStdString(adt));
    return values.join(';');
  }

  QString csvCell(QString value)
  {
    value.replace('"', "\"\"");
    return '"' + value + '"';
  }

  bool matchesRecord(SceneObject const& object, DuplicateRecord const& record)
  {
    if (object.chunk_mover_preview || (object.which() == eWMO) != record.is_wmo
        || !object.instance_model()->file_key().hasFilepath()
        || lowerPath(object.instance_model()->file_key().filepath()) != record.asset)
      return false;

    if (object.pos.x != record.position.x || object.pos.y != record.position.y
        || object.pos.z != record.position.z || object.dir.x != record.rotation.x
        || object.dir.y != record.rotation.y || object.dir.z != record.rotation.z
        || object.scale != record.scale)
      return false;

    if (record.is_wmo)
    {
      auto const& wmo = static_cast<WMOInstance const&>(object);
      return wmo.mFlags == record.wmo_flags && wmo.doodadset() == record.doodad_set
          && wmo.mNameset == record.name_set;
    }
    return true;
  }

  std::vector<std::uint32_t> matchingLiveUids(World* world, DuplicateRecord const& record)
  {
    std::vector<std::uint32_t> result;
    auto& storage = world->getModelInstanceStorage();
    for (std::uint32_t uid : record.uids)
    {
      auto instance = storage.get_instance(uid);
      if (instance && instance->index() == eEntry_Object
          && matchesRecord(*std::get<selected_object_type>(*instance), record))
        result.push_back(uid);
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  bool sourceTilesLoaded(World* world, DuplicateRecord const& record)
  {
    for (auto const& [uid, adt] : record.uid_sources)
    {
      auto const tile = tileForAdt(adt);
      if (!tile || !world->mapIndex.tileLoaded(*tile)
          || world->mapIndex.getTile(*tile)->loading_failed())
        return false;
    }
    return !record.uid_sources.empty();
  }
}

namespace Noggit::Ui
{
  struct DuplicateObjectAudit::Impl
  {
    DuplicateObjectAudit* owner = nullptr;
    MapView* map_view = nullptr;
    QLabel* summary = nullptr;
    QLabel* filter_summary = nullptr;
    QLineEdit* search = nullptr;
    QCheckBox* show_m2 = nullptr;
    QCheckBox* show_wmo = nullptr;
    QTreeWidget* tree = nullptr;
    QPushButton* scan_button = nullptr;
    QPushButton* export_button = nullptr;
    QPushButton* remove_button = nullptr;
    std::vector<DuplicateRecord> records;
    std::size_t files_scanned = 0;
    std::size_t placements_scanned = 0;
    bool has_scanned = false;

    Impl(DuplicateObjectAudit* owner_, MapView* map_view_)
      : owner(owner_)
      , map_view(map_view_)
    {
      auto* layout = new QVBoxLayout(owner);
      layout->setContentsMargins(6, 6, 6, 6);

      auto* explanation = new QLabel(
        "Shared references with the same UID across neighboring ADTs are valid and excluded. "
        "Double-click a row to select its location.",
        owner);
      explanation->setWordWrap(true);
      layout->addWidget(explanation);

      auto* filters = new QHBoxLayout();
      show_m2 = new QCheckBox("M2", owner);
      show_wmo = new QCheckBox("WMO", owner);
      show_m2->setChecked(true);
      show_wmo->setChecked(true);
      search = new QLineEdit(owner);
      search->setPlaceholderText("Search asset, UID, ADT, or coordinates...");
      search->setClearButtonEnabled(true);
      filters->addWidget(show_m2);
      filters->addWidget(show_wmo);
      filters->addWidget(search, 1);
      layout->addLayout(filters);

      summary = new QLabel("Checking loaded ADTs for exact M2/WMO placement duplicates.", owner);
      filter_summary = new QLabel("No scan results loaded.", owner);
      layout->addWidget(summary);
      layout->addWidget(filter_summary);

      tree = new QTreeWidget(owner);
      tree->setColumnCount(9);
      tree->setHeaderLabels({"Type", "State", "Copies", "Asset", "UIDs",
                             "X", "Y", "Z", "ADTs"});
      tree->setRootIsDecorated(false);
      tree->setSelectionMode(QAbstractItemView::SingleSelection);
      tree->setSortingEnabled(true);
      tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
      tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
      tree->header()->setSectionResizeMode(8, QHeaderView::Stretch);
      layout->addWidget(tree);

      auto* buttons = new QHBoxLayout();
      auto* previous = new QPushButton("Previous", owner);
      auto* go_to = new QPushButton("Go To", owner);
      auto* next = new QPushButton("Next", owner);
      remove_button = new QPushButton("Remove Duplicate", owner);
      export_button = new QPushButton("Export CSV", owner);
      scan_button = new QPushButton("Refresh Loaded ADTs", owner);
      export_button->setEnabled(false);
      remove_button->setEnabled(false);
      scan_button->setToolTip("Check only the ADTs currently loaded in the editor.");
      buttons->addWidget(previous);
      buttons->addWidget(go_to);
      buttons->addWidget(next);
      buttons->addWidget(remove_button);
      buttons->addWidget(export_button);
      buttons->addStretch();
      buttons->addWidget(scan_button);
      layout->addLayout(buttons);

      QObject::connect(scan_button, &QPushButton::clicked, owner, [this] { startScan(); });
      QObject::connect(export_button, &QPushButton::clicked, owner, [this] { exportCsv(); });
      QObject::connect(search, &QLineEdit::textChanged, owner, [this] { filter(); });
      QObject::connect(show_m2, &QCheckBox::toggled, owner, [this] { filter(); });
      QObject::connect(show_wmo, &QCheckBox::toggled, owner, [this] { filter(); });
      QObject::connect(go_to, &QPushButton::clicked, owner, [this] { focusCurrent(); });
      QObject::connect(remove_button, &QPushButton::clicked, owner,
                       [this] { removeCurrent(); });
      QObject::connect(tree, &QTreeWidget::itemDoubleClicked, owner,
                       [this](QTreeWidgetItem*, int) { focusCurrent(); });
      QObject::connect(previous, &QPushButton::clicked, owner,
                       [this] { selectRelative(-1); });
      QObject::connect(next, &QPushButton::clicked, owner,
                       [this] { selectRelative(1); });
      QObject::connect(tree, &QTreeWidget::itemSelectionChanged, owner,
                       [this] { updateRemoveButton(); });
      QObject::connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::objectHistoryNavigated,
                        owner, [this]
      {
        if (owner->isVisible() && has_scanned && refreshLiveRecords())
          populate();
      });
      if (auto* dock = qobject_cast<QDockWidget*>(owner->parentWidget()))
        QObject::connect(dock, &QDockWidget::visibilityChanged, owner,
                         [this](bool visible)
        {
          if (visible)
          {
            if (!has_scanned)
              startScan();
            else if (refreshLiveRecords())
              populate();
          }
        });
      auto* live_refresh = new QTimer(owner);
      live_refresh->setInterval(1500);
      QObject::connect(live_refresh, &QTimer::timeout, owner, [this]
      {
        if (owner->isVisible() && has_scanned && refreshLiveRecords())
          populate();
      });
      live_refresh->start();
    }

    void startScan()
    {
      if (!map_view || !map_view->getWorld())
        return;
      has_scanned = true;
      refreshLiveRecords();
      populate();
    }

    bool refreshLiveRecords()
    {
      World* world = map_view ? map_view->getWorld() : nullptr;
      if (!world)
        return false;
      std::size_t const old_tiles = files_scanned;
      std::size_t const old_placements = placements_scanned;
      auto signature = [](std::vector<DuplicateRecord> const& entries)
      {
        std::vector<std::pair<std::string, std::vector<std::uint32_t>>> result;
        result.reserve(entries.size());
        for (DuplicateRecord const& record : entries)
          result.emplace_back(placementKey(record.is_wmo, record.asset, record.position,
            record.rotation, static_cast<std::uint16_t>(std::round(record.scale * 1024.0f)),
            record.wmo_flags, record.doodad_set, record.name_set), record.uids);
        return result;
      };
      auto const before = signature(records);
      records.clear();
      files_scanned = 0;
      placements_scanned = 0;
      for (MapTile* tile : world->mapIndex.loaded_tiles())
        if (tile && tile->finishedLoading() && !tile->loading_failed())
          ++files_scanned;

      std::map<std::string, DuplicateRecord> live_groups;
      auto add_live = [this, &live_groups, world](SceneObject& object)
      {
        if (object.chunk_mover_preview || !object.instance_model()->file_key().hasFilepath())
          return;

        std::set<std::string> loaded_sources;
        for (MapTile* tile : object.getTiles())
          if (tile && world->mapIndex.getTile(tile->index) == tile
              && tile->finishedLoading() && !tile->loading_failed())
            loaded_sources.insert(world->basename + "_" + std::to_string(tile->index.x)
                                  + "_" + std::to_string(tile->index.z) + ".adt");
        if (loaded_sources.empty())
        {
          TileIndex const tile(object.pos);
          if (world->mapIndex.tileLoaded(tile)
              && !world->mapIndex.getTile(tile)->loading_failed())
            loaded_sources.insert(world->basename + "_" + std::to_string(tile.x)
                                  + "_" + std::to_string(tile.z) + ".adt");
        }
        if (loaded_sources.empty())
          return;

        ++placements_scanned;
        bool const is_wmo = object.which() == eWMO;
        auto* wmo = is_wmo ? static_cast<WMOInstance*>(&object) : nullptr;
        std::string asset = lowerPath(object.instance_model()->file_key().filepath());
        glm::vec3 const rotation(object.dir.x, object.dir.y, object.dir.z);
        std::string const key = placementKey(is_wmo, asset, object.pos, rotation,
          static_cast<std::uint16_t>(std::round(object.scale * 1024.0f)),
          wmo ? wmo->mFlags : 0, wmo ? wmo->doodadset() : 0,
          wmo ? wmo->mNameset : 0);
        DuplicateRecord& record = live_groups[key];
        if (record.uids.empty())
        {
          record.is_wmo = is_wmo;
          record.asset = std::move(asset);
          record.position = object.pos;
          record.rotation = rotation;
          record.scale = object.scale;
          record.wmo_flags = wmo ? wmo->mFlags : 0;
          record.doodad_set = wmo ? wmo->doodadset() : 0;
          record.name_set = wmo ? wmo->mNameset : 0;
        }
        record.uids.push_back(object.uid);
        record.uid_sources[object.uid] = *loaded_sources.begin();
        for (std::string const& name : loaded_sources)
          if (std::find(record.adts.begin(), record.adts.end(), name) == record.adts.end())
            record.adts.push_back(name);
      };
      auto& storage = world->getModelInstanceStorage();
      storage.for_each_m2_instance([&](ModelInstance& object) { add_live(object); });
      storage.for_each_wmo_instance([&](WMOInstance& object) { add_live(object); });
      for (auto& [key, record] : live_groups)
      {
        if (record.uids.size() < 2)
          continue;
        std::sort(record.uids.begin(), record.uids.end());
        std::sort(record.adts.begin(), record.adts.end());
        records.push_back(std::move(record));
      }
      std::sort(records.begin(), records.end(), [](DuplicateRecord const& lhs,
                                                   DuplicateRecord const& rhs)
      {
        if (lhs.uids.size() != rhs.uids.size())
          return lhs.uids.size() > rhs.uids.size();
        if (lhs.is_wmo != rhs.is_wmo)
          return lhs.is_wmo > rhs.is_wmo;
        if (lhs.asset != rhs.asset)
          return lhs.asset < rhs.asset;
        if (lhs.position.x != rhs.position.x)
          return lhs.position.x < rhs.position.x;
        return lhs.position.z < rhs.position.z;
      });
      return signature(records) != before || files_scanned != old_tiles
          || placements_scanned != old_placements;
    }

    void updateRemoveButton()
    {
      auto* item = tree->currentItem();
      std::size_t const index = item ? item->data(0, Qt::UserRole).toULongLong() : records.size();
      remove_button->setEnabled(index < records.size() && map_view && map_view->getWorld()
                                && !item->isHidden() && records[index].uids.size() > 1
                                && sourceTilesLoaded(map_view->getWorld(), records[index]));
    }

    void populate()
    {
      QByteArray selected_key;
      if (auto* selected = tree->currentItem())
        selected_key = selected->data(0, Qt::UserRole + 1).toByteArray();

      tree->setSortingEnabled(false);
      tree->setUpdatesEnabled(false);
      tree->clear();
      QTreeWidgetItem* restore = nullptr;
      std::size_t locations = 0;
      std::size_t identities = 0;
      std::size_t excess = 0;
      for (std::size_t row = 0; row < records.size(); ++row)
      {
        DuplicateRecord const& record = records[row];
        if (record.uids.size() < 2)
          continue;
        ++locations;
        identities += record.uids.size();
        excess += record.uids.size() - 1;
        QString const rotation = QString("%1, %2, %3")
          .arg(record.rotation.x, 0, 'f', 3)
          .arg(record.rotation.y, 0, 'f', 3)
          .arg(record.rotation.z, 0, 'f', 3);
        auto* item = new QTreeWidgetItem(tree, {
          record.is_wmo ? "WMO" : "M2",
          "Loaded",
          QString::number(record.uids.size()),
          QString::fromStdString(record.asset),
          joinUids(record.uids),
          QString::number(record.position.x, 'f', 6),
          QString::number(record.position.y, 'f', 6),
          QString::number(record.position.z, 'f', 6),
          joinAdts(record.adts)});
        item->setData(0, Qt::UserRole, static_cast<qulonglong>(row));
        std::string const key = placementKey(record.is_wmo, record.asset, record.position,
          record.rotation, static_cast<std::uint16_t>(std::round(record.scale * 1024.0f)),
          record.wmo_flags, record.doodad_set, record.name_set);
        QByteArray const row_key(key.data(), static_cast<int>(key.size()));
        item->setData(0, Qt::UserRole + 1, row_key);
        item->setToolTip(2, QString("%1 excess object(s)").arg(record.uids.size() - 1));
        item->setToolTip(3, QString("%1\nRotation: %2\nScale: %3")
          .arg(QString::fromStdString(record.asset), rotation)
          .arg(record.scale, 0, 'f', 4));
        item->setToolTip(8, joinAdts(record.adts));
        if (!selected_key.isEmpty() && row_key == selected_key)
          restore = item;
      }
      if (restore)
        tree->setCurrentItem(restore);
      tree->setUpdatesEnabled(true);
      tree->setSortingEnabled(true);
      export_button->setEnabled(locations > 0);

      summary->setText(
        QString("%1 duplicate location(s), %2 distinct UIDs, %3 excess object(s); "
                "%4 placements checked across %5 loaded ADTs.")
          .arg(locations)
          .arg(identities)
          .arg(excess)
          .arg(placements_scanned)
          .arg(files_scanned));
      filter();
      updateRemoveButton();
    }

    void filter()
    {
      QString const needle = search->text().trimmed();
      int visible = 0;
      for (int row = 0; row < tree->topLevelItemCount(); ++row)
      {
        QTreeWidgetItem* item = tree->topLevelItem(row);
        bool const type_matches = (item->text(0) == "M2" && show_m2->isChecked())
                               || (item->text(0) == "WMO" && show_wmo->isChecked());
        bool text_matches = needle.isEmpty();
        for (int column = 0; !text_matches && column < item->columnCount(); ++column)
          text_matches = item->text(column).contains(needle, Qt::CaseInsensitive);
        bool const matches = type_matches && text_matches;
        item->setHidden(!matches);
        if (!matches)
          item->setSelected(false);
        else
          ++visible;
      }
      filter_summary->setText(QString("Showing %1 of %2 duplicate location(s).")
                                .arg(visible).arg(tree->topLevelItemCount()));
      updateRemoveButton();
    }

    void selectRelative(int direction)
    {
      int const count = tree->topLevelItemCount();
      if (!count)
        return;
      int row = tree->indexOfTopLevelItem(tree->currentItem());
      for (int checked = 0; checked < count; ++checked)
      {
        row = row < 0 ? (direction > 0 ? 0 : count - 1)
                      : (row + direction + count) % count;
        QTreeWidgetItem* item = tree->topLevelItem(row);
        if (item->isHidden())
          continue;
        tree->setCurrentItem(item);
        tree->scrollToItem(item);
        focusCurrent();
        return;
      }
    }

    void focusCurrent()
    {
      QTreeWidgetItem* item = tree->currentItem();
      if (!item || !map_view)
        return;
      std::size_t const index = item->data(0, Qt::UserRole).toULongLong();
      if (index >= records.size())
        return;

      DuplicateRecord const record = records[index];
      if (!sourcesStillLoaded(record))
        return;
      std::vector<std::uint32_t> const live_uids = matchingLiveUids(map_view->getWorld(), record);
      refreshLiveRecords();
      populate();
      if (live_uids.empty())
      {
        QMessageBox::information(owner, "Duplicate Object Audit",
                                 "This placement changed since the scan. Refresh the audit to review it.");
        return;
      }

      auto instance = map_view->getWorld()->getModelInstanceStorage().get_instance(live_uids.front());
      if (!instance || instance->index() != eEntry_Object)
        return;
      SceneObject* object = std::get<selected_object_type>(*instance);
      map_view->getWorld()->reset_selection();
      map_view->getWorld()->add_to_selection(object, true);
      glm::vec3 const target = object->pos;
      map_view->cursorPosition(target);
      float const distance = record.is_wmo ? 100.0f : 60.0f;
      Camera* camera = map_view->getCamera();
      camera->position = target + glm::vec3(0.0f, distance * 0.65f, -distance);
      glm::vec3 const direction = glm::normalize(target - camera->position);
      float const horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
      camera->yaw(math::degrees(glm::degrees(std::atan2(direction.x, direction.z))));
      camera->pitch(math::degrees(glm::degrees(std::atan2(-direction.y, horizontal))));
      map_view->setCameraDirty();
      map_view->invalidate();
      map_view->setFocus(Qt::OtherFocusReason);
      showStatus(QString("%1 duplicate at (%2, %3, %4), UID %5")
        .arg(record.is_wmo ? "WMO" : "M2")
        .arg(target.x, 0, 'f', 2).arg(target.y, 0, 'f', 2).arg(target.z, 0, 'f', 2)
        .arg(live_uids.front()));
    }

    void showStatus(QString const& message)
    {
      if (map_view)
        if (auto* window = qobject_cast<QMainWindow*>(map_view->window()))
          window->statusBar()->showMessage(message, 8000);
    }

    bool sourcesStillLoaded(DuplicateRecord const& record)
    {
      if (!map_view || !map_view->getWorld())
        return false;
      if (sourceTilesLoaded(map_view->getWorld(), record))
        return true;
      showStatus("A source ADT is no longer loaded; refresh the list to continue.");
      refreshLiveRecords();
      populate();
      return false;
    }

    void removeCurrent()
    {
      auto* item = tree->currentItem();
      std::size_t const index = item ? item->data(0, Qt::UserRole).toULongLong() : records.size();
      if (index >= records.size() || !map_view || !sourcesStillLoaded(records[index]))
        return;

      DuplicateRecord const record = records[index];
      World* world = map_view->getWorld();
      world->wait_for_all_tile_updates();
      std::vector<std::uint32_t> const live_uids = matchingLiveUids(world, record);
      if (live_uids.size() < 2)
      {
        refreshLiveRecords();
        populate();
        showStatus("The duplicate changed since the scan; nothing was removed.");
        return;
      }

      std::vector<std::uint32_t> const remove_uids(live_uids.begin() + 1, live_uids.end());
      QMessageBox confirmation(owner);
      confirmation.setIcon(QMessageBox::Warning);
      confirmation.setWindowTitle("Remove duplicate objects");
      confirmation.setText(QString("Keep %1 UID %2 and remove %3 duplicate%4?")
        .arg(record.is_wmo ? "WMO" : "M2")
        .arg(live_uids.front())
        .arg(remove_uids.size())
        .arg(remove_uids.size() == 1 ? "" : "s"));
      confirmation.setInformativeText(
        QString("%1\nLocation: %2, %3, %4\nRemove UIDs: %5\n\nThis action can be undone.")
          .arg(QString::fromStdString(record.asset))
          .arg(record.position.x, 0, 'f', 2)
          .arg(record.position.y, 0, 'f', 2)
          .arg(record.position.z, 0, 'f', 2)
          .arg(joinUids(remove_uids)));
      QPushButton* confirm_button = confirmation.addButton("Remove Duplicate", QMessageBox::DestructiveRole);
      confirmation.addButton(QMessageBox::Cancel);
      confirmation.setDefaultButton(QMessageBox::Cancel);
      confirmation.exec();
      if (confirmation.clickedButton() != confirm_button)
        return;

      if (!sourcesStillLoaded(record) || matchingLiveUids(world, record) != live_uids)
      {
        showStatus("The duplicate changed during confirmation; nothing was removed.");
        refreshLiveRecords();
        populate();
        return;
      }

      map_view->makeCurrent();
      OpenGL::context::scoped_setter const context_setter(::gl, map_view->context());
      NOGGIT_ACTION_MGR->beginAction(map_view, Noggit::ActionFlags::eOBJECTS_REMOVED);
      world->deleteInstances(remove_uids, true);
      NOGGIT_ACTION_MGR->endAction();
      refreshLiveRecords();
      populate();
      map_view->invalidate();
      showStatus(QString("Removed %1 duplicate object%2. Undo restores them.")
        .arg(remove_uids.size()).arg(remove_uids.size() == 1 ? "" : "s"));
    }

    void exportCsv()
    {
      if (tree->topLevelItemCount() == 0)
        return;
      QString filename = QFileDialog::getSaveFileName(
        owner, "Export Duplicate Object Audit", "duplicate-object-audit.csv",
        "CSV files (*.csv)");
      if (filename.isEmpty())
        return;
      if (!filename.endsWith(".csv", Qt::CaseInsensitive))
        filename += ".csv";

      QSaveFile file(filename);
      if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
      {
        QMessageBox::critical(owner, "Export Failed", file.errorString());
        return;
      }
      QTextStream stream(&file);
      stream.setCodec("UTF-8");
      stream << "type,asset,x,y,z,rotation_x,rotation_y,rotation_z,scale,copy_count,excess_count,uids,adts\n";
      for (DuplicateRecord const& record : records)
      {
        if (record.uids.size() < 2)
          continue;
        stream << (record.is_wmo ? "WMO" : "M2") << ','
               << csvCell(QString::fromStdString(record.asset)) << ','
               << QString::number(record.position.x, 'f', 9) << ','
               << QString::number(record.position.y, 'f', 9) << ','
               << QString::number(record.position.z, 'f', 9) << ','
               << QString::number(record.rotation.x, 'f', 9) << ','
               << QString::number(record.rotation.y, 'f', 9) << ','
               << QString::number(record.rotation.z, 'f', 9) << ','
               << QString::number(record.scale, 'f', 6) << ','
               << record.uids.size() << ','
               << record.uids.size() - 1 << ','
               << csvCell(joinUids(record.uids)) << ','
               << csvCell(joinAdts(record.adts)) << '\n';
      }
      if (!file.commit())
        QMessageBox::critical(owner, "Export Failed", file.errorString());
    }
  };

  DuplicateObjectAudit::DuplicateObjectAudit(MapView* map_view, QWidget* parent)
    : QWidget(parent)
    , _impl(std::make_unique<Impl>(this, map_view))
  {
  }

  DuplicateObjectAudit::~DuplicateObjectAudit() = default;
}
