#include <noggit/ui/NpcTemplateBrowser.hpp>

#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/ActionManager.hpp>
#include <noggit/database/WorldDatabaseSchema.hpp>
#include <noggit/DBC.h>
#include <noggit/DBCFile.h>
#include <noggit/MapView.h>
#include <noggit/Model.h>
#include <noggit/NpcSpawnOverlay.hpp>
#include <noggit/SceneObject.hpp>
#include <noggit/World.h>
#include <noggit/TextureManager.h>
#include <noggit/ui/tools/AssetBrowser/BrowserModelView.hpp>
#include <noggit/ui/NpcDialogueSoundEditor.hpp>
#include <noggit/ui/NpcFactionSelector.hpp>
#include <noggit/ui/NpcSoundEditor.hpp>

#include <external/PNG2BLP/Png2Blp.h>

#include <blizzard-archive-library/include/Listfile.hpp>

#include <math/ray.hpp>

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QBuffer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QSaveFile>
#include <QSettings>
#include <QScrollArea>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace Noggit::Ui
{
NpcTemplateBrowser::EditorSnapshot NpcTemplateBrowser::captureEditorSnapshot() const
{
  EditorSnapshot state;
  state.position = _edit_spawn_position;
  state.yaw = _edit_yaw->value();
  state.respawn = _edit_respawn->value();
  state.wander = _edit_wander->value();
  state.movement = _edit_movement->currentData().toInt();
  state.speed_walk = _edit_speed_walk->value();
  state.speed_run = _edit_speed_run->value();
  state.display = _edit_display->value();
  state.waypoints = _waypoints;
  state.selected_waypoint = _waypoint_list->currentRow();
  state.behavior = _waypoint_behavior->currentData().toInt();
  state.conform = _conform_waypoints->isChecked();
  state.emote = _edit_emote->currentData().toUInt();
  state.name = _edit_name->text();
  state.subname = _edit_subname->text();
  state.minlevel = _edit_minlevel->value();
  state.maxlevel = _edit_maxlevel->value();
  state.faction = static_cast<int>(_edit_faction->factionId());
  state.template_display = _edit_template_display->value();
  state.preview_movement = _preview_movement->isChecked();
  state.preview_animation = _preview_animation->currentIndex();
  state.preview_animation_text = _preview_animation->currentText();
  return state;
}

void NpcTemplateBrowser::applyEditorSnapshot(EditorSnapshot const& state, unsigned domains)
{
  QScopedValueRollback<bool> const restoring(_restoring_history, true);
  if (domains & EditTransform)
  {
    _edit_spawn_position = state.position;
    QSignalBlocker const block(_edit_yaw);
    _edit_yaw->setValue(state.yaw);
    _edit_position_label->setText(QString("%1, %2, %3")
      .arg(state.position.x, 0, 'f', 2).arg(state.position.y, 0, 'f', 2)
      .arg(state.position.z, 0, 'f', 2));
    if (_selected_spawn_guid && _map_view && _map_view->getWorld())
      if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
      {
        overlay->setPreviewRoute({}, false);
        overlay->setTransform(state.position, static_cast<float>(state.yaw), overlay->scale());
      }
  }
  if (domains & EditSpawn)
  {
    QSignalBlocker const respawn_block(_edit_respawn);
    QSignalBlocker const wander_block(_edit_wander);
    QSignalBlocker const movement_block(_edit_movement);
    QSignalBlocker const display_block(_edit_display);
    _edit_respawn->setValue(state.respawn);
    _edit_wander->setValue(state.wander);
    _edit_movement->setCurrentIndex(std::max(0, _edit_movement->findData(state.movement)));
    _edit_display->setValue(state.display);
    _edit_wander->setEnabled(state.movement == 1);
  }
  if (domains & EditPath)
  {
    QSignalBlocker const behavior_block(_waypoint_behavior);
    QSignalBlocker const conform_block(_conform_waypoints);
    _waypoints = state.waypoints;
    _waypoint_behavior->setCurrentIndex(std::max(0, _waypoint_behavior->findData(state.behavior)));
    _conform_waypoints->setChecked(state.conform);
    updateWaypointList();
    _waypoint_list->setCurrentRow(state.selected_waypoint);
  }
  if (domains & EditEmote)
  {
    QSignalBlocker const block(_edit_emote);
    _edit_emote->setCurrentIndex(std::max(0, _edit_emote->findData(state.emote)));
  }
  if (domains & EditTemplate)
  {
    QSignalBlocker const name_block(_edit_name);
    QSignalBlocker const subname_block(_edit_subname);
    QSignalBlocker const min_block(_edit_minlevel);
    QSignalBlocker const max_block(_edit_maxlevel);
    QSignalBlocker const faction_block(_edit_faction);
    QSignalBlocker const display_block(_edit_template_display);
    QSignalBlocker const walk_speed_block(_edit_speed_walk);
    QSignalBlocker const run_speed_block(_edit_speed_run);
    _edit_name->setText(state.name);
    _edit_subname->setText(state.subname);
    _edit_minlevel->setValue(state.minlevel);
    _edit_maxlevel->setValue(state.maxlevel);
    _edit_faction->setFactionId(static_cast<unsigned>(std::max(0, state.faction)));
    _edit_template_display->setValue(state.template_display);
    _edit_speed_walk->setValue(state.speed_walk);
    _edit_speed_run->setValue(state.speed_run);
  }
  if (domains & EditPreview)
  {
    QSignalBlocker const movement_block(_preview_movement);
    QSignalBlocker const animation_block(_preview_animation);
    _preview_movement->setChecked(state.preview_movement);
    _preview_animation->setCurrentIndex(state.preview_animation);
    _preview_animation->setEditText(state.preview_animation_text);
  }
  _history_snapshot = captureEditorSnapshot();
  if (_selected_spawn_guid) _draft_snapshots[*_selected_spawn_guid] = _history_snapshot;
  updateMovementPreview();
  if (_map_view) { _map_view->invalidate(); _map_view->update(); }
}

void NpcTemplateBrowser::recordNpcEdit(unsigned domains)
{
  if (_restoring_history || !_selected_spawn_guid || !_map_view) return;
  EditorSnapshot const before = _history_snapshot;
  EditorSnapshot const after = captureEditorSnapshot();
  auto const same_points = [&]
  {
    if (before.waypoints.size() != after.waypoints.size()) return false;
    for (std::size_t i = 0; i < before.waypoints.size(); ++i)
    {
      auto const& a = before.waypoints[i];
      auto const& b = after.waypoints[i];
      if (a.position.x != b.position.x || a.position.y != b.position.y
          || a.position.z != b.position.z || a.delay_ms != b.delay_ms
          || a.run != b.run || a.emote_id != b.emote_id || a.generated != b.generated)
        return false;
    }
    return true;
  };
  unsigned changed_domains = 0;
  if (before.position.x != after.position.x || before.position.y != after.position.y
      || before.position.z != after.position.z || before.yaw != after.yaw)
    changed_domains |= EditTransform;
  if (before.respawn != after.respawn || before.wander != after.wander
      || before.movement != after.movement || before.display != after.display)
    changed_domains |= EditSpawn;
  if (!same_points() || before.behavior != after.behavior || before.conform != after.conform)
    changed_domains |= EditPath;
  if (before.emote != after.emote) changed_domains |= EditEmote;
  if (before.name != after.name || before.subname != after.subname
      || before.minlevel != after.minlevel || before.maxlevel != after.maxlevel
      || before.faction != after.faction || before.template_display != after.template_display
      || before.speed_walk != after.speed_walk || before.speed_run != after.speed_run)
    changed_domains |= EditTemplate;
  if (before.preview_movement != after.preview_movement
      || before.preview_animation != after.preview_animation
      || before.preview_animation_text != after.preview_animation_text)
    changed_domains |= EditPreview;
  if (!changed_domains) return;
  domains |= changed_domains;
  if (NOGGIT_ACTION_MGR->getCurrentAction()) return;
  std::uint64_t const guid = *_selected_spawn_guid;
  QPointer<NpcTemplateBrowser> const browser(this);
  auto* action = NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eNPC_EDIT);
  action->setNpcEdit(guid, domains, [browser, guid, domains, before, after](bool redo)
  {
    if (!browser || !browser->_map_view) return;
    browser->_pending_transform_edit = false;
    if (!browser->_selected_spawn_guid || *browser->_selected_spawn_guid != guid)
    {
      QScopedValueRollback<bool> const restoring(browser->_restoring_history, true);
      unsigned const entry = browser->_map_view->getWorld()->findNpcSpawnOverlay(guid)
        ? browser->_map_view->getWorld()->findNpcSpawnOverlay(guid)->entry() : 0;
      browser->selectSpawn(guid, entry);
    }
    if (browser->_selected_spawn_guid && *browser->_selected_spawn_guid == guid)
      browser->applyEditorSnapshot(redo ? after : before, domains);
  });
  NOGGIT_ACTION_MGR->endAction();
  _history_snapshot = after;
  _draft_snapshots[guid] = after;
}

void NpcTemplateBrowser::finishNpcTransformEdit()
{
  if (!_pending_transform_edit) return;
  _pending_transform_edit = false;
  recordNpcEdit(EditTransform);
}

void NpcTemplateBrowser::commitNpcEdits(unsigned domains)
{
  if (!_selected_spawn_guid) return;
  finishNpcTransformEdit();
  NOGGIT_ACTION_MGR->discardNpcEdits(*_selected_spawn_guid, domains);
  _history_snapshot = captureEditorSnapshot();
  _draft_snapshots[*_selected_spawn_guid] = _history_snapshot;
}

namespace
{
  std::atomic<unsigned> connection_serial{0};

  bool useWorldDatabase(QString const& host, int port, QString const& database,
                        QString const& user, QString const& password, bool write,
                        std::function<bool(QSqlDatabase&, QString&)> const& action,
                        QString& error)
  {
    if (host.trimmed().isEmpty() || database.trimmed().isEmpty() || user.trimmed().isEmpty())
    {
      error = "Enter a server, world database, and user.";
      return false;
    }
    if (!QSqlDatabase::isDriverAvailable("QMYSQL"))
    {
      error = "Qt MySQL driver is unavailable.";
      return false;
    }
    QString const name = QString("npc_spawn_editor_%1").arg(++connection_serial);
    bool success = false;
    {
      QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", name);
      db.setHostName(host.trimmed());
      db.setPort(port);
      db.setDatabaseName(database.trimmed());
      db.setUserName(user.trimmed());
      db.setPassword(password);
      db.setConnectOptions("MYSQL_OPT_CONNECT_TIMEOUT=5");
      if (!db.open()) error = db.lastError().text();
      else
      {
        if (write)
        {
          QSqlQuery autocommit(db);
          if (!autocommit.exec("SET SESSION autocommit = 1"))
            error = "Could not enable autocommit: " + autocommit.lastError().text();
          else
            success = action(db, error);
        }
        else
          success = action(db, error);
        db.close();
      }
    }
    QSqlDatabase::removeDatabase(name);
    return success;
  }

  int fieldIndex(QSqlRecord const& record, QString const& name)
  {
    for (int i = 0; i < record.count(); ++i)
      if (record.fieldName(i).compare(name, Qt::CaseInsensitive) == 0) return i;
    return -1;
  }

  QVariant field(QSqlQuery const& query, QString const& name)
  {
    int const index = fieldIndex(query.record(), name);
    return index < 0 ? QVariant() : query.value(index);
  }

  bool tableExists(QSqlDatabase& db, QString const& table)
  {
    QSqlQuery query(db);
    query.prepare("SHOW TABLES LIKE ?");
    query.addBindValue(table);
    return query.exec() && query.next();
  }

  bool saveDialogueToSharedTemplate(
    QSqlDatabase& db, unsigned entry, std::uint64_t spawn_guid,
    std::vector<NpcDialogueLineChange> const& lines,
    std::vector<NpcDialogueLineUpdate> const& line_updates,
    std::vector<NpcDialogueSoundChange> const& sounds,
    std::vector<NpcDialogueTriggerChange> const& triggers,
    std::vector<NpcDialogueFacingChange> const& facing_changes,
    QString const& database_name, QString& failure)
  {
    QString const lock_name = QString("noggit_npc_editor_%1")
      .arg(database_name.trimmed()).left(64);
    QSqlQuery lock(db);
    lock.prepare("SELECT GET_LOCK(?, 5)");
    lock.addBindValue(lock_name);
    if (!lock.exec() || !lock.next() || lock.value(0).toInt() != 1)
    {
      failure = "Could not acquire the NPC dialogue save lock.";
      return false;
    }
    lock.finish();
    auto unlock = [&]
    {
      QSqlQuery release(db);
      release.prepare("SELECT RELEASE_LOCK(?)");
      release.addBindValue(lock_name);
      release.exec();
    };
    QSqlQuery begin(db);
    if (!begin.exec("START TRANSACTION"))
    {
      failure = "Could not start the dialogue save transaction: " + begin.lastError().text();
      unlock();
      return false;
    }
    std::vector<std::pair<unsigned, unsigned>> inserted_lines;
    std::vector<std::pair<qlonglong, unsigned>> inserted_triggers;
    std::vector<NpcDialogueLineChange> original_lines;
    std::vector<std::tuple<unsigned, unsigned, unsigned>> original_sounds;
    std::vector<std::tuple<qlonglong, unsigned, unsigned>> original_facing;
    QSet<QString> text_columns;
    QString original_ai;
    bool ai_changed = false;
    auto rollback = [&](QString message)
    {
      QSqlQuery undo(db);
      undo.exec("ROLLBACK");
      // Legacy creature_text and smart_scripts may be nontransactional.
      for (auto const& [owner, id] : inserted_triggers)
      {
        QSqlQuery remove(db);
        remove.prepare("DELETE FROM smart_scripts WHERE entryorguid = ? "
                       "AND source_type = 0 AND id = ? AND link = 0");
        remove.addBindValue(owner);
        remove.addBindValue(id);
        if (!remove.exec()) message += " Trigger cleanup failed: " + remove.lastError().text();
      }
      for (auto const& [group, id] : inserted_lines)
      {
        QSqlQuery remove(db);
        remove.prepare("DELETE FROM creature_text WHERE CreatureID = ? AND GroupID = ? AND ID = ?");
        remove.addBindValue(entry);
        remove.addBindValue(group);
        remove.addBindValue(id);
        if (!remove.exec()) message += " Dialogue cleanup failed: " + remove.lastError().text();
      }
      for (NpcDialogueLineChange const& line : original_lines)
      {
        QStringList assignments;
        QVariantList values;
        auto add = [&](QString const& column, QVariant const& value)
        {
          if (!text_columns.contains(column.toLower())) return;
          assignments.push_back("`" + column + "` = ?");
          values.push_back(value);
        };
        add("Text", line.text);
        add("Type", line.chat_type);
        add("Language", line.language);
        add("Probability", line.probability);
        add("Emote", line.emote);
        add("Duration", line.duration);
        add("Sound", line.sound);
        add("BroadcastTextId", line.broadcast_text_id);
        add("TextRange", line.text_range);
        add("comment", line.comment);
        QSqlQuery restore(db);
        restore.prepare(QString("UPDATE creature_text SET %1 WHERE CreatureID = ? "
                                "AND GroupID = ? AND ID = ?").arg(assignments.join(", ")));
        for (QVariant const& value : values) restore.addBindValue(value);
        restore.addBindValue(entry);
        restore.addBindValue(line.group_id);
        restore.addBindValue(line.line_id);
        if (!restore.exec()) message += " Line cleanup failed: " + restore.lastError().text();
      }
      for (auto const& [group, id, sound] : original_sounds)
      {
        QSqlQuery restore(db);
        restore.prepare("UPDATE creature_text SET Sound = ? "
                        "WHERE CreatureID = ? AND GroupID = ? AND ID = ?");
        restore.addBindValue(sound);
        restore.addBindValue(entry);
        restore.addBindValue(group);
        restore.addBindValue(id);
        if (!restore.exec()) message += " Sound cleanup failed: " + restore.lastError().text();
      }
      for (auto const& [owner, id, duration] : original_facing)
      {
        QSqlQuery restore(db);
        restore.prepare("UPDATE smart_scripts SET action_param5 = ? WHERE entryorguid = ? "
                        "AND source_type = 0 AND id = ? AND action_type = 1");
        restore.addBindValue(duration);
        restore.addBindValue(owner);
        restore.addBindValue(id);
        if (!restore.exec()) message += " Facing cleanup failed: " + restore.lastError().text();
      }
      if (ai_changed)
      {
        QSqlQuery restore(db);
        restore.prepare("UPDATE creature_template SET AIName = ? WHERE entry = ?");
        restore.addBindValue(original_ai);
        restore.addBindValue(entry);
        if (!restore.exec()) message += " AI cleanup failed: " + restore.lastError().text();
      }
      unlock();
      failure = std::move(message);
      return false;
    };

    QSqlQuery source(db);
    source.prepare("SELECT AIName, ScriptName FROM creature_template WHERE entry = ? FOR UPDATE");
    source.addBindValue(entry);
    if (!source.exec() || !source.next())
      return rollback("The source NPC template could not be found: " + source.lastError().text());
    original_ai = source.value(0).toString();
    QString const script_name = source.value(1).toString();
    source.finish();
    if ((!triggers.empty() || !facing_changes.empty()) && (!script_name.isEmpty()
        || (!original_ai.isEmpty() && original_ai.compare("SmartAI", Qt::CaseInsensitive) != 0)))
      return rollback("This shared template uses another AI or C++ script; SmartAI triggers cannot be added.");

    QString column_error;
    if ((!lines.empty() || !line_updates.empty() || !sounds.empty())
        && !tableExists(db, "creature_text"))
      return rollback("This database has no creature_text table.");
    if ((!lines.empty() || !line_updates.empty() || !sounds.empty())
        && !Noggit::Sql::worldTableColumns(db, "creature_text", text_columns, column_error))
      return rollback("Could not inspect creature_text: " + column_error);
    for (char const* required : {"creatureid", "groupid", "id", "text", "type",
                                 "language", "probability", "emote", "duration", "sound"})
      if ((!lines.empty() || !line_updates.empty()) && !text_columns.contains(required))
        return rollback(QString("creature_text is missing required column %1.").arg(required));
    QSet<QString> smart_columns;
    if ((!triggers.empty() || !facing_changes.empty()) && !tableExists(db, "smart_scripts"))
      return rollback("This database has no smart_scripts table.");
    if ((!triggers.empty() || !facing_changes.empty())
        && !Noggit::Sql::worldTableColumns(db, "smart_scripts", smart_columns, column_error))
      return rollback("Could not inspect smart_scripts: " + column_error);
    for (char const* required : {"entryorguid", "source_type", "id", "link", "event_type",
                                 "event_phase_mask", "event_chance", "event_flags",
                                 "event_param1", "event_param2", "event_param3", "event_param4",
                                 "action_type", "action_param1", "action_param2", "action_param3",
                                 "action_param4", "action_param5", "action_param6", "target_type",
                                 "target_param1", "target_param2", "target_param3", "target_x",
                                 "target_y", "target_z", "target_o", "comment"})
      if ((!triggers.empty() || !facing_changes.empty()) && !smart_columns.contains(required))
        return rollback(QString("smart_scripts is missing required column %1.").arg(required));

    for (NpcDialogueLineChange const& line : lines)
    {
      if (line.broadcast_text_id)
      {
        if (!tableExists(db, "broadcast_text"))
          return rollback("This database has no broadcast_text table for the dialogue link.");
        QSqlQuery broadcast(db);
        broadcast.prepare("SELECT 1 FROM broadcast_text WHERE ID = ? LIMIT 1");
        broadcast.addBindValue(line.broadcast_text_id);
        if (!broadcast.exec() || !broadcast.next())
          return rollback(QString("BroadcastText %1 does not exist.").arg(line.broadcast_text_id));
      }
      QStringList columns;
      QStringList placeholders;
      QVariantList values;
      auto add = [&](QString const& column, QVariant const& value)
      {
        if (!text_columns.contains(column.toLower())) return;
        columns.push_back("`" + column + "`");
        placeholders.push_back("?");
        values.push_back(value);
      };
      add("CreatureID", entry);
      add("GroupID", line.group_id);
      add("ID", line.line_id);
      add("Text", line.text);
      add("Type", line.chat_type);
      add("Language", line.language);
      add("Probability", line.probability);
      add("Emote", line.emote);
      add("Duration", line.duration);
      add("Sound", line.sound);
      add("BroadcastTextId", line.broadcast_text_id);
      add("TextRange", line.text_range);
      add("comment", line.comment);
      QSqlQuery insert(db);
      insert.prepare(QString("INSERT INTO creature_text (%1) VALUES (%2)")
        .arg(columns.join(", "), placeholders.join(", ")));
      for (QVariant const& value : values) insert.addBindValue(value);
      if (!insert.exec())
        return rollback(QString("Could not add dialogue set %1, line %2: %3")
          .arg(line.group_id).arg(line.line_id).arg(insert.lastError().text()));
      inserted_lines.emplace_back(line.group_id, line.line_id);
    }
    for (NpcDialogueLineUpdate const& change : line_updates)
    {
      auto const& line = change.replacement;
      if (line.broadcast_text_id)
      {
        if (!tableExists(db, "broadcast_text"))
          return rollback("This database has no broadcast_text table for the dialogue link.");
        QSqlQuery broadcast(db);
        broadcast.prepare("SELECT 1 FROM broadcast_text WHERE ID = ? LIMIT 1");
        broadcast.addBindValue(line.broadcast_text_id);
        if (!broadcast.exec() || !broadcast.next())
          return rollback(QString("BroadcastText %1 does not exist.").arg(line.broadcast_text_id));
      }
      QStringList assignments;
      QVariantList values;
      auto add = [&](QString const& column, QVariant const& value)
      {
        if (!text_columns.contains(column.toLower())) return;
        assignments.push_back("`" + column + "` = ?");
        values.push_back(value);
      };
      add("Text", line.text);
      add("Type", line.chat_type);
      add("Language", line.language);
      add("Probability", line.probability);
      add("Emote", line.emote);
      add("Duration", line.duration);
      add("Sound", line.sound);
      add("BroadcastTextId", line.broadcast_text_id);
      add("TextRange", line.text_range);
      add("comment", line.comment);
      QSqlQuery update(db);
      update.prepare(QString("UPDATE creature_text SET %1 WHERE CreatureID = ? "
                             "AND GroupID = ? AND ID = ?").arg(assignments.join(", ")));
      for (QVariant const& value : values) update.addBindValue(value);
      update.addBindValue(entry);
      update.addBindValue(line.group_id);
      update.addBindValue(line.line_id);
      if (!update.exec() || update.numRowsAffected() != 1)
        return rollback(QString("Could not update dialogue set %1, line %2: %3")
          .arg(line.group_id).arg(line.line_id)
          .arg(update.lastError().isValid() ? update.lastError().text()
                                            : "The dialogue row was not found."));
      original_lines.push_back(change.original);
    }
    for (NpcDialogueSoundChange const& sound : sounds)
    {
      QSqlQuery previous(db);
      previous.prepare("SELECT Sound FROM creature_text "
                       "WHERE CreatureID = ? AND GroupID = ? AND ID = ?");
      previous.addBindValue(entry);
      previous.addBindValue(sound.group_id);
      previous.addBindValue(sound.line_id);
      if (!previous.exec() || !previous.next())
        return rollback("A dialogue line to update no longer exists.");
      unsigned const old_sound = previous.value(0).toUInt();
      previous.finish();
      QSqlQuery update(db);
      update.prepare("UPDATE creature_text SET Sound = ? "
                     "WHERE CreatureID = ? AND GroupID = ? AND ID = ?");
      update.addBindValue(sound.sound);
      update.addBindValue(entry);
      update.addBindValue(sound.group_id);
      update.addBindValue(sound.line_id);
      if (!update.exec() || update.numRowsAffected() != 1)
        return rollback("Could not update the dialogue sound: " + update.lastError().text());
      original_sounds.emplace_back(sound.group_id, sound.line_id, old_sound);
    }
    for (NpcDialogueTriggerChange const& trigger : triggers)
    {
      if (trigger.selected_spawn_only && !spawn_guid)
        return rollback("A selected-spawn trigger needs a placed NPC.");
      qlonglong const owner = trigger.selected_spawn_only
        ? -static_cast<qlonglong>(spawn_guid) : static_cast<qlonglong>(entry);
      QSqlQuery next_id(db);
      next_id.prepare("SELECT COALESCE(MAX(id) + 1, 0) FROM smart_scripts "
                      "WHERE entryorguid = ? AND source_type = 0");
      next_id.addBindValue(owner);
      if (!next_id.exec() || !next_id.next())
        return rollback("Could not allocate a SmartAI trigger ID: " + next_id.lastError().text());
      unsigned const smart_id = next_id.value(0).toUInt();
      next_id.finish();
      QStringList columns;
      QStringList placeholders;
      QVariantList values;
      auto add = [&](QString const& column, QVariant const& value)
      {
        if (!smart_columns.contains(column.toLower())) return;
        columns.push_back("`" + column + "`");
        placeholders.push_back("?");
        values.push_back(value);
      };
      add("entryorguid", owner);
      add("source_type", 0);
      add("id", smart_id);
      add("link", 0);
      add("event_type", trigger.event_type);
      add("event_phase_mask", 0);
      add("event_chance", trigger.event_chance);
      add("event_flags", 0);
      for (unsigned p = 0; p < trigger.event_params.size(); ++p)
        add(QString("event_param%1").arg(p + 1), trigger.event_params[p]);
      add("action_type", 1);
      add("action_param1", trigger.group_id);
      add("action_param2", 0);
      add("action_param3", 0);
      add("action_param4", trigger.speech_delay);
      add("action_param5", trigger.face_player_duration);
      add("action_param6", 0);
      add("target_type", 1);
      for (unsigned p = 1; p <= 4; ++p)
        add(QString("target_param%1").arg(p), 0);
      for (char const* column : {"target_x", "target_y", "target_z", "target_o"}) add(column, 0);
      add("comment", QString("Noggit: %1 -> dialogue group %2")
        .arg(trigger.event_label).arg(trigger.group_id));
      QSqlQuery insert(db);
      insert.prepare(QString("INSERT INTO smart_scripts (%1) VALUES (%2)")
        .arg(columns.join(", "), placeholders.join(", ")));
      for (QVariant const& value : values) insert.addBindValue(value);
      if (!insert.exec())
        return rollback("Could not save the dialogue trigger: " + insert.lastError().text());
      inserted_triggers.emplace_back(owner, smart_id);
    }
    for (NpcDialogueFacingChange const& change : facing_changes)
    {
      qlonglong const owner = static_cast<qlonglong>(change.smart_owner);
      if (owner != static_cast<qlonglong>(entry)
          && (!spawn_guid || owner != -static_cast<qlonglong>(spawn_guid)))
        return rollback("An existing dialogue trigger has a different owner.");
      QSqlQuery previous(db);
      previous.prepare("SELECT action_param5 FROM smart_scripts WHERE entryorguid = ? "
                       "AND source_type = 0 AND id = ? AND action_type = 1 "
                       "AND action_param1 = ? FOR UPDATE");
      previous.addBindValue(owner);
      previous.addBindValue(change.smart_id);
      previous.addBindValue(change.group_id);
      if (!previous.exec() || !previous.next()
          || previous.value(0).toUInt() != change.original_duration)
        return rollback("An existing dialogue trigger changed before facing could be saved.");
      previous.finish();
      QSqlQuery update(db);
      update.prepare("UPDATE smart_scripts SET action_param5 = ? WHERE entryorguid = ? "
                     "AND source_type = 0 AND id = ? AND action_type = 1 "
                     "AND action_param1 = ? AND action_param5 = ?");
      update.addBindValue(change.duration);
      update.addBindValue(owner);
      update.addBindValue(change.smart_id);
      update.addBindValue(change.group_id);
      update.addBindValue(change.original_duration);
      if (!update.exec() || update.numRowsAffected() != 1)
        return rollback("Could not update dialogue facing: " + update.lastError().text());
      original_facing.emplace_back(owner, change.smart_id, change.original_duration);
    }
    if ((!triggers.empty() || !facing_changes.empty())
        && original_ai.compare("SmartAI", Qt::CaseInsensitive) != 0)
    {
      QSqlQuery enable(db);
      enable.prepare("UPDATE creature_template SET AIName = 'SmartAI' "
                     "WHERE entry = ? AND COALESCE(ScriptName, '') = ''");
      enable.addBindValue(entry);
      if (!enable.exec() || enable.numRowsAffected() != 1)
        return rollback("Could not enable SmartAI for the shared template: "
                        + enable.lastError().text());
      ai_changed = true;
    }
    QSqlQuery commit(db);
    if (!commit.exec("COMMIT"))
      return rollback("Could not commit the shared dialogue: " + commit.lastError().text());
    unlock();
    return true;
  }

  QString smartEventLabel(unsigned type, unsigned param1 = 0, unsigned param2 = 0,
                          unsigned param3 = 0, unsigned param4 = 0)
  {
    switch (type)
    {
      case 0: return QString("While in combat (%1–%2 ms, repeat %3–%4 ms)")
                       .arg(param1).arg(param2).arg(param3).arg(param4);
      case 1: return QString("While out of combat (%1–%2 ms, repeat %3–%4 ms)")
                       .arg(param1).arg(param2).arg(param3).arg(param4);
      case 2: return QString("At %1–%2% health").arg(param1).arg(param2);
      case 3: return QString("At %1–%2% mana").arg(param1).arg(param2);
      case 4: return "On aggro";
      case 5: return "After killing a creature";
      case 6: return "On death";
      case 7: return "On evade";
      case 8: return param1 ? QString("When hit by spell %1").arg(param1)
                            : "When hit by a spell";
      case 9: return QString("When the victim is %1–%2 yards away").arg(param1).arg(param2);
      case 10: return QString("When a target is within %1 yards out of combat").arg(param2);
      case 11: return "On respawn";
      case 17: return param1 ? QString("After summoning creature %1").arg(param1)
                             : "After summoning a creature";
      case 19: return param1 ? QString("When quest %1 is accepted").arg(param1)
                             : "When a quest is accepted";
      case 20: return param1 ? QString("When quest %1 is rewarded").arg(param1)
                             : "When a quest is rewarded";
      case 21: return "On reaching home";
      case 22: return QString("On emote %1").arg(param1);
      case 25: return "On reset or spawn";
      case 26: return QString("When a target is within %1 yards in combat").arg(param2);
      case 34: return QString("On movement point %1").arg(param2);
      case 38: return QString("When data field %1 becomes %2").arg(param1).arg(param2);
      case 52: return QString("After dialogue group %1 finishes").arg(param1);
      case 54: return "Immediately after being summoned";
      case 59: return QString("When timed event %1 fires").arg(param1);
      case 60: return QString("On update (%1–%2 ms)").arg(param1).arg(param2);
      case 61: return "On linked SmartAI event";
      case 77: return "On counter set";
      case 108: return QString("On waypoint %1").arg(param1);
      case 109: return "When waypoint path ends";
      default: return QString("SmartAI event %1").arg(type);
    }
  }

  bool setWeaponAttachment(Tools::AssetBrowser::ModelViewer* preview,
                           DBCFile* item_display_info,
                           std::shared_ptr<BlizzardArchive::ClientData> const& client_data,
                           unsigned display_id, unsigned inventory_type,
                           unsigned attachment_id,
                           unsigned render_attachment_id = 0)
  {
    if (!preview || !display_id || !item_display_info || !client_data) return false;
    try
    {
      auto const display_record = item_display_info->getByID(display_id);
      std::string stem = display_record.getString(1);
      std::replace(stem.begin(), stem.end(), '/', '\\');
      if (stem.empty()) return false;
      QString const stem_name = QString::fromStdString(stem);
      if (stem_name.endsWith(".mdx", Qt::CaseInsensitive)
          || stem_name.endsWith(".mdl", Qt::CaseInsensitive)
          || stem_name.endsWith(".m2", Qt::CaseInsensitive))
        stem.resize(stem.find_last_of('.'));

      std::vector<std::string> directories;
      if (stem.find('\\') != std::string::npos)
        directories.emplace_back();
      else if (inventory_type == 14 || inventory_type == 23)
        directories = {"Item\\ObjectComponents\\Shield\\",
                       "Item\\ObjectComponents\\Weapon\\",
                       "Item\\ObjectComponents\\Misc\\"};
      else
        directories = {"Item\\ObjectComponents\\Weapon\\",
                       "Item\\ObjectComponents\\Shield\\",
                       "Item\\ObjectComponents\\Misc\\"};

      std::string model_path;
      for (std::string const& directory : directories)
      {
        std::string const candidate = directory + stem + ".m2";
        if (client_data->exists(BlizzardArchive::Listfile::FileKey(candidate)))
        {
          model_path = candidate;
          break;
        }
      }
      if (model_path.empty()) return false;

      std::string texture_path = display_record.getString(3);
      std::replace(texture_path.begin(), texture_path.end(), '/', '\\');
      if (!texture_path.empty())
      {
        if (texture_path.find('\\') == std::string::npos)
          texture_path = model_path.substr(0, model_path.find_last_of("\\/") + 1)
              + texture_path;
        if (!QString::fromStdString(texture_path).endsWith(".blp", Qt::CaseInsensitive))
          texture_path += ".blp";
        if (!client_data->exists(BlizzardArchive::Listfile::FileKey(texture_path)))
          texture_path.clear();
      }

      // Attachment 0 is the character shield socket. Attachment 2 is the
      // generic left-hand socket used by off-hand weapons and held objects.
      if (inventory_type == 14 && attachment_id == 2)
        attachment_id = 0;
      return preview->setCreatureAttachment(attachment_id, model_path, texture_path,
                                            render_attachment_id);
    }
    catch (...) { return false; }
  }

  float normalizedYaw(float yaw)
  {
    yaw = std::fmod(yaw, 360.0f);
    if (yaw > 180.0f) yaw -= 360.0f;
    if (yaw <= -180.0f) yaw += 360.0f;
    return yaw;
  }

  float serverOrientation(float yaw)
  {
    float angle = static_cast<float>(3.141592653589793)
      + normalizedYaw(yaw) * static_cast<float>(3.141592653589793 / 180.0);
    angle = std::fmod(angle, static_cast<float>(2.0 * 3.141592653589793));
    return angle < 0.0f ? angle + static_cast<float>(2.0 * 3.141592653589793)
      : angle;
  }

  glm::vec3 fromServerPosition(double x, double y, double z)
  {
    return {static_cast<float>(ZEROPOINT - y), static_cast<float>(z),
            static_cast<float>(ZEROPOINT - x)};
  }

  std::string characterTexturePath(std::string path)
  {
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.empty()) return {};
    QString const name = QString::fromStdString(path);
    if (!name.endsWith(".blp", Qt::CaseInsensitive)) path += ".blp";
    return path;
  }

  QImage characterTextureImage(std::string const& path, QSize const& size = {})
  {
    if (path.empty()) return {};
    try
    {
      QPixmap* pixmap = Noggit::BLPRenderer::getInstance().render_blp_to_pixmap(
        path, size.isValid() ? size.width() : -1, size.isValid() ? size.height() : -1,
        true);
      return pixmap ? pixmap->toImage().convertToFormat(QImage::Format_ARGB32) : QImage{};
    }
    catch (...) { return {}; }
  }

  bool writeCharacterBlp(QImage const& source, QString const& logical_path,
                         std::shared_ptr<BlizzardArchive::ClientData> const& client_data,
                         QString& error)
  {
    if (source.isNull() || !client_data)
    {
      error = "The character body texture could not be composed.";
      return false;
    }
    QString relative = logical_path;
    relative.replace('\\', '/');
    QString const disk_path = QDir(QString::fromStdString(client_data->projectPath()))
      .filePath(relative);
    QFileInfo const info(disk_path);
    if (!QDir().mkpath(info.absolutePath()))
    {
      error = "Could not create the custom NPC texture directory.";
      return false;
    }

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !source.save(&buffer, "PNG"))
    {
      error = "Could not encode the composed character texture.";
      return false;
    }

    Png2Blp encoder;
    encoder.load(png.constData(), static_cast<uint32_t>(png.size()));
    uint32_t byte_count = 0;
    void* bytes = encoder.createBlpDxtInMemory(true, FORMAT_DXT5, byte_count);
    if (!bytes || !byte_count)
    {
      error = "Could not encode the composed character texture as BLP.";
      std::free(bytes);
      return false;
    }
    // PNG2BLP writes DXT5 blocks but leaves the BLP2 alpha-depth byte at zero.
    // Some readers then treat each 16-byte block as DXT1, corrupting small face
    // details (most visibly lips and eyes). DXT5 is eight-bit alpha in BLP2.
    constexpr uint32_t blp2_alpha_depth_offset = 9;
    if (byte_count > blp2_alpha_depth_offset)
      static_cast<unsigned char*>(bytes)[blp2_alpha_depth_offset] = 8;
    QSaveFile output(disk_path);
    bool const written = output.open(QIODevice::WriteOnly)
      && output.write(static_cast<char const*>(bytes), byte_count) == byte_count
      && output.commit();
    std::free(bytes);
    if (!written)
    {
      error = "Could not write " + disk_path + ".";
      return false;
    }
    return true;
  }

  QString deploymentDirectory(QString const& configured_root, QString const& leaf)
  {
    QDir const root(configured_root.trimmed());
    return root.dirName().compare(leaf, Qt::CaseInsensitive) == 0
      ? root.absolutePath() : root.filePath(leaf);
  }

  bool copyFileAtomically(QString const& source_path, QString const& destination_path,
                          QString& error)
  {
    QString const source = QDir::cleanPath(QFileInfo(source_path).absoluteFilePath());
    QString const destination = QDir::cleanPath(QFileInfo(destination_path).absoluteFilePath());
    if (source.compare(destination, Qt::CaseInsensitive) == 0) return true;

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
    {
      error = QString("Could not read %1: %2").arg(source, input.errorString());
      return false;
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly))
    {
      error = QString("Could not open %1 for writing: %2")
        .arg(destination, output.errorString());
      return false;
    }
    while (!input.atEnd())
    {
      QByteArray const bytes = input.read(1024 * 1024);
      if (bytes.isEmpty() && input.error() != QFile::NoError)
      {
        error = QString("Could not read %1: %2").arg(source, input.errorString());
        output.cancelWriting();
        return false;
      }
      if (output.write(bytes) != bytes.size())
      {
        error = QString("Could not write %1: %2").arg(destination, output.errorString());
        output.cancelWriting();
        return false;
      }
    }
    if (!output.commit())
    {
      error = QString("Could not replace %1: %2").arg(destination, output.errorString());
      return false;
    }
    return true;
  }
}

bool NpcTemplateBrowser::npcDbcDeploymentConfigured(QString& error) const
{
  if (!_client_data_folder || !_server_data_folder)
  {
    error = "The NPC DBC deployment controls are unavailable.";
    return false;
  }
  QString const client_root = _client_data_folder->text().trimmed();
  QString const server_root = _server_data_folder->text().trimmed();
  if (client_root.isEmpty() || server_root.isEmpty())
  {
    error = "Configure both the client Data folder and server data folder under "
            "World database connection.";
    return false;
  }
  if (!QDir(client_root).exists())
  {
    error = "The configured client Data folder does not exist: " + client_root;
    return false;
  }
  if (!QDir(server_root).exists())
  {
    error = "The configured server data folder does not exist: " + server_root;
    return false;
  }
  return true;
}

bool NpcTemplateBrowser::deployNpcDbcs(QString& error) const
{
  if (!npcDbcDeploymentConfigured(error)) return false;

  QSettings settings;
  QString const client_root = _client_data_folder->text().trimmed();
  QString const server_root = _server_data_folder->text().trimmed();
  settings.setValue("npc_browser/client_data_folder", client_root);
  settings.setValue("npc_browser/server_data_folder", server_root);

  QString const project_root = QString::fromStdString(_client_data->projectPath());
  QString const source_directory = QDir(project_root).filePath("DBFilesClient");
  QStringList files = {"CreatureDisplayInfo.dbc", "CreatureDisplayInfoExtra.dbc"};
  for (QString const& sound_file : {QString("NPCSounds.dbc"),
                                    QString("CreatureSoundData.dbc")})
    if (QFile::exists(QDir(source_directory).filePath(sound_file)))
      files.push_back(sound_file);
  QStringList const destinations = {
    deploymentDirectory(client_root, "DBFilesClient"),
    deploymentDirectory(server_root, "dbc")
  };
  for (QString const& directory : destinations)
  {
    if (!QDir().mkpath(directory))
    {
      error = "Could not create the DBC deployment directory: " + directory;
      return false;
    }
    for (QString const& file : files)
      if (!copyFileAtomically(QDir(source_directory).filePath(file),
                              QDir(directory).filePath(file), error))
        return false;
  }
  return true;
}

QString NpcTemplateBrowser::weaponVisibilitySettingsKey(std::uint64_t guid) const
{
  if (!_map_view || !_map_view->getWorld()) return {};
  // A GUID is only unique within one world database. Keep preview choices
  // separate across connections and maps without putting names into QSettings
  // path segments unescaped.
  return QString("npc_browser/weapon_visibility/%1/%2/%3/%4/%5")
    .arg(QString::fromLatin1(_host->text().trimmed().toLower().toUtf8().toHex()))
    .arg(_port->value())
    .arg(QString::fromLatin1(_database->text().trimmed().toLower().toUtf8().toHex()))
    .arg(_map_view->getWorld()->getMapID())
    .arg(static_cast<qulonglong>(guid));
}

void NpcTemplateBrowser::applyWeaponVisibility(std::uint64_t guid)
{
  if (!_map_view || !_map_view->getWorld()) return;
  auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(guid);
  if (!overlay) return;
  QSettings settings;
  QString const key = weaponVisibilitySettingsKey(guid);
  bool const main_off_hand = settings.value(key + "/main_off_hand",
    overlay->showsMainOffHand()).toBool();
  bool const ranged = settings.value(key + "/ranged",
    overlay->showsRangedWeapon()).toBool();
  overlay->setWeaponVisibility(main_off_hand, ranged);
  if (_selected_spawn_guid && *_selected_spawn_guid == guid)
  {
    QSignalBlocker const main_blocker(_show_main_off_hand);
    QSignalBlocker const ranged_blocker(_show_ranged_weapon);
    _show_main_off_hand->setChecked(main_off_hand);
    _show_ranged_weapon->setChecked(ranged);
  }
  _map_view->invalidate();
  _map_view->update();
}

void NpcTemplateBrowser::saveSelectedWeaponVisibility()
{
  if (!_selected_spawn_guid || !_map_view || !_map_view->getWorld()) return;
  auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid);
  if (!overlay) return;
  bool const main_off_hand = _show_main_off_hand->isChecked();
  bool const ranged = _show_ranged_weapon->isChecked();
  overlay->setWeaponVisibility(main_off_hand, ranged);
  QSettings settings;
  QString const key = weaponVisibilitySettingsKey(*_selected_spawn_guid);
  settings.setValue(key + "/main_off_hand", main_off_hand);
  settings.setValue(key + "/ranged", ranged);
  _map_view->invalidate();
  _map_view->update();
}

namespace
{
  bool eventGroupVisible(std::vector<int> const& memberships,
                         std::unordered_set<int> const& active_events)
  {
    bool requires_event = false;
    bool matching_event = false;
    for (int const signed_event : memberships)
    {
      if (signed_event > 0)
      {
        requires_event = true;
        matching_event = matching_event || active_events.contains(signed_event);
      }
      else if (signed_event < 0 && active_events.contains(-signed_event))
        return false;
    }
    return !requires_event || matching_event;
  }
}

bool NpcTemplateBrowser::eventVisible(std::vector<int> const& direct_events,
                                      std::vector<int> const& pool_events) const
{
  return eventGroupVisible(direct_events, _active_event_ids)
    && eventGroupVisible(pool_events, _active_event_ids);
}

void NpcTemplateBrowser::reconcileSelectedSpawnVisibility()
{
  if (_selected_gameobject_guid)
  {
    auto const found = _cached_gameobjects.find(*_selected_gameobject_guid);
    if (found == _cached_gameobjects.end()
        || !(found->second.phase_mask & _active_phase_mask)
        || !eventVisible(found->second.direct_events, found->second.pool_events))
    {
      _selected_gameobject_guid.reset();
      if (_phase_assignment_panel) _phase_assignment_panel->hide();
    }
  }
  if (_selected_spawn_guid && _map_view && _map_view->getWorld())
  {
    auto const found = _cached_spawns.find(*_selected_spawn_guid);
    if (found != _cached_spawns.end()
        && (!(found->second.phase_mask & _active_phase_mask)
            || !eventVisible(found->second.direct_events, found->second.pool_events)))
    {
      finishNpcTransformEdit();
      _draft_snapshots[*_selected_spawn_guid] = captureEditorSnapshot();
      _map_view->getWorld()->selectNpcSpawnOverlay({});
      _selected_spawn_guid.reset();
      _selected_spawn_rotating = false;
      _moving_spawn = false;
      _capturing_waypoints = false;
      _spawn_editor->hide();
      if (_phase_assignment_panel) _phase_assignment_panel->hide();
      if (_waypoint_panel) _waypoint_panel->setEnabled(false);
      if (_move_spawn_button) _move_spawn_button->setText("Move NPC by clicking terrain");
    }
  }
}

void NpcTemplateBrowser::setActivePhaseMask(unsigned mask)
{
  if (mask == _active_phase_mask) return;
  _active_phase_mask = mask;
  reconcileSelectedSpawnVisibility();
  updateVisibleCachedSpawns();
}

void NpcTemplateBrowser::showPhaseAssignment(bool gameobject, std::uint64_t guid,
                                              unsigned entry, unsigned mask)
{
  if (!_phase_assignment_panel || !_phase_assignment_mask) return;
  std::vector<int> const* direct_events = nullptr;
  std::vector<int> const* pool_events = nullptr;
  if (gameobject)
  {
    if (auto const found = _cached_gameobjects.find(guid);
        found != _cached_gameobjects.end())
    {
      direct_events = &found->second.direct_events;
      pool_events = &found->second.pool_events;
    }
  }
  else if (auto const found = _cached_spawns.find(guid);
           found != _cached_spawns.end())
  {
    direct_events = &found->second.direct_events;
    pool_events = &found->second.pool_events;
  }
  QStringList event_labels;
  auto add_event_labels = [&](std::vector<int> const* memberships)
  {
    if (!memberships) return;
    for (int const signed_event : *memberships)
    {
      auto const found = _event_descriptions.find(std::abs(signed_event));
      QString const name = found == _event_descriptions.end()
        ? QString::number(std::abs(signed_event)) : found->second;
      QString const label = signed_event < 0 ? "Absent during " + name : name;
      if (!event_labels.contains(label)) event_labels.push_back(label);
    }
  };
  add_event_labels(direct_events);
  add_event_labels(pool_events);
  _phase_assignment_identity->setText(
    QString("%1 GUID %2 · Template %3 · Current mask %4 · %5")
      .arg(gameobject ? "Gameobject" : "NPC")
      .arg(guid).arg(entry).arg(mask)
      .arg(event_labels.isEmpty() ? "Normal world" : event_labels.join(", ")));
  {
    QSignalBlocker const blocker(_phase_assignment_mask);
    _phase_assignment_mask->setEditText(QString::number(mask));
  }
  _phase_assignment_panel->show();
}

void NpcTemplateBrowser::selectGameObjectSpawn(std::uint64_t guid, unsigned entry)
{
  if (!_map_view || !_map_view->getWorld()) return;
  auto const found = _cached_gameobjects.find(guid);
  if (found == _cached_gameobjects.end()) return;
  finishNpcTransformEdit();
  if (_selected_spawn_guid)
  {
    _draft_snapshots[*_selected_spawn_guid] = captureEditorSnapshot();
    if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
    {
      overlay->setPreviewRoute({}, false);
      overlay->setTransform(_saved_spawn_position, _saved_spawn_yaw, overlay->scale());
    }
    _map_view->getWorld()->selectNpcSpawnOverlay({});
    _selected_spawn_guid.reset();
  }
  _selected_gameobject_guid = guid;
  _selected_spawn_rotating = false;
  _moving_spawn = false;
  _capturing_waypoints = false;
  _spawn_editor->hide();
  if (_phase_assignment_panel) _phase_assignment_panel->hide();
  if (_waypoint_panel) _waypoint_panel->setEnabled(false);
  showPropertiesPanel();
  showPhaseAssignment(true, guid, entry, found->second.phase_mask);
  _status->setText(QString("Selected gameobject GUID %1. Set its phase mask and save it to "
                           "the world database.").arg(guid));
}

void NpcTemplateBrowser::saveSelectedPhaseMask()
{
  if (!_map_view || !_map_view->getWorld()) return;
  bool const gameobject = _selected_gameobject_guid.has_value();
  std::optional<std::uint64_t> const guid = gameobject
    ? _selected_gameobject_guid : _selected_spawn_guid;
  if (!guid) return;
  bool valid = false;
  qulonglong const entered_mask = _phase_assignment_mask->currentText().toULongLong(&valid);
  if (!valid || entered_mask == 0
      || entered_mask > std::numeric_limits<unsigned>::max())
  {
    _status->setText("Enter a phase mask from 1 to 4294967295.");
    return;
  }
  unsigned const mask = static_cast<unsigned>(entered_mask);
  if (gameobject && !_cached_gameobjects.contains(*guid))
  {
    _status->setText("Refresh the map cache before editing this gameobject's phase.");
    return;
  }
  unsigned const entry = gameobject
    ? _cached_gameobjects.at(*guid).entry : _selected_spawn_entry;
  unsigned const map_id = _map_view->getWorld()->getMapID();
  QString error;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      QString const table = gameobject ? "`gameobject`" : "`creature`";
      QSqlQuery selected(db);
      selected.prepare(QString("SELECT `phaseMask` FROM %1 WHERE `guid` = ? AND `map` = ?")
        .arg(table));
      selected.addBindValue(QVariant::fromValue<qulonglong>(*guid));
      selected.addBindValue(map_id);
      if (!selected.exec() || !selected.next())
      {
        failure = selected.lastError().isValid() ? selected.lastError().text()
          : "The selected spawn no longer exists in the world database.";
        return false;
      }
      selected.finish();
      QSqlQuery update(db);
      update.prepare(QString("UPDATE %1 SET `phaseMask` = ? WHERE `guid` = ? AND `map` = ?")
        .arg(table));
      update.addBindValue(mask);
      update.addBindValue(QVariant::fromValue<qulonglong>(*guid));
      update.addBindValue(map_id);
      if (!update.exec())
      {
        failure = update.lastError().text();
        return false;
      }
      QSqlQuery verify(db);
      verify.prepare(QString("SELECT `phaseMask` FROM %1 WHERE `guid` = ? AND `map` = ?")
        .arg(table));
      verify.addBindValue(QVariant::fromValue<qulonglong>(*guid));
      verify.addBindValue(map_id);
      if (!verify.exec() || !verify.next() || verify.value(0).toUInt() != mask)
      {
        failure = verify.lastError().isValid() ? verify.lastError().text()
          : "The database did not retain the requested phase mask.";
        return false;
      }
      return true;
    }, error);
  if (!saved)
  {
    _status->setText("Could not save phase mask: " + error);
    return;
  }
  if (gameobject)
    _cached_gameobjects.at(*guid).phase_mask = mask;
  else if (auto found = _cached_spawns.find(*guid); found != _cached_spawns.end())
    found->second.phase_mask = mask;
  _status->setText(QString("Saved phase mask %1 for %2 GUID %3. Reload the world server "
                           "to apply it in game.")
    .arg(mask).arg(gameobject ? "gameobject" : "NPC").arg(*guid));
  if (!(mask & _active_phase_mask))
  {
    if (gameobject) _selected_gameobject_guid.reset();
    else
    {
      finishNpcTransformEdit();
      _draft_snapshots[*guid] = captureEditorSnapshot();
      _map_view->getWorld()->selectNpcSpawnOverlay({});
      _selected_spawn_guid.reset();
      _spawn_editor->hide();
      if (_waypoint_panel) _waypoint_panel->setEnabled(false);
    }
    _phase_assignment_panel->hide();
  }
  else
    showPhaseAssignment(gameobject, *guid, entry, mask);
  updateVisibleCachedSpawns();
}

void NpcTemplateBrowser::loadNearbySpawns()
{
  if (!_map_view || !_map_view->getWorld()) return;
  unsigned const map_id = _map_view->getWorld()->getMapID();
  std::unordered_map<std::uint64_t, CachedNpcSpawn> spawns;
  std::array<std::vector<std::uint64_t>, 64 * 64> by_tile;
  std::unordered_map<std::uint64_t, CachedGameObjectSpawn> gameobjects;
  std::array<std::vector<std::uint64_t>, 64 * 64> gameobjects_by_tile;
  std::map<int, QString> event_descriptions;
  QString error;
  bool const loaded = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    false, [&](QSqlDatabase& db, QString& failure)
    {
      Sql::WorldDatabaseSchema schema;
      if (!Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
      bool const equipment_available = tableExists(db, "creature_equip_template")
        && tableExists(db, "item_template");
      QString const spawn_display = schema.creatureSpawnDisplay("c").isEmpty()
        ? schema.templateDisplay("t")
        : QString("COALESCE(NULLIF(%1, 0), %2)")
            .arg(schema.creatureSpawnDisplay("c"), schema.templateDisplay("t"));
      QString sql = QString(
        "SELECT c.`guid`, %1, %2 AS `display_id`, c.`position_x`, c.`position_y`, "
        "c.`position_z`, c.`orientation`, c.`phaseMask`")
          .arg(schema.creatureEntry("c"), spawn_display);
      if (equipment_available)
        sql += ", e.ItemID1, i1.displayid, i1.InventoryType, "
               "e.ItemID2, i2.displayid, i2.InventoryType, "
               "e.ItemID3, i3.displayid, i3.InventoryType, i3.Subclass";
      sql += QString(" FROM creature c LEFT JOIN creature_template t ON t.entry = %1 ")
        .arg(schema.creatureEntry("c"));
      if (equipment_available)
        sql += QString("LEFT JOIN creature_equip_template e ON e.CreatureID = %1 "
                       "AND e.ID = c.equipment_id "
                       "LEFT JOIN item_template i1 ON i1.entry = e.ItemID1 "
                       "LEFT JOIN item_template i2 ON i2.entry = e.ItemID2 "
                       "LEFT JOIN item_template i3 ON i3.entry = e.ItemID3 ")
                 .arg(schema.creatureEntry("c"));
      sql += "WHERE c.map = ?";
      QSqlQuery query(db);
      query.setForwardOnly(true);
      query.prepare(sql);
      query.addBindValue(map_id);
      if (!query.exec())
      {
        failure = query.lastError().text();
        return false;
      }
      while (query.next())
      {
        CachedNpcSpawn spawn;
        spawn.guid = query.value(0).toULongLong();
        spawn.entry = query.value(1).toUInt();
        spawn.display = query.value(2).toUInt();
        spawn.position = fromServerPosition(query.value(3).toDouble(),
                                            query.value(4).toDouble(), query.value(5).toDouble());
        spawn.yaw = normalizedYaw(
          static_cast<float>(query.value(6).toDouble() * 180.0 / 3.141592653589793) - 180.0f);
        spawn.phase_mask = query.value(7).toUInt();
        if (equipment_available)
        {
          for (std::size_t slot = 0; slot < spawn.weapon_displays.size(); ++slot)
          {
            int const column = 8 + static_cast<int>(slot) * 3;
            auto const [display_id, inventory_type] = itemVisual(
              query.value(column).toUInt(), query.value(column + 1).toUInt(),
              query.value(column + 2).toUInt());
            spawn.weapon_displays[slot] = display_id;
            spawn.weapon_inventory_types[slot] = inventory_type;
          }
          spawn.ranged_entry = query.value(14).toUInt();
          spawn.ranged_subclass = query.value(17).toUInt();
        }
        if (!spawn.guid) continue;
        TileIndex const tile(spawn.position);
        if (!tile.is_valid()) continue;
        auto const [_, inserted] = spawns.emplace(spawn.guid, std::move(spawn));
        if (inserted) by_tile[tile.index()].push_back(query.value(0).toULongLong());
      }
      query.finish();
      QSqlQuery gameobject_query(db);
      gameobject_query.setForwardOnly(true);
      gameobject_query.prepare(
        "SELECT go.`guid`, go.`id`, gt.`displayId`, gt.`size`, "
        "go.`position_x`, go.`position_y`, go.`position_z`, go.`orientation`, "
        "go.`phaseMask` "
        "FROM `gameobject` go LEFT JOIN `gameobject_template` gt "
        "ON gt.`entry` = go.`id` WHERE go.`map` = ?");
      gameobject_query.addBindValue(map_id);
      if (!gameobject_query.exec())
      {
        failure = "Could not cache gameobject spawns: " + gameobject_query.lastError().text();
        return false;
      }
      while (gameobject_query.next())
      {
        CachedGameObjectSpawn spawn;
        spawn.guid = gameobject_query.value(0).toULongLong();
        spawn.entry = gameobject_query.value(1).toUInt();
        spawn.display = gameobject_query.value(2).toUInt();
        spawn.scale = gameobject_query.value(3).toFloat();
        spawn.position = fromServerPosition(gameobject_query.value(4).toDouble(),
          gameobject_query.value(5).toDouble(), gameobject_query.value(6).toDouble());
        spawn.yaw = normalizedYaw(static_cast<float>(
          gameobject_query.value(7).toDouble() * 180.0 / 3.141592653589793) - 180.0f);
        spawn.phase_mask = gameobject_query.value(8).toUInt();
        if (!spawn.guid) continue;
        TileIndex const tile(spawn.position);
        if (!tile.is_valid()) continue;
        if (spawn.display && _gameobject_display_info)
        {
          try
          {
            auto const display = _gameobject_display_info->getByID(spawn.display);
            QString path = QString::fromStdString(display.getString(1));
            if (path.endsWith(".mdx", Qt::CaseInsensitive)
                || path.endsWith(".mdl", Qt::CaseInsensitive))
            {
              path.chop(4);
              path += ".m2";
            }
            spawn.model_path = path.toStdString();
          }
          catch (std::exception const&) {}
        }
        auto const [_, inserted] = gameobjects.emplace(spawn.guid, std::move(spawn));
        if (inserted) gameobjects_by_tile[tile.index()].push_back(
          gameobject_query.value(0).toULongLong());
      }
      gameobject_query.finish();

      QSqlQuery event_definitions(db);
      event_definitions.setForwardOnly(true);
      if (!event_definitions.exec(
            "SELECT `eventEntry`, `description` FROM `game_event` ORDER BY `eventEntry`"))
      {
        failure = "Could not cache game event definitions: "
          + event_definitions.lastError().text();
        return false;
      }
      while (event_definitions.next())
        event_descriptions.emplace(event_definitions.value(0).toInt(),
                                   event_definitions.value(1).toString());
      event_definitions.finish();

      auto attach_direct_events = [&](QString const& statement, auto& cache)
      {
        QSqlQuery events(db);
        events.setForwardOnly(true);
        events.prepare(statement);
        events.addBindValue(map_id);
        if (!events.exec())
        {
          failure = "Could not cache spawn event links: " + events.lastError().text();
          return false;
        }
        while (events.next())
        {
          auto const found = cache.find(events.value(0).toULongLong());
          if (found == cache.end()) continue;
          int const signed_event = events.value(1).toInt();
          auto& memberships = found->second.direct_events;
          if (signed_event && std::find(memberships.begin(), memberships.end(),
                                         signed_event) == memberships.end())
            memberships.push_back(signed_event);
        }
        events.finish();
        return true;
      };
      if (!attach_direct_events(
            "SELECT ec.`guid`, ec.`eventEntry` FROM `game_event_creature` ec "
            "JOIN `creature` c ON c.`guid` = ec.`guid` WHERE c.`map` = ?", spawns)
          || !attach_direct_events(
            "SELECT eg.`guid`, eg.`eventEntry` FROM `game_event_gameobject` eg "
            "JOIN `gameobject` go ON go.`guid` = eg.`guid` WHERE go.`map` = ?",
            gameobjects))
        return false;

      // An event can activate a parent pool. Propagate that signed event link
      // to nested child pools before assigning memberships to their spawns.
      std::unordered_map<unsigned, std::vector<unsigned>> child_pools;
      QSqlQuery pool_hierarchy(db);
      pool_hierarchy.setForwardOnly(true);
      if (!pool_hierarchy.exec("SELECT `pool_id`, `mother_pool` FROM `pool_pool`"))
      {
        failure = "Could not cache pool hierarchy: " + pool_hierarchy.lastError().text();
        return false;
      }
      while (pool_hierarchy.next())
        child_pools[pool_hierarchy.value(1).toUInt()].push_back(
          pool_hierarchy.value(0).toUInt());
      pool_hierarchy.finish();

      std::unordered_map<unsigned, std::vector<int>> pool_events;
      QSqlQuery event_pools(db);
      event_pools.setForwardOnly(true);
      if (!event_pools.exec("SELECT `pool_entry`, `eventEntry` FROM `game_event_pool`"))
      {
        failure = "Could not cache event pools: " + event_pools.lastError().text();
        return false;
      }
      while (event_pools.next())
      {
        unsigned const root = event_pools.value(0).toUInt();
        int const signed_event = event_pools.value(1).toInt();
        if (!root || !signed_event) continue;
        std::vector<unsigned> pending{root};
        std::unordered_set<unsigned> visited;
        while (!pending.empty())
        {
          unsigned const pool = pending.back();
          pending.pop_back();
          if (!visited.insert(pool).second) continue;
          auto& memberships = pool_events[pool];
          if (std::find(memberships.begin(), memberships.end(), signed_event)
              == memberships.end())
            memberships.push_back(signed_event);
          if (auto const children = child_pools.find(pool);
              children != child_pools.end())
            pending.insert(pending.end(), children->second.begin(),
                           children->second.end());
        }
      }
      event_pools.finish();

      auto attach_pool_events = [&](QString const& statement, auto& cache)
      {
        QSqlQuery members(db);
        members.setForwardOnly(true);
        members.prepare(statement);
        members.addBindValue(map_id);
        if (!members.exec())
        {
          failure = "Could not cache pooled spawn events: "
            + members.lastError().text();
          return false;
        }
        while (members.next())
        {
          auto const found = cache.find(members.value(0).toULongLong());
          auto const linked = pool_events.find(members.value(1).toUInt());
          if (found == cache.end() || linked == pool_events.end()) continue;
          auto& memberships = found->second.pool_events;
          for (int const signed_event : linked->second)
            if (std::find(memberships.begin(), memberships.end(), signed_event)
                == memberships.end())
              memberships.push_back(signed_event);
        }
        members.finish();
        return true;
      };
      if (!attach_pool_events(
            "SELECT pc.`guid`, pc.`pool_entry` FROM `pool_creature` pc "
            "JOIN `creature` c ON c.`guid` = pc.`guid` WHERE c.`map` = ?", spawns)
          || !attach_pool_events(
            "SELECT pg.`guid`, pg.`pool_entry` FROM `pool_gameobject` pg "
            "JOIN `gameobject` go ON go.`guid` = pg.`guid` WHERE go.`map` = ?",
            gameobjects))
        return false;
      return true;
    }, error);
  if (!loaded)
  {
    _status->setText("Could not cache this map's NPC spawns: " + error);
    return;
  }

  World* world = _map_view->getWorld();
  for (std::uint64_t const guid : _active_cached_spawns)
  {
    if (_selected_spawn_guid == guid || _draft_snapshots.contains(guid)) continue;
    world->removeNpcSpawnOverlay(guid);
  }
  for (std::uint64_t const guid : _active_cached_gameobjects)
    world->removeServerGameObjectOverlay(guid);
  _active_cached_spawns.clear();
  _active_cached_gameobjects.clear();
  if (_selected_spawn_guid && world->findNpcSpawnOverlay(*_selected_spawn_guid))
    _active_cached_spawns.insert(*_selected_spawn_guid);
  _cached_spawns.swap(spawns);
  _npc_appearance_cache.clear();
  _cached_spawns_by_tile.swap(by_tile);
  _cached_gameobjects.swap(gameobjects);
  _cached_gameobjects_by_tile.swap(gameobjects_by_tile);
  _event_descriptions.swap(event_descriptions);
  for (auto it = _active_event_ids.begin(); it != _active_event_ids.end();)
    if (_event_descriptions.contains(*it)) ++it;
    else it = _active_event_ids.erase(it);
  _failed_cached_spawns.clear();
  _failed_cached_gameobjects.clear();
  _cached_map = map_id;
  _cached_connection = QString("%1:%2/%3/%4")
    .arg(_host->text().trimmed()).arg(_port->value())
    .arg(_database->text().trimmed(), _user->text().trimmed());
  _spawn_cache_loaded = true;
  populateEventMenu();
  _map_view->invalidate();
  _map_view->update();
  std::size_t const unresolved_gameobjects = std::count_if(
    _cached_gameobjects.begin(), _cached_gameobjects.end(),
    [](auto const& entry) { return entry.second.model_path.empty(); });
  QString const model_warning = !_gameobject_display_info
    ? " GameObjectDisplayInfo.dbc is unavailable; gameobject models cannot be shown."
    : unresolved_gameobjects
      ? QString(" %1 gameobject(s) have no usable client model.")
          .arg(static_cast<qulonglong>(unresolved_gameobjects)) : QString();
  _status->setText(QString("Cached %1 NPCs and %2 gameobjects for map %3. Nearby "
                           "spawns appear automatically in loaded tiles; click an NPC or "
                           "gameobject to edit its phase.%4")
                     .arg(static_cast<qulonglong>(_cached_spawns.size()))
                     .arg(static_cast<qulonglong>(_cached_gameobjects.size()))
                     .arg(map_id).arg(model_warning));
}

void NpcTemplateBrowser::updateVisibleCachedSpawns()
{
  if (!_spawn_cache_loaded || !_map_view || !_map_view->getWorld()) return;
  World* world = _map_view->getWorld();
  QString const connection = QString("%1:%2/%3/%4")
    .arg(_host->text().trimmed()).arg(_port->value())
    .arg(_database->text().trimmed(), _user->text().trimmed());
  if (world->getMapID() != _cached_map || connection != _cached_connection)
  {
    for (std::uint64_t const guid : _active_cached_spawns)
      if (_selected_spawn_guid != guid && !_draft_snapshots.contains(guid))
        world->removeNpcSpawnOverlay(guid);
    for (std::uint64_t const guid : _active_cached_gameobjects)
      world->removeServerGameObjectOverlay(guid);
    _active_cached_spawns.clear();
    _active_cached_gameobjects.clear();
    _cached_spawns.clear();
    _npc_appearance_cache.clear();
    for (auto& tile : _cached_spawns_by_tile) tile.clear();
    _cached_gameobjects.clear();
    for (auto& tile : _cached_gameobjects_by_tile) tile.clear();
    _failed_cached_spawns.clear();
    _failed_cached_gameobjects.clear();
    _active_event_ids.clear();
    _event_descriptions.clear();
    if (_event_selector)
    {
      _event_selector->setText("Active events: None");
      _event_selector->setEnabled(false);
    }
    _spawn_cache_loaded = false;
    return;
  }

  glm::vec3 const camera = _map_view->_camera.position;
  float constexpr maximum_distance_squared =
    Noggit::NpcSpawnOverlay::view_distance * Noggit::NpcSpawnOverlay::view_distance;
  float constexpr prefetch_distance_squared = 350.0f * 350.0f;
  float constexpr retention_distance_squared = 325.0f * 325.0f;
  auto const appearance_key = [](CachedNpcSpawn const& spawn)
  {
    return std::array<unsigned, 9>{spawn.display,
      spawn.weapon_displays[0], spawn.weapon_displays[1], spawn.weapon_displays[2],
      spawn.weapon_inventory_types[0], spawn.weapon_inventory_types[1],
      spawn.weapon_inventory_types[2], spawn.ranged_entry, spawn.ranged_subclass};
  };
  std::vector<std::pair<float, std::uint64_t>> nearby;
  std::vector<std::pair<float, std::uint64_t>> approaching;
  std::vector<std::pair<float, std::uint64_t>> nearby_gameobjects;
  std::unordered_set<std::uint64_t> visible;
  std::unordered_set<std::uint64_t> visible_gameobjects;
  for (MapTile* tile : world->mapIndex.loaded_tiles())
  {
    if (!tile || !tile->finishedLoading() || !tile->index.is_valid()) continue;
    for (std::uint64_t const guid : _cached_spawns_by_tile[tile->index.index()])
    {
      auto const found = _cached_spawns.find(guid);
      if (found == _cached_spawns.end()) continue;
      if (!(found->second.phase_mask & _active_phase_mask)) continue;
      if (!eventVisible(found->second.direct_events, found->second.pool_events)) continue;
      glm::vec3 const delta = found->second.position - camera;
      float const distance_squared = glm::dot(delta, delta);
      if (distance_squared <= prefetch_distance_squared
          && !_active_cached_spawns.contains(guid))
        approaching.emplace_back(distance_squared, guid);
      if (distance_squared > maximum_distance_squared) continue;
      visible.insert(guid);
      nearby.emplace_back(distance_squared, guid);
    }
    for (std::uint64_t const guid : _cached_gameobjects_by_tile[tile->index.index()])
    {
      auto const found = _cached_gameobjects.find(guid);
      if (found == _cached_gameobjects.end()) continue;
      if (!(found->second.phase_mask & _active_phase_mask)) continue;
      if (!eventVisible(found->second.direct_events, found->second.pool_events)) continue;
      glm::vec3 const delta = found->second.position - camera;
      float const distance_squared = glm::dot(delta, delta);
      if (distance_squared > maximum_distance_squared) continue;
      visible_gameobjects.insert(guid);
      nearby_gameobjects.emplace_back(distance_squared, guid);
    }
  }

  // Request body assets before they enter the visible radius. ModelManager
  // queues these loads on its worker; no wait is performed in this timer.
  std::sort(approaching.begin(), approaching.end());
  std::unordered_set<std::string> prefetched_paths;
  unsigned queued_models = 0;
  QElapsedTimer prefetch_budget;
  prefetch_budget.start();
  for (auto const& [distance_squared, guid] : approaching)
  {
    CachedNpcSpawn const& spawn = _cached_spawns.at(guid);
    std::string path;
    if (spawn.appearance) path = spawn.appearance->body.model_path;
    else if (auto const found = _npc_appearance_cache.find(appearance_key(spawn));
             found != _npc_appearance_cache.end())
      path = found->second.first.body.model_path;
    else path = npcModelPath(spawn.display);
    if (path.empty() || !prefetched_paths.insert(path).second) continue;
    if (_prefetched_npc_models.contains(
          {Noggit::NoggitRenderContext::NPC_SPAWN_CACHE, path})
        && _prefetched_npc_models.contains({world->getRenderContext(), path}))
      continue;
    if (queued_models >= 8 || prefetch_budget.nsecsElapsed() >= 2'000'000)
      break;
    prefetchNpcModel(path, Noggit::NoggitRenderContext::NPC_SPAWN_CACHE);
    prefetchNpcModel(path, world->getRenderContext());
    ++queued_models;
  }

  bool changed = false;
  for (auto it = _active_cached_spawns.begin(); it != _active_cached_spawns.end();)
  {
    std::uint64_t const guid = *it;
    auto const cached = _cached_spawns.find(guid);
    bool const in_active_phase = cached == _cached_spawns.end()
      || (cached->second.phase_mask & _active_phase_mask);
    bool const in_active_event = cached == _cached_spawns.end()
      || eventVisible(cached->second.direct_events, cached->second.pool_events);
    bool const keep_warm = cached != _cached_spawns.end()
      && glm::dot(cached->second.position - camera,
                  cached->second.position - camera) <= retention_distance_squared;
    if (visible.contains(guid) || (in_active_phase && in_active_event
        && keep_warm) || (in_active_phase && in_active_event
        && (_selected_spawn_guid == guid || _draft_snapshots.contains(guid))))
    {
      ++it;
      continue;
    }
    world->removeNpcSpawnOverlay(guid);
    it = _active_cached_spawns.erase(it);
    changed = true;
  }

  std::sort(nearby.begin(), nearby.end());
  unsigned created = 0;
  unsigned attempted = 0;
  QElapsedTimer creation_budget;
  creation_budget.start();
  static constexpr std::array<unsigned, 3> attachment_ids{1, 2, 12};
  for (auto const& [distance_squared, guid] : nearby)
  {
    if (_active_cached_spawns.contains(guid)) continue;
    if (world->findNpcSpawnOverlay(guid))
    {
      _active_cached_spawns.insert(guid);
      continue;
    }
    if (_failed_cached_spawns.contains(guid)) continue;
    if (created >= 8 || attempted >= 8 || creation_budget.nsecsElapsed() >= 6'000'000)
      break;
    CachedNpcSpawn const& spawn = _cached_spawns.at(guid);
    ++attempted;
    if (!spawn.display && !spawn.appearance)
    {
      _failed_cached_spawns.insert(guid);
      continue;
    }
    try
    {
      float scale = spawn.scale;
      std::optional<Noggit::NpcAppearance> appearance = spawn.appearance;
      if (!appearance)
      {
        auto const key = appearance_key(spawn);
        if (auto const found = _npc_appearance_cache.find(key);
            found != _npc_appearance_cache.end())
        {
          appearance = found->second.first;
          scale = found->second.second;
        }
        else
        {
          bool pending = false;
          if (!loadDisplay(spawn.display, false, nullptr, _cache_preview, &scale,
                           &pending))
          {
            if (!pending) _failed_cached_spawns.insert(guid);
            continue;
          }
          for (std::size_t slot = 0; slot < spawn.weapon_displays.size(); ++slot)
            if (spawn.weapon_displays[slot])
              setWeaponAttachment(_cache_preview, _item_display_info.get(), _client_data,
                                  spawn.weapon_displays[slot],
                                  spawn.weapon_inventory_types[slot], attachment_ids[slot],
                                  slot == 2 ? rangedHandAttachment(spawn.ranged_entry,
                                    spawn.ranged_subclass) : 0);
          if (_cache_preview->creatureAssetsPending()) continue;
          appearance = _cache_preview->creatureAppearance();
          if (appearance)
            _npc_appearance_cache.emplace(key, std::make_pair(*appearance, scale));
        }
      }
      if (!appearance)
      {
        _failed_cached_spawns.insert(guid);
        continue;
      }
      bool models_pending = false;
      bool models_failed = false;
      auto request_world_model = [&](std::string const& path, bool required)
      {
        Model* const model = prefetchNpcModel(path, world->getRenderContext());
        if (!model) { models_failed = models_failed || required; return; }
        if (!model->finishedLoading()) { models_pending = true; return; }
        if (model->loading_failed() || model->skin_load_failed())
          models_failed = models_failed || required;
      };
      request_world_model(appearance->body.model_path, true);
      for (auto const& attachment : appearance->attachments)
        request_world_model(attachment.model.model_path, false);
      auto request_world_textures = [&](Noggit::NpcModelAppearance const& model)
      {
        for (auto const& [slot, path] : model.replacement_textures)
        {
          blp_texture* const texture = prefetchNpcTexture(
            path, world->getRenderContext());
          if (!texture) continue;
          if (!texture->finishedLoading()) models_pending = true;
        }
      };
      request_world_textures(appearance->body);
      for (auto const& attachment : appearance->attachments)
        request_world_textures(attachment.model);
      if (models_failed)
      {
        _failed_cached_spawns.insert(guid);
        continue;
      }
      if (models_pending) continue;
      world->addNpcSpawnOverlay(guid, spawn.entry, spawn.position, spawn.yaw,
                                scale, *appearance);
      applyWeaponVisibility(guid);
      _active_cached_spawns.insert(guid);
      ++created;
      changed = true;
    }
    catch (std::exception const&)
    {
      _failed_cached_spawns.insert(guid);
    }
  }
  for (auto it = _active_cached_gameobjects.begin();
       it != _active_cached_gameobjects.end();)
  {
    if (visible_gameobjects.contains(*it))
    {
      ++it;
      continue;
    }
    world->removeServerGameObjectOverlay(*it);
    it = _active_cached_gameobjects.erase(it);
    changed = true;
  }
  std::sort(nearby_gameobjects.begin(), nearby_gameobjects.end());
  unsigned created_gameobjects = 0;
  for (auto const& [distance_squared, guid] : nearby_gameobjects)
  {
    if (created_gameobjects >= 24) break;
    if (world->findServerGameObjectOverlay(guid))
    {
      _active_cached_gameobjects.insert(guid);
      continue;
    }
    if (_failed_cached_gameobjects.contains(guid)) continue;
    CachedGameObjectSpawn const& spawn = _cached_gameobjects.at(guid);
    if (spawn.model_path.empty())
    {
      _failed_cached_gameobjects.insert(guid);
      continue;
    }
    try
    {
      world->addServerGameObjectOverlay(guid, spawn.entry, spawn.model_path,
                                        spawn.position, spawn.yaw, spawn.scale);
      _active_cached_gameobjects.insert(guid);
      ++created_gameobjects;
      changed = true;
    }
    catch (std::exception const&)
    {
      _failed_cached_gameobjects.insert(guid);
    }
  }
  if (changed)
  {
    _map_view->invalidate();
    _map_view->update();
  }
}

void NpcTemplateBrowser::removeCachedSpawn(std::uint64_t guid)
{
  auto const found = _cached_spawns.find(guid);
  if (found != _cached_spawns.end())
  {
    TileIndex const tile(found->second.position);
    if (tile.is_valid())
    {
      auto& guids = _cached_spawns_by_tile[tile.index()];
      std::erase(guids, guid);
    }
    _cached_spawns.erase(found);
  }
  _active_cached_spawns.erase(guid);
  _failed_cached_spawns.erase(guid);
}

void NpcTemplateBrowser::updateCachedSpawn(std::uint64_t guid, unsigned entry,
                                             glm::vec3 const& position, unsigned display,
                                             float yaw,
                                             Noggit::NpcAppearance const* appearance,
                                             float scale,
                                             std::optional<unsigned> phase_mask)
{
  if (!_spawn_cache_loaded || !guid) return;
  TileIndex const tile(position);
  if (!tile.is_valid())
  {
    removeCachedSpawn(guid);
    return;
  }
  auto [found, inserted] = _cached_spawns.try_emplace(guid);
  if (inserted)
    _cached_spawns_by_tile[tile.index()].push_back(guid);
  else
  {
    TileIndex const old_tile(found->second.position);
    if (old_tile.is_valid() && !(old_tile == tile))
    {
      std::erase(_cached_spawns_by_tile[old_tile.index()], guid);
      _cached_spawns_by_tile[tile.index()].push_back(guid);
    }
  }
  CachedNpcSpawn& spawn = found->second;
  spawn.guid = guid;
  spawn.entry = entry;
  spawn.position = position;
  spawn.yaw = yaw;
  if (phase_mask) spawn.phase_mask = *phase_mask;
  if (display) spawn.display = display;
  if (appearance)
  {
    spawn.appearance = *appearance;
    spawn.scale = scale;
  }
  _failed_cached_spawns.erase(guid);
  if (_map_view && _map_view->getWorld()->findNpcSpawnOverlay(guid))
    _active_cached_spawns.insert(guid);
}

void NpcTemplateBrowser::selectSpawn(std::uint64_t guid, unsigned entry)
{
  if (!_map_view || !guid) return;
  auto sync_template = [this]
  {
    if (selectTemplate(_selected_spawn_entry, false)) return;
    QSignalBlocker const blocker(_table->selectionModel());
    _table->clearSelection();
    _table->setCurrentIndex({});
    _details->setText(QString("<b>%1</b><br>Template ID: %2")
      .arg(_edit_name->text().toHtmlEscaped()).arg(_selected_spawn_entry));
  };
  finishNpcTransformEdit();
  _selected_gameobject_guid.reset();
  QScopedValueRollback<bool> const restoring(_restoring_history, true);
  _selected_spawn_rotating = false;
  showPropertiesPanel();
  if (_selected_spawn_guid && *_selected_spawn_guid == guid)
  {
    // The editor fields and overlay contain the current draft, which may have
    // been moved with the viewport gizmo but not saved yet. Reselecting the
    // same NPC must not reload its older database transform over that draft.
    if (_map_view->getWorld()->findNpcSpawnOverlay(guid))
    {
      sync_template();
      if (auto const cached = _cached_spawns.find(guid); cached != _cached_spawns.end())
        showPhaseAssignment(false, guid, entry, cached->second.phase_mask);
      _map_view->getWorld()->selectNpcSpawnOverlay(guid);
      _map_view->activateNpcTransformGizmo();
      updateWaypointList();
      updateMovementPreview();
      _map_view->invalidate();
      _map_view->update();
      _status->setText(QString("Reselected NPC GUID %1. Use Rotate NPC with gizmo or "
                               "hold R and move the mouse; save spawn settings to keep the facing.")
                         .arg(guid));
      return;
    }
  }
  if (_selected_spawn_guid && *_selected_spawn_guid != guid)
  {
    _draft_snapshots[*_selected_spawn_guid] = captureEditorSnapshot();
    if (auto* old = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
    {
      old->setPreviewRoute({}, false);
      old->setTransform(_saved_spawn_position, _saved_spawn_yaw, old->scale());
    }
  }
  _selected_spawn_guid = guid;
  _selected_spawn_entry = entry;
  _capturing_waypoints = false;
  _moving_spawn = false;
  {
    // Animation overrides are a temporary preview choice for one selected
    // spawn. Never carry that choice into the next unrelated NPC.
    QSignalBlocker const blocker(_preview_animation);
    _preview_animation->setCurrentIndex(0);
    _preview_animation->setEditText(_preview_animation->itemText(0));
  }
  _capture_waypoint_button->setText("Add points by clicking terrain");
  _move_spawn_button->setText("Move NPC by clicking terrain");

  QString error;
  bool const loaded = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    false, [&](QSqlDatabase& db, QString& failure)
    {
      Sql::WorldDatabaseSchema schema;
      if (!Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
      QSqlQuery spawn(db);
      spawn.prepare("SELECT * FROM creature WHERE guid = ? LIMIT 1");
      spawn.addBindValue(QVariant::fromValue<qulonglong>(guid));
      if (!spawn.exec() || !spawn.next())
      {
        failure = spawn.lastError().isValid() ? spawn.lastError().text()
          : "The selected spawn no longer exists in creature.";
        return false;
      }
      _selected_spawn_entry = field(spawn, schema.creature_entry_column).toUInt();
      _edit_respawn->setValue(field(spawn, "spawntimesecs").toInt());
      QVariant wander = field(spawn, "wander_distance");
      if (!wander.isValid()) wander = field(spawn, "spawndist");
      _edit_wander->setValue(wander.toDouble());
      int const movement = field(spawn, "MovementType").toInt();
      _edit_movement->setCurrentIndex(std::max(0, _edit_movement->findData(movement)));
      _edit_display->setValue(schema.creature_spawn_display_column.isEmpty()
        ? 0 : field(spawn, schema.creature_spawn_display_column).toInt());
      _edit_display->setEnabled(!schema.creature_spawn_display_column.isEmpty());
      _edit_display->setToolTip(schema.creature_spawn_display_column.isEmpty()
        ? "AzerothCore stores displays in creature_template_model. Use Template display, "
          "or create a unique template for a per-NPC appearance."
        : "Optional per-spawn display override. Zero uses the template display.");
      _edit_spawn_position = fromServerPosition(field(spawn, "position_x").toDouble(),
        field(spawn, "position_y").toDouble(), field(spawn, "position_z").toDouble());
      _saved_spawn_position = _edit_spawn_position;
      _saved_spawn_yaw = normalizedYaw(static_cast<float>(
        field(spawn, "orientation").toDouble() * 180.0 / 3.141592653589793) - 180.0f);
      {
        QSignalBlocker const blocker(_edit_yaw);
        _edit_yaw->setValue(_saved_spawn_yaw);
      }
      _edit_position_label->setText(QString("%1, %2, %3")
        .arg(_edit_spawn_position.x, 0, 'f', 2)
        .arg(_edit_spawn_position.y, 0, 'f', 2)
        .arg(_edit_spawn_position.z, 0, 'f', 2));
      spawn.finish();

      QSqlQuery templ(db);
      templ.prepare("SELECT * FROM creature_template WHERE entry = ? LIMIT 1");
      templ.addBindValue(_selected_spawn_entry);
      if (!templ.exec() || !templ.next())
      {
        failure = templ.lastError().isValid() ? templ.lastError().text()
          : "The selected spawn's template is missing.";
        return false;
      }
      _edit_name->setText(field(templ, "name").toString());
      _edit_subname->setText(field(templ, "subname").toString());
      _edit_minlevel->setValue(field(templ, "minlevel").toInt());
      _edit_maxlevel->setValue(field(templ, "maxlevel").toInt());
      _edit_faction->setFactionId(field(templ, "faction").toUInt());
      _loaded_speed_walk = std::max(0.05, field(templ, "speed_walk").toDouble());
      _loaded_speed_run = std::max(0.05, field(templ, "speed_run").toDouble());
      templ.finish();
      unsigned display = 0;
      if (!Sql::loadTemplateDisplay(db, schema, _selected_spawn_entry, display, failure))
        return false;
      _edit_template_display->setValue(display);
      {
        QSignalBlocker const walk_block(_edit_speed_walk);
        QSignalBlocker const run_block(_edit_speed_run);
        _edit_speed_walk->setValue(_loaded_speed_walk);
        _edit_speed_run->setValue(_loaded_speed_run);
      }
      QSqlQuery usage(db);
      usage.prepare(QString("SELECT COUNT(*) FROM creature WHERE %1")
                      .arg(schema.creatureReferencesTemplate()));
      usage.addBindValue(_selected_spawn_entry);
      if (!usage.exec() || !usage.next())
      {
        failure = usage.lastError().isValid() ? usage.lastError().text()
          : "Could not count spawns that use the selected template.";
        return false;
      }
      _template_spawn_count = usage.value(0).toUInt();
      _speed_usage->setText(_template_spawn_count > 1
        ? QString("Shared template: speed changes affect all %1 placed NPCs. "
                  "Use Appearance > Create unique template for per-NPC speed.")
            .arg(_template_spawn_count)
        : "This template is used by one placed NPC; speed changes affect only this spawn.");

      {
        QSignalBlocker const blocker(_edit_weapon_stance);
        _edit_weapon_stance->setCurrentIndex(0);
        unsigned bytes2 = 0;
        bool has_spawn_addon = false;
        if (tableExists(db, "creature_addon"))
        {
          QSqlQuery addon(db);
          addon.prepare("SELECT bytes2 FROM creature_addon WHERE guid = ? LIMIT 1");
          addon.addBindValue(QVariant::fromValue<qulonglong>(guid));
          if (addon.exec() && addon.next())
          {
            bytes2 = addon.value(0).toUInt();
            has_spawn_addon = true;
          }
        }
        if (!has_spawn_addon && tableExists(db, "creature_template_addon"))
        {
          QSqlQuery addon(db);
          addon.prepare("SELECT bytes2 FROM creature_template_addon WHERE entry = ? LIMIT 1");
          addon.addBindValue(_selected_spawn_entry);
          if (addon.exec() && addon.next()) bytes2 = addon.value(0).toUInt();
        }
        int const stance = static_cast<int>(bytes2 & 0xffu);
        if (stance == 1 || stance == 2)
          _edit_weapon_stance->setCurrentIndex(_edit_weapon_stance->findData(stance));
      }

      _waypoints.clear();
      _edit_emote->setCurrentIndex(std::max(0, _edit_emote->findData(0)));
      if (tableExists(db, "creature_addon") && tableExists(db, "waypoint_data"))
      {
        QSqlQuery addon(db);
        addon.prepare("SELECT path_id, emote FROM creature_addon WHERE guid = ? LIMIT 1");
        addon.addBindValue(QVariant::fromValue<qulonglong>(guid));
        if (addon.exec() && addon.next())
        {
          unsigned const path_id = addon.value(0).toUInt();
          unsigned const emote_id = addon.value(1).toUInt();
          int emote_index = _edit_emote->findData(emote_id);
          if (emote_index < 0)
          {
            _edit_emote->addItem(QString("%1 = Unknown/custom emote").arg(emote_id), emote_id);
            emote_index = _edit_emote->count() - 1;
            _edit_emote->setItemData(emote_index, 0u, EmoteAnimationRole);
            _edit_emote->setItemData(emote_index, 0u, EmoteProcedureRole);
            _edit_emote->setItemData(emote_index, 0u, EmoteParameterRole);
          }
          _edit_emote->setCurrentIndex(emote_index);
          addon.finish();
          if (path_id)
          {
            bool const has_waypoint_scripts = tableExists(db, "waypoint_scripts");
            QSqlQuery points(db);
            points.prepare("SELECT position_x, position_y, position_z, delay, move_type, action "
                           "FROM waypoint_data WHERE id = ? ORDER BY point");
            points.addBindValue(path_id);
            if (points.exec())
              while (points.next())
              {
                WaypointDraft point{fromServerPosition(points.value(0).toDouble(),
                  points.value(1).toDouble(), points.value(2).toDouble()),
                  points.value(3).toUInt(), points.value(4).toInt() == 1};
                unsigned const action_id = points.value(5).toUInt();
                if (action_id && has_waypoint_scripts)
                {
                  QSqlQuery action(db);
                  action.prepare("SELECT datalong FROM waypoint_scripts "
                                 "WHERE id = ? AND command = 1 ORDER BY delay LIMIT 1");
                  action.addBindValue(action_id);
                  if (action.exec() && action.next())
                    point.emote_id = action.value(0).toUInt();
                }
                _waypoints.push_back(point);
              }
          }
        }
      }
      return true;
    }, error);
  if (!loaded)
  {
    _selected_spawn_guid.reset();
    _spawn_editor->hide();
    if (_phase_assignment_panel) _phase_assignment_panel->hide();
    if (_waypoint_panel) _waypoint_panel->setEnabled(false);
    _status->setText("Could not inspect NPC spawn: " + error);
    return;
  }

  sync_template();

  // Builds prior to the authored/generated waypoint split accidentally saved
  // the two-metre terrain-preview samples as real waypoint_data rows. Recover
  // the authored corners when that very specific closed, densely sampled
  // shape is encountered. The repaired path remains an unsaved editor change;
  // the database is only rewritten if the user explicitly saves it.
  bool const collapsed_surface_supports = collapseLegacyConformedWaypoints();

  // A retraced Trinity path is stored as 1..N..2. Collapse that generated
  // return half when loading so Noggit continues to show only the points the
  // user authored and does not expand an already-expanded route again.
  bool loaded_retrace = false;
  if (_waypoints.size() >= 4 && _waypoints.size() % 2 == 0)
  {
    std::size_t const endpoint = _waypoints.size() / 2;
    loaded_retrace = true;
    for (std::size_t index = 1; index < endpoint; ++index)
    {
      if (glm::distance(_waypoints[index].position,
                        _waypoints[_waypoints.size() - index].position) > 0.05f)
      {
        loaded_retrace = false;
        break;
      }
    }
    if (loaded_retrace)
      _waypoints.resize(endpoint + 1);
  }
  {
    QSignalBlocker const blocker(_waypoint_behavior);
    int const behavior = loaded_retrace || _waypoints.size() <= 2 ? 1 : 0;
    _waypoint_behavior->setCurrentIndex(std::max(0, _waypoint_behavior->findData(behavior)));
  }

  _spawn_identity->setText(QString("GUID %1 · Template %2").arg(guid).arg(_selected_spawn_entry));
  if (auto const cached = _cached_spawns.find(guid); cached != _cached_spawns.end())
    showPhaseAssignment(false, guid, entry, cached->second.phase_mask);
  else if (_phase_assignment_panel)
    _phase_assignment_panel->hide();
  unsigned const effective_display = _edit_display->value() > 0
    ? static_cast<unsigned>(_edit_display->value())
    : static_cast<unsigned>(_edit_template_display->value());
  loadDisplay(effective_display, false);
  _spawn_editor->show();
  if (_waypoint_panel) _waypoint_panel->setEnabled(true);
  _map_view->getWorld()->selectNpcSpawnOverlay(guid);
  if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(guid))
  {
    int const stance = _edit_weapon_stance->currentData().toInt();
    if (stance == 1 || stance == 2)
      overlay->setWeaponVisibility(stance == 1, stance == 2);
  }
  applyWeaponVisibility(guid);
  _map_view->activateNpcTransformGizmo();
  if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(guid))
    overlay->setTransform(_saved_spawn_position, _saved_spawn_yaw, overlay->scale());
  updateWaypointList();
  updateAnimationAvailability();
  updateMovementPreview();
  if (auto const draft = _draft_snapshots.find(guid); draft != _draft_snapshots.end())
    applyEditorSnapshot(draft->second, EditAll);
  _history_snapshot = captureEditorSnapshot();
  _map_view->invalidate();
  _map_view->update();
  if (collapsed_surface_supports)
    _status->setText(QString("Recovered %1 authored waypoints from a legacy expanded preview "
                             "path for NPC GUID %2. Save the path to replace the inflated "
                             "database route.").arg(_waypoints.size()).arg(guid));
  else
    _status->setText(QString("Selected NPC GUID %1. Use Rotate NPC with gizmo or hold R and "
                             "move the mouse; save spawn settings to keep the facing.").arg(guid));
}

void NpcTemplateBrowser::showPropertiesPanel()
{
  for (QWidget* parent = _properties_panel; parent; parent = parent->parentWidget())
  {
    if (auto* dock = qobject_cast<QDockWidget*>(parent))
    {
      dock->show();
      dock->raise();
      break;
    }
  }
}

void NpcTemplateBrowser::addWaypoint(glm::vec3 const& position)
{
  std::size_t const old_size = _waypoints.size();
  if (_waypoints.empty())
    _waypoints.push_back({_edit_spawn_position, 0, false});
  _waypoints.push_back({position, 0, false});
  if (!_preview_movement->isChecked())
    _preview_movement->setChecked(true);
  {
    QSignalBlocker const blocker(_waypoint_list);
    for (std::size_t index = old_size; index < _waypoints.size(); ++index)
      _waypoint_list->addItem(waypointListText(index));
  }
  _waypoint_list->setCurrentRow(static_cast<int>(_waypoints.size()) - 1);
  updateMovementPreview();
  _status->setText(QString("Added waypoint %1. Save path when finished.").arg(_waypoints.size()));
  recordNpcEdit(EditPath);
}

QString NpcTemplateBrowser::waypointListText(std::size_t index) const
{
  if (index >= _waypoints.size()) return {};
  auto const& point = _waypoints[index];
  QString const action = point.emote_id
    ? QString(" · %1").arg(waypointEmoteLabel(point.emote_id)) : QString();
  return QString("%1. %2, %3, %4 · %5 · wait %6 ms%7")
    .arg(index + 1).arg(point.position.x, 0, 'f', 1)
    .arg(point.position.y, 0, 'f', 1).arg(point.position.z, 0, 'f', 1)
    .arg(point.run ? "run" : "walk").arg(point.delay_ms).arg(action);
}

void NpcTemplateBrowser::updateWaypointList()
{
  int const selected = _waypoint_list->currentRow();
  QSignalBlocker const blocker(_waypoint_list);
  _waypoint_list->clear();
  for (std::size_t i = 0; i < _waypoints.size(); ++i)
    _waypoint_list->addItem(waypointListText(i));
  if (selected >= 0 && selected < _waypoint_list->count())
    _waypoint_list->setCurrentRow(selected);
}

bool NpcTemplateBrowser::collapseLegacyConformedWaypoints()
{
  // A legacy conformed loop always ended with a generated sample no farther
  // than the old two-metre support spacing from point one. Requiring both that
  // closing signature and an overwhelmingly dense route avoids simplifying a
  // normally authored path merely because it contains many points.
  if (_waypoints.size() < 16)
    return false;

  auto horizontal_step = [](glm::vec3 const& from, glm::vec3 const& to)
  {
    return glm::vec2{to.x - from.x, to.z - from.z};
  };
  float const closing_distance = glm::length(horizontal_step(
    _waypoints.back().position, _waypoints.front().position));
  if (closing_distance > 2.1f)
    return false;

  std::size_t dense_steps = 0;
  for (std::size_t index = 1; index < _waypoints.size(); ++index)
  {
    float const length = glm::length(horizontal_step(
      _waypoints[index - 1].position, _waypoints[index].position));
    if (length > 0.001f && length <= 2.1f)
      ++dense_steps;
  }
  if (dense_steps * 100 < (_waypoints.size() - 1) * 92)
    return false;

  std::vector<WaypointDraft> authored;
  authored.reserve(_waypoints.size() / 4);
  authored.push_back(_waypoints.front());
  for (std::size_t index = 1; index + 1 < _waypoints.size(); ++index)
  {
    glm::vec2 const incoming = horizontal_step(
      _waypoints[index - 1].position, _waypoints[index].position);
    glm::vec2 const outgoing = horizontal_step(
      _waypoints[index].position, _waypoints[index + 1].position);
    float const incoming_length = glm::length(incoming);
    float const outgoing_length = glm::length(outgoing);
    if (incoming_length <= 0.001f || outgoing_length <= 0.001f)
      continue;

    float const direction_dot = glm::dot(incoming, outgoing)
      / (incoming_length * outgoing_length);
    float const relative_step_change = std::abs(incoming_length - outgoing_length)
      / std::max(incoming_length, outgoing_length);
    bool const authored_boundary = direction_dot < 0.99998f
      || relative_step_change > 0.02f;
    bool const authored_action = _waypoints[index].delay_ms != 0
      || _waypoints[index].emote_id != 0;
    if (authored_boundary || authored_action)
    {
      WaypointDraft point = _waypoints[index];
      point.generated = false;
      authored.push_back(point);
    }
  }

  // The final database row is part of the generated closing leg, not the
  // authored endpoint. That endpoint is the final direction/step boundary
  // retained above.
  if (authored.size() < 2 || authored.size() * 2 >= _waypoints.size())
    return false;

  _waypoints = std::move(authored);
  _conformed_source.clear();
  _conformed_cache.clear();
  _conformed_segments.clear();
  return true;
}

std::vector<NpcTemplateBrowser::WaypointDraft> NpcTemplateBrowser::patrolWaypoints() const
{
  std::vector<WaypointDraft> result = _waypoints;
  bool const retrace = !_waypoint_behavior || _waypoint_behavior->currentData().toInt() == 1;
  if (retrace && _waypoints.size() > 2)
  {
    for (std::size_t index = _waypoints.size() - 2; index > 0; --index)
    {
      WaypointDraft return_point = _waypoints[index];
      return_point.generated = true;
      result.push_back(return_point);
    }
  }
  return result;
}

std::vector<NpcTemplateBrowser::WaypointDraft> NpcTemplateBrowser::terrainConformedWaypoints() const
{
  std::vector<WaypointDraft> const patrol = patrolWaypoints();
  if (patrol.empty() || !_map_view || !_map_view->getWorld())
    return patrol;

  auto same_path = [](std::vector<WaypointDraft> const& lhs,
                      std::vector<WaypointDraft> const& rhs)
  {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t index = 0; index < lhs.size(); ++index)
    {
      auto const& a = lhs[index];
      auto const& b = rhs[index];
      if (a.position.x != b.position.x || a.position.y != b.position.y
          || a.position.z != b.position.z || a.delay_ms != b.delay_ms
          || a.run != b.run || a.generated != b.generated || a.emote_id != b.emote_id)
        return false;
    }
    return true;
  };
  if (same_path(patrol, _conformed_source))
    return _conformed_cache;

  // Trinity's no-MMaps fallback moves directly between database nodes. Keep
  // those segments short and place support nodes on Noggit's visible terrain
  // or WMO floor so the spline does not cut through either surface.
  constexpr float support_spacing = 2.0f;
  std::vector<WaypointDraft> result;
  result.reserve(patrol.size() * 2);

  auto conform_terrain = [this](glm::vec3 position)
  {
    if (auto const ground = _map_view->getWorld()->try_get_ground_height(position))
      position.y = ground->y;
    return position;
  };

  struct WmoSurfaceHit
  {
    SceneObject* object = nullptr;
    glm::vec3 position{};
  };

  auto wmo_surface = [this](glm::vec3 const& position, SceneObject* required_object)
    -> std::optional<WmoSurfaceHit>
  {
    constexpr float wmo_surface_tolerance = 4.0f;
    glm::vec3 const ray_origin{
      position.x, position.y + wmo_surface_tolerance, position.z};
    math::ray const ray(ray_origin, {0.0f, -1.0f, 0.0f});
    selection_result const hits = _map_view->getWorld()->intersect(
      glm::mat4x4(1.0f), ray,
      false, true, false, true, false, true, true, false, false, false, 0.0f, true);

    std::optional<WmoSurfaceHit> closest;
    float closest_difference = wmo_surface_tolerance;
    for (auto const& hit : hits)
    {
      if (hit.second.index() != eEntry_Object)
        continue;
      SceneObject* const object = std::get<selected_object_type>(hit.second);
      if (!object || object->which() != eWMO)
        continue;
      if (required_object && object != required_object)
        continue;

      float const distance = hit.first * std::max(std::abs(object->scale), 0.001f);
      glm::vec3 const candidate = ray.position(distance);
      float const difference = std::abs(candidate.y - position.y);
      if (distance >= 0.0f && difference <= closest_difference)
      {
        closest = WmoSurfaceHit{object, candidate};
        closest_difference = difference;
      }
    }
    return closest;
  };

  WaypointDraft first = patrol.front();
  result.push_back(first);

  auto append_segment = [&](WaypointDraft const& destination, bool include_authored_endpoint)
  {
    glm::vec3 const start = result.back().position;
    auto same_position = [](glm::vec3 const& lhs, glm::vec3 const& rhs)
    {
      return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    };
    auto same_waypoint = [&](WaypointDraft const& lhs, WaypointDraft const& rhs)
    {
      return same_position(lhs.position, rhs.position)
        && lhs.delay_ms == rhs.delay_ms && lhs.run == rhs.run
        && lhs.generated == rhs.generated && lhs.emote_id == rhs.emote_id;
    };
    for (ConformedSegmentCache const& cached : _conformed_segments)
    {
      if (cached.include_endpoint == include_authored_endpoint
          && same_position(cached.start, start)
          && same_waypoint(cached.destination, destination))
      {
        result.insert(result.end(), cached.points.begin(), cached.points.end());
        return;
      }
    }
    std::size_t const segment_begin = result.size();
    auto const start_wmo = wmo_surface(start, nullptr);
    auto const destination_wmo = wmo_surface(destination.position, nullptr);
    bool const wmo_segment = start_wmo && destination_wmo;
    SceneObject* const segment_wmo = wmo_segment
      && start_wmo->object == destination_wmo->object ? start_wmo->object : nullptr;
    auto const start_ground = _map_view->getWorld()->try_get_ground_height(start);
    auto const destination_ground =
      _map_view->getWorld()->try_get_ground_height(destination.position);
    bool const terrain_segment = !wmo_segment && start_ground && destination_ground
      && std::abs(start.y - start_ground->y) <= 0.75f
      && std::abs(destination.position.y - destination_ground->y) <= 0.75f;

    float const horizontal_distance = glm::length(glm::vec2(
      destination.position.x - start.x, destination.position.z - start.z));
    unsigned const steps = std::max(1u, static_cast<unsigned>(
      std::ceil(horizontal_distance / support_spacing)));
    unsigned const last_step = include_authored_endpoint ? steps : steps - 1u;
    for (unsigned step = 1; step <= last_step; ++step)
    {
      bool const authored = include_authored_endpoint && step == steps;
      WaypointDraft point;
      point.position = glm::mix(start, destination.position,
        static_cast<float>(step) / static_cast<float>(steps));
      if (!authored)
      {
        if (wmo_segment)
        {
          if (auto const surface = wmo_surface(point.position, segment_wmo))
            point.position = surface->position;
          else
            continue;
        }
        else if (terrain_segment)
          point.position = conform_terrain(point.position);
        else if (auto const surface = wmo_surface(point.position, nullptr))
          point.position = surface->position;
        else if (auto const ground = _map_view->getWorld()->try_get_ground_height(point.position);
                 ground && std::abs(ground->y - point.position.y) <= 4.0f)
          point.position.y = ground->y;
        else
          continue;
      }
      point.delay_ms = authored ? destination.delay_ms : 0u;
      point.run = destination.run;
      point.generated = !authored || destination.generated;
      point.emote_id = authored ? destination.emote_id : 0u;
      result.push_back(point);
    }
    if (_conformed_segments.size() >= 2048)
      _conformed_segments.clear();
    _conformed_segments.push_back({start, destination, include_authored_endpoint,
      std::vector<WaypointDraft>(result.begin() + segment_begin, result.end())});
  };

  for (std::size_t index = 1; index < patrol.size(); ++index)
    append_segment(patrol[index], true);

  // Waypoint paths repeat. Conform the closing leg too, but leave the exact
  // first point to the server's normal last-to-first transition so it is not
  // duplicated in waypoint_data.
  append_segment(first, false);
  _conformed_source = patrol;
  _conformed_cache = result;
  return result;
}

unsigned NpcTemplateBrowser::emotePreviewAnimation(int index) const
{
  if (index < 0 || index >= _edit_emote->count()) return 0;

  unsigned const procedure = _edit_emote->itemData(index, EmoteProcedureRole).toUInt();
  if (procedure != 1)
    return _edit_emote->itemData(index, EmoteAnimationRole).toUInt();

  // Stand-state emotes store a UnitStandStateType in EmoteSpecProcParam instead
  // of a directly playable animation ID. Resolve those states to the persistent
  // WotLK character animation used by the client.
  switch (_edit_emote->itemData(index, EmoteParameterRole).toUInt())
  {
    case 0: return 0;   // Stand
    case 1: return 97;  // Sit ground
    case 2: return 103; // Sit chair (generic/medium)
    case 3: return 100; // Sleep
    case 4: return 102; // Sit low chair
    case 5: return 103; // Sit medium chair
    case 6: return 104; // Sit high chair
    case 7: return 6;   // Dead
    case 8: return 75;  // Kneel
    case 9: return 41;  // Submerged / swim idle
    default: return 0;
  }
}

unsigned NpcTemplateBrowser::waypointPreviewAnimation(unsigned emote_id) const
{
  int const index = _waypoint_emote->findData(emote_id);
  if (index < 0 || emote_id == 0) return 0;
  unsigned const procedure = _waypoint_emote->itemData(index, EmoteProcedureRole).toUInt();
  if (procedure != 1)
    return _waypoint_emote->itemData(index, EmoteAnimationRole).toUInt();
  switch (_waypoint_emote->itemData(index, EmoteParameterRole).toUInt())
  {
    case 0: return 0;
    case 1: return 97;
    case 2: return 103;
    case 3: return 100;
    case 4: return 102;
    case 5: return 103;
    case 6: return 104;
    case 7: return 6;
    case 8: return 75;
    case 9: return 41;
    default: return 0;
  }
}

QString NpcTemplateBrowser::waypointEmoteLabel(unsigned emote_id) const
{
  int const index = _waypoint_emote->findData(emote_id);
  return index >= 0 ? _waypoint_emote->itemText(index)
                    : QString("Emote %1").arg(emote_id);
}

void NpcTemplateBrowser::updateMovementPreview()
{
  if (!_selected_spawn_guid || !_map_view) return;
  auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid);
  if (!overlay) return;
  std::vector<Noggit::NpcSpawnOverlay::PreviewWaypoint> route;
  std::optional<glm::vec3> route_start;
  std::optional<glm::vec3> route_end;
  int const movement = _edit_movement->currentData().toInt();
  float const wander_radius = movement == 1
    ? static_cast<float>(_edit_wander->value()) : 0.0f;
  overlay->setMovementPreviewSettings(wander_radius,
    static_cast<float>(_edit_speed_walk->value()),
    static_cast<float>(_edit_speed_run->value()));
  if (movement == 2)
  {
    std::vector<WaypointDraft> const points = _conform_waypoints
      && _conform_waypoints->isChecked()
      ? terrainConformedWaypoints() : patrolWaypoints();
    if (points.size() >= 2)
    {
      route_start = points.front().position;
      route_end = _waypoints.empty()
        ? std::optional<glm::vec3>{points.back().position}
        : std::optional<glm::vec3>{_waypoints.back().position};
      for (std::size_t index = 1; index < points.size(); ++index)
      {
        WaypointDraft const& point = points[index];
        route.push_back({point.position, point.delay_ms, point.run, !point.generated,
                         waypointPreviewAnimation(point.emote_id)});
      }
      WaypointDraft const& first = points.front();
      route.push_back({first.position, first.delay_ms, first.run, true,
                       waypointPreviewAnimation(first.emote_id)});
    }
  }
  else if (movement == 1 && _edit_wander->value() > 0.0)
  {
    float const radius = static_cast<float>(_edit_wander->value());
    glm::vec3 const center = overlay->anchorPosition();
    for (unsigned i = 0; i < 8; ++i)
    {
      float const angle = static_cast<float>(i * 2.399963229728653);
      float const distance = radius * (0.35f + 0.55f * (i % 3) / 2.0f);
      glm::vec3 position = center + glm::vec3(std::cos(angle) * distance, 0.0f,
                                             std::sin(angle) * distance);
      if (auto const ground = _map_view->getWorld()->try_get_ground_height(position))
        position.y = ground->y;
      // Trinity chooses random wander pauses at runtime. Use varied, stable
      // preview delays so the editor communicates that behavior without
      // implying an exact server route.
      route.push_back({position, 1800u + (i % 4u) * 650u, false});
    }
    route.push_back({center, 2400, false});
  }
  overlay->setPreviewRoute(std::move(route), _preview_movement->isChecked(), route_start,
                           movement == 2, route_end);
  bool numeric = false;
  unsigned animation_id = _preview_animation->currentData().toUInt(&numeric);
  if (!numeric) animation_id = _preview_animation->currentText().trimmed().toUInt(&numeric);
  bool const manual_override = numeric && animation_id != 0;
  if (!manual_override)
    animation_id = emotePreviewAnimation(_edit_emote->currentIndex());
  unsigned const emote_procedure = _edit_emote->currentData(EmoteProcedureRole).toUInt();
  overlay->setPreviewAnimation(animation_id, manual_override,
                               manual_override || emote_procedure != 0);
  _map_view->invalidate();
  _map_view->update();
}

void NpcTemplateBrowser::updateAnimationAvailability()
{
  if (!_selected_spawn_guid || !_map_view) return;
  auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid);
  if (!overlay || !overlay->body().finishedLoading()) return;
  Model* model = overlay->body().model.get();

  for (QComboBox* combo : {_edit_emote, _waypoint_emote})
  {
    auto* items = qobject_cast<QStandardItemModel*>(combo->model());
    if (!items) continue;
    for (int index = 0; index < combo->count(); ++index)
    {
      unsigned const emote_id = combo->itemData(index).toUInt();
      unsigned const procedure = combo->itemData(index, EmoteProcedureRole).toUInt();
      unsigned const animation_id = combo == _edit_emote
        ? emotePreviewAnimation(index) : waypointPreviewAnimation(emote_id);
      bool const supported = emote_id == 0
        || ((animation_id != 0 || procedure == 1)
            && model->hasAnimation(static_cast<std::uint16_t>(animation_id)));
      if (auto* item = items->item(index))
      {
        item->setEnabled(supported);
        if (!supported)
          item->setToolTip(QString("Animation %1 is not present in this NPC model.").arg(animation_id));
        else if (procedure == 1)
          item->setToolTip(QString("This stand state previews with client animation %1.").arg(animation_id));
        else
          item->setToolTip(QString());
      }
    }
  }

  if (auto* items = qobject_cast<QStandardItemModel*>(_preview_animation->model()))
  {
    for (int index = 0; index < _preview_animation->count(); ++index)
    {
      unsigned const animation_id = _preview_animation->itemData(index).toUInt();
      bool const supported = index == 0
        || model->hasAnimation(static_cast<std::uint16_t>(animation_id));
      if (auto* item = items->item(index))
      {
        item->setEnabled(supported);
        item->setToolTip(supported ? QString()
          : QString("Animation %1 is not present in this NPC model.").arg(animation_id));
      }
    }
    if (_preview_animation->currentIndex() > 0
        && !model->hasAnimation(static_cast<std::uint16_t>(
          _preview_animation->currentData().toUInt())))
    {
      QSignalBlocker const blocker(_preview_animation);
      _preview_animation->setCurrentIndex(0);
    }
  }
}

bool NpcTemplateBrowser::refreshSpawnAppearance(unsigned display_id, unsigned entry,
                                                bool appearance_already_loaded)
{
  if (!_selected_spawn_guid || !_map_view
      || (!appearance_already_loaded && !loadDisplay(display_id, false))) return false;
  auto const appearance = _preview->creatureAppearance();
  if (!appearance) return false;
  World* world = _map_view->getWorld();
  auto* old = world->findNpcSpawnOverlay(*_selected_spawn_guid);
  if (!old) return false;
  glm::vec3 const position = old->anchorPosition();
  float const yaw = old->anchorYaw();
  try
  {
    world->addNpcSpawnOverlay(*_selected_spawn_guid, entry, position, yaw,
                              _selected_model_scale, *appearance);
    world->selectNpcSpawnOverlay(*_selected_spawn_guid);
    updateAnimationAvailability();
    updateMovementPreview();
    return true;
  }
  catch (std::exception const&) { return false; }
}

void NpcTemplateBrowser::saveMovementSettings()
{
  if (!_selected_spawn_guid) return;
  int const movement = _edit_movement->currentData().toInt();
  double const radius = movement == 1 ? _edit_wander->value() : 0.0;
  if (movement == 1 && radius <= 0.0)
  {
    _status->setText("Set a wandering radius greater than zero for random movement.");
    return;
  }
  if (movement == 2 && _waypoints.size() < 2)
  {
    _status->setText("Add and save at least two waypoints before enabling waypoint movement.");
    return;
  }

  double const walk_speed = _edit_speed_walk->value();
  double const run_speed = _edit_speed_run->value();
  bool const speed_changed = std::abs(walk_speed - _loaded_speed_walk) > 0.00001
    || std::abs(run_speed - _loaded_speed_run) > 0.00001;
  if (speed_changed && _template_spawn_count > 1)
  {
    QMessageBox warning(QMessageBox::Warning, "Shared NPC patrol speed",
      QString("Template %1 is used by %2 placed NPCs.")
        .arg(_selected_spawn_entry).arg(_template_spawn_count),
      QMessageBox::Cancel, this);
    warning.setInformativeText(
      "Saving these speed multipliers changes every spawn that uses this template. "
      "For per-NPC speed, cancel and use Appearance > Create unique template and apply.");
    QPushButton* apply = warning.addButton("Apply to All", QMessageBox::AcceptRole);
    warning.setDefaultButton(QMessageBox::Cancel);
    warning.exec();
    if (warning.clickedButton() != apply) return;
  }

  QString error;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      Sql::WorldDatabaseSchema schema;
      if (!Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;

      QSqlQuery spawn_count(db);
      spawn_count.prepare(QString("SELECT COUNT(*) FROM creature WHERE guid = ? AND %1 = ?")
                            .arg(schema.creatureEntry()));
      spawn_count.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      spawn_count.addBindValue(_selected_spawn_entry);
      if (!spawn_count.exec() || !spawn_count.next() || spawn_count.value(0).toUInt() != 1)
      {
        failure = "The selected creature row is missing or is not unique.";
        return false;
      }

      QSqlQuery template_count(db);
      template_count.prepare("SELECT COUNT(*) FROM creature_template WHERE entry = ?");
      template_count.addBindValue(_selected_spawn_entry);
      if (!template_count.exec() || !template_count.next()
          || template_count.value(0).toUInt() != 1)
      {
        failure = "The selected creature template is missing or is not unique.";
        return false;
      }

      if (movement == 2)
      {
        QSqlQuery path(db);
        path.prepare("SELECT ca.path_id, COUNT(wd.point) "
                     "FROM creature_addon ca LEFT JOIN waypoint_data wd ON wd.id = ca.path_id "
                     "WHERE ca.guid = ? GROUP BY ca.path_id LIMIT 1");
        path.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
        if (!path.exec() || !path.next() || !path.value(0).toUInt()
            || path.value(1).toUInt() < 2)
        {
          failure = "Save the waypoint path before enabling waypoint movement.";
          return false;
        }
      }

      QSqlQuery update_spawn(db);
      update_spawn.prepare(QString("UPDATE creature SET MovementType = ?, %1 = ? "
                                   "WHERE guid = ? AND %2 = ?")
                             .arg(schema.creatureRadius(), schema.creatureEntry()));
      update_spawn.addBindValue(movement);
      update_spawn.addBindValue(radius);
      update_spawn.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      update_spawn.addBindValue(_selected_spawn_entry);
      if (!update_spawn.exec())
      {
        failure = "Could not save movement settings: " + update_spawn.lastError().text();
        return false;
      }

      QSqlQuery update_template(db);
      update_template.prepare("UPDATE creature_template SET speed_walk = ?, speed_run = ? "
                              "WHERE entry = ?");
      update_template.addBindValue(walk_speed);
      update_template.addBindValue(run_speed);
      update_template.addBindValue(_selected_spawn_entry);
      if (!update_template.exec())
      {
        failure = "Could not save patrol speed: " + update_template.lastError().text();
        return false;
      }

      QSqlQuery verify(db);
      verify.prepare(QString("SELECT c.MovementType, %1, ct.speed_walk, ct.speed_run "
                             "FROM creature c JOIN creature_template ct ON ct.entry = %2 "
                             "WHERE c.guid = ? AND %2 = ? LIMIT 1")
                       .arg(schema.creatureRadius("c"), schema.creatureEntry("c")));
      verify.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      verify.addBindValue(_selected_spawn_entry);
      if (!verify.exec() || !verify.next())
      {
        failure = verify.lastError().isValid()
          ? "Could not verify movement settings: " + verify.lastError().text()
          : "The saved movement settings could not be read back.";
        return false;
      }
      if (verify.value(0).toInt() != movement
          || std::abs(verify.value(1).toDouble() - radius) > 0.001
          || std::abs(verify.value(2).toDouble() - walk_speed) > 0.0001
          || std::abs(verify.value(3).toDouble() - run_speed) > 0.0001)
      {
        failure = "The database did not retain the requested movement and speed values.";
        return false;
      }
      return true;
    }, error);
  if (!saved)
  {
    _status->setText("Could not save NPC movement: " + error);
    return;
  }

  _loaded_speed_walk = walk_speed;
  _loaded_speed_run = run_speed;
  commitNpcEdits(EditSpawn | EditTemplate);
  updateMovementPreview();
  _status->setText(QString("Movement saved for GUID %1: %2, radius %3, walk %4x, run %5x. "
                           "Reload the world server to apply it in game.")
    .arg(*_selected_spawn_guid)
    .arg(_edit_movement->currentText())
    .arg(radius, 0, 'f', 1)
    .arg(walk_speed, 0, 'f', 3)
    .arg(run_speed, 0, 'f', 3));
}

void NpcTemplateBrowser::saveSelectedSpawn()
{
  if (!_selected_spawn_guid) return;
  int const movement = _edit_movement->currentData().toInt();
  if (movement == 1 && _edit_wander->value() <= 0.0)
  {
    _status->setText("Set a wandering radius greater than zero for random movement.");
    return;
  }
  if (movement == 2 && _waypoints.empty())
  {
    _status->setText("Add and save a waypoint path before enabling waypoint movement.");
    return;
  }
  unsigned const display = _edit_display->value() > 0
    ? static_cast<unsigned>(_edit_display->value())
    : static_cast<unsigned>(_edit_template_display->value());
  if (!display || !loadDisplay(display, false))
  {
    _status->setText("The selected display ID is unavailable in the current client files.");
    return;
  }
  QString error;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      Sql::WorldDatabaseSchema schema;
      if (!Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
      if (movement == 2)
      {
        QSqlQuery path(db);
        path.prepare("SELECT ca.path_id, COUNT(wd.point) "
                     "FROM creature_addon ca LEFT JOIN waypoint_data wd ON wd.id = ca.path_id "
                     "WHERE ca.guid = ? GROUP BY ca.path_id LIMIT 1");
        path.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
        if (!path.exec() || !path.next() || !path.value(0).toUInt()
            || path.value(1).toUInt() < 2)
        {
          failure = "Save the waypoint path before enabling waypoint movement.";
          return false;
        }
      }
      QSqlQuery row_count(db);
      row_count.prepare(QString("SELECT COUNT(*) FROM creature WHERE guid = ? AND %1 = ?")
                          .arg(schema.creatureEntry()));
      row_count.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      row_count.addBindValue(_selected_spawn_entry);
      if (!row_count.exec() || !row_count.next() || row_count.value(0).toUInt() != 1)
      {
        failure = "The selected creature row is missing or is not unique.";
        return false;
      }
      QSqlQuery update(db);
      QString update_sql = QString("UPDATE creature SET spawntimesecs = ?, %1 = ?, "
                                   "MovementType = ?")
                             .arg(schema.creatureRadius());
      if (!schema.creature_spawn_display_column.isEmpty())
        update_sql += ", " + schema.creatureSpawnDisplay() + " = ?";
      update_sql += QString(", position_x = ?, position_y = ?, position_z = ?, "
                            "orientation = ? WHERE guid = ? AND %1 = ?")
                      .arg(schema.creatureEntry());
      update.prepare(update_sql);
      update.addBindValue(_edit_respawn->value());
      double const saved_radius = movement == 1 ? _edit_wander->value() : 0.0;
      update.addBindValue(saved_radius);
      update.addBindValue(movement);
      if (!schema.creature_spawn_display_column.isEmpty())
        update.addBindValue(_edit_display->value());
      update.addBindValue(ZEROPOINT - _edit_spawn_position.z);
      update.addBindValue(ZEROPOINT - _edit_spawn_position.x);
      update.addBindValue(_edit_spawn_position.y);
      update.addBindValue(serverOrientation(static_cast<float>(_edit_yaw->value())));
      update.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      update.addBindValue(_selected_spawn_entry);
      if (!update.exec())
      {
        failure = update.lastError().text();
        return false;
      }
      QSqlQuery verify(db);
      verify.prepare(QString("SELECT MovementType, %1 FROM creature "
                             "WHERE guid = ? AND %2 = ? LIMIT 1")
                       .arg(schema.creatureRadius(), schema.creatureEntry()));
      verify.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      verify.addBindValue(_selected_spawn_entry);
      if (!verify.exec() || !verify.next()
          || verify.value(0).toInt() != movement
          || std::abs(verify.value(1).toDouble() - saved_radius) > 0.001)
      {
        failure = verify.lastError().isValid()
          ? "Could not verify the saved movement settings: " + verify.lastError().text()
          : "The database did not retain the requested movement settings.";
        return false;
      }
      return true;
    }, error);
  if (!saved)
  {
    _status->setText("Could not save NPC spawn settings: " + error);
    return;
  }
  bool const shown = refreshSpawnAppearance(display, _selected_spawn_entry);
  _saved_spawn_position = _edit_spawn_position;
  _saved_spawn_yaw = static_cast<float>(_edit_yaw->value());
  updateCachedSpawn(*_selected_spawn_guid, _selected_spawn_entry,
                    _edit_spawn_position, display, _saved_spawn_yaw);
  commitNpcEdits(EditTransform | EditSpawn);
  updateMovementPreview();
  _status->setText(shown
    ? "Spawn settings saved. Reload the world server to apply them in game."
    : "Spawn settings saved in the database, but Noggit could not refresh the display preview.");
}

void NpcTemplateBrowser::deleteSelectedSpawn()
{
  if (!_selected_spawn_guid || !_map_view || !_map_view->getWorld()) return;

  std::uint64_t const guid = *_selected_spawn_guid;
  unsigned const entry = _selected_spawn_entry;
  QMessageBox confirmation(QMessageBox::Warning, "Delete NPC spawn",
    QString("Permanently delete NPC spawn GUID %1 (template %2)?").arg(guid).arg(entry),
    QMessageBox::Cancel, this);
  confirmation.setInformativeText(
    "The spawn, its spawn-addon row, and any waypoint path used only by this spawn will be "
    "deleted from the world database. The NPC template will remain. This cannot be undone.");
  QPushButton* delete_button = confirmation.addButton(QMessageBox::Yes);
  delete_button->setText("Delete Spawn");
  confirmation.setDefaultButton(QMessageBox::Cancel);
  confirmation.setEscapeButton(QMessageBox::Cancel);
  if (confirmation.exec() != QMessageBox::Yes) return;

  QString error;
  bool const deleted = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      // Some deployed Qt MySQL drivers report the transaction feature as
      // unavailable even though the connected MySQL/MariaDB server supports
      // transactions. QSqlDatabase::transaction() refuses to issue anything
      // in that case, so use the server commands directly.
      QSqlQuery transaction(db);
      if (!transaction.exec("START TRANSACTION"))
      {
        failure = "Could not start the NPC deletion transaction: "
          + transaction.lastError().text();
        return false;
      }
       auto fail_transaction = [&](QString const& message)
      {
        failure = message;
        QSqlQuery rollback(db);
        rollback.exec("ROLLBACK");
        return false;
       };

       Noggit::Sql::WorldDatabaseSchema schema;
       if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure))
         return fail_transaction(failure);

       QSqlQuery spawn(db);
       spawn.prepare(QString("SELECT %1 FROM creature WHERE guid = ? FOR UPDATE")
                       .arg(schema.creatureEntry()));
      spawn.addBindValue(QVariant::fromValue<qulonglong>(guid));
      if (!spawn.exec())
        return fail_transaction("Could not inspect the NPC spawn: " + spawn.lastError().text());
      if (!spawn.next())
        return fail_transaction("The selected NPC spawn no longer exists in creature.");
      if (spawn.value(0).toUInt() != entry)
        return fail_transaction("The selected NPC spawn changed before it could be deleted.");
      spawn.finish();

      unsigned path_id = 0;
      bool const has_addon = tableExists(db, "creature_addon");
      bool const has_waypoints = tableExists(db, "waypoint_data");
      bool const has_scripts = tableExists(db, "waypoint_scripts");
      if (has_addon)
      {
        QSqlQuery addon(db);
        addon.prepare("SELECT path_id FROM creature_addon WHERE guid = ? FOR UPDATE");
        addon.addBindValue(QVariant::fromValue<qulonglong>(guid));
        if (!addon.exec())
          return fail_transaction("Could not inspect the NPC spawn addon: " + addon.lastError().text());
        if (addon.next()) path_id = addon.value(0).toUInt();
        addon.finish();

        QSqlQuery remove_addon(db);
        remove_addon.prepare("DELETE FROM creature_addon WHERE guid = ?");
        remove_addon.addBindValue(QVariant::fromValue<qulonglong>(guid));
        if (!remove_addon.exec())
          return fail_transaction("Could not delete the NPC spawn addon: "
                                  + remove_addon.lastError().text());
      }

      if (path_id && has_waypoints)
      {
        bool path_is_unshared = true;
        if (has_addon)
        {
          QSqlQuery references(db);
          references.prepare("SELECT 1 FROM creature_addon WHERE path_id = ? LIMIT 1");
          references.addBindValue(path_id);
          if (!references.exec())
            return fail_transaction("Could not check waypoint path ownership: "
                                    + references.lastError().text());
          path_is_unshared = !references.next();
        }

        if (path_is_unshared)
        {
          std::vector<unsigned> action_ids;
          if (has_scripts)
          {
            QSqlQuery actions(db);
            actions.prepare("SELECT DISTINCT action FROM waypoint_data "
                            "WHERE id = ? AND action <> 0");
            actions.addBindValue(path_id);
            if (!actions.exec())
              return fail_transaction("Could not inspect waypoint actions: "
                                      + actions.lastError().text());
            while (actions.next()) action_ids.push_back(actions.value(0).toUInt());
          }

          QSqlQuery remove_path(db);
          remove_path.prepare("DELETE FROM waypoint_data WHERE id = ?");
          remove_path.addBindValue(path_id);
          if (!remove_path.exec())
            return fail_transaction("Could not delete the NPC waypoint path: "
                                    + remove_path.lastError().text());

          for (unsigned const action_id : action_ids)
          {
            QSqlQuery action_references(db);
            action_references.prepare("SELECT 1 FROM waypoint_data WHERE action = ? LIMIT 1");
            action_references.addBindValue(action_id);
            if (!action_references.exec())
              return fail_transaction("Could not check waypoint action ownership: "
                                      + action_references.lastError().text());
            if (action_references.next()) continue;

            QSqlQuery remove_action(db);
            remove_action.prepare("DELETE FROM waypoint_scripts WHERE id = ?");
            remove_action.addBindValue(action_id);
            if (!remove_action.exec())
              return fail_transaction("Could not delete a waypoint action: "
                                      + remove_action.lastError().text());
          }
        }
      }

      QSqlQuery remove_spawn(db);
       remove_spawn.prepare(QString("DELETE FROM creature WHERE guid = ? AND %1 = ?")
                              .arg(schema.creatureEntry()));
      remove_spawn.addBindValue(QVariant::fromValue<qulonglong>(guid));
      remove_spawn.addBindValue(entry);
      if (!remove_spawn.exec())
        return fail_transaction("Could not delete the NPC spawn: " + remove_spawn.lastError().text());
      if (remove_spawn.numRowsAffected() != 1)
        return fail_transaction("The NPC spawn changed before the deletion was committed.");

      QSqlQuery commit(db);
      if (!commit.exec("COMMIT"))
        return fail_transaction("Could not commit the NPC deletion: " + commit.lastError().text());
      return true;
    }, error);
  if (!deleted)
  {
    QString const message = "Could not delete NPC spawn: " + error;
    _status->setText(message);
    QMessageBox::critical(_properties_panel, "NPC spawn was not deleted", message);
    return;
  }

  NOGGIT_ACTION_MGR->discardNpcEdits(guid, EditAll);
  _draft_snapshots.erase(guid);
  removeCachedSpawn(guid);
  QSettings().remove(weaponVisibilitySettingsKey(guid));
  _map_view->getWorld()->removeNpcSpawnOverlay(guid);
  _selected_spawn_guid.reset();
  _selected_spawn_entry = 0;
  _capturing_waypoints = false;
  _moving_spawn = false;
  _selected_spawn_rotating = false;
  _pending_transform_edit = false;
  _waypoints.clear();
  updateWaypointList();
  _spawn_editor->hide();
  if (_phase_assignment_panel) _phase_assignment_panel->hide();
  if (_waypoint_panel) _waypoint_panel->setEnabled(false);
  _map_view->invalidate();
  _map_view->update();
  _status->setText(QString("Deleted NPC spawn GUID %1. Reload the world server to remove it in game.")
                   .arg(guid));
}

void NpcTemplateBrowser::deleteSelectedCustomNpc()
{
  unsigned const entry = selectedTemplateEntry();
  unsigned const display_id = selectedTemplateDisplay();
  unsigned extra_id = 0;
  QString texture_stem;
  if (!entry || !generatedNpcAssets(display_id, &extra_id, &texture_stem))
  {
    QMessageBox::information(this, "NPC template is protected",
      "Only humanoid NPCs created from scratch by Noggit can be deleted from this list. "
      "Built-in and shared server templates are protected.");
    return;
  }
  QString deployment_configuration_error;
  if (!npcDbcDeploymentConfigured(deployment_configuration_error))
  {
    QMessageBox::critical(this, "DBC deployment folders required",
                          deployment_configuration_error);
    _connection_toggle->setChecked(true);
    return;
  }

  QString npc_name;
  unsigned spawn_count = 0;
  unsigned shared_display_count = 0;
  QString error;
  bool const inspected = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
     false, [&](QSqlDatabase& db, QString& failure)
     {
       Noggit::Sql::WorldDatabaseSchema schema;
       if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
       QSqlQuery npc(db);
      npc.prepare("SELECT name FROM creature_template WHERE entry = ?");
      npc.addBindValue(entry);
      if (!npc.exec() || !npc.next())
      {
        failure = "The selected NPC template no longer exists: " + npc.lastError().text();
        return false;
      }
      npc_name = npc.value(0).toString();

      QSqlQuery spawns(db);
       spawns.prepare(QString("SELECT COUNT(*) FROM creature WHERE %1")
                        .arg(schema.creatureReferencesTemplate()));
      spawns.addBindValue(entry);
      if (!spawns.exec() || !spawns.next())
      {
        failure = "Could not check for placed NPC spawns: " + spawns.lastError().text();
        return false;
      }
      spawn_count = spawns.value(0).toUInt();

      QSqlQuery displays(db);
       if (schema.uses_creature_template_model)
       {
         displays.prepare("SELECT COUNT(DISTINCT `CreatureID`) FROM `creature_template_model` "
                          "WHERE `CreatureID` <> ? AND `CreatureDisplayID` = ?");
         displays.addBindValue(entry);
         displays.addBindValue(display_id);
       }
       else
       {
         displays.prepare("SELECT COUNT(*) FROM creature_template WHERE entry <> ? AND "
                          "(modelid1 = ? OR modelid2 = ? OR modelid3 = ? OR modelid4 = ?)");
         displays.addBindValue(entry);
         for (int index = 0; index < 4; ++index) displays.addBindValue(display_id);
       }
      if (!displays.exec() || !displays.next())
      {
        failure = "Could not check custom display ownership: " + displays.lastError().text();
        return false;
      }
      shared_display_count = displays.value(0).toUInt();
      return true;
    }, error);
  if (!inspected)
  {
    QMessageBox::critical(this, "NPC could not be inspected", error);
    return;
  }
  if (spawn_count)
  {
    QMessageBox::warning(this, "Delete placed spawns first",
      QString("%1 still has %2 placed spawn(s). Select each placed NPC and use "
              "Delete NPC spawn before deleting its template from the list.")
        .arg(npc_name).arg(spawn_count));
    return;
  }
  if (shared_display_count)
  {
    QMessageBox::warning(this, "Custom display is still shared",
      QString("Display %1 is referenced by %2 other NPC template(s). Change those templates "
              "before deleting this NPC so their client appearance is not removed.")
        .arg(display_id).arg(shared_display_count));
    return;
  }

  QMessageBox confirmation(QMessageBox::Warning, "Delete custom NPC",
    QString("Permanently delete %1 (template %2)?").arg(npc_name).arg(entry),
    QMessageBox::Cancel, this);
  confirmation.setInformativeText(
    QString("This removes the creature template and its related database rows, custom display "
            "%1, its creature_model_info row, display-extra row %2, and baked texture %3.blp. "
            "This cannot be undone.")
      .arg(display_id).arg(extra_id).arg(texture_stem));
  QPushButton* delete_button = confirmation.addButton(QMessageBox::Yes);
  delete_button->setText("Delete Custom NPC");
  confirmation.setDefaultButton(QMessageBox::Cancel);
  confirmation.setEscapeButton(QMessageBox::Cancel);
  if (confirmation.exec() != QMessageBox::Yes) return;

  DBCFile const previous_display_info(*_display_info);
  DBCFile const previous_display_extra(*_display_extra);
  bool client_assets_changed = false;
  auto restore_client_assets = [&]
  {
    _display_info->overwriteWith(previous_display_info);
    _display_extra->overwriteWith(previous_display_extra);
    _display_info->save();
    _display_extra->save();
    client_assets_changed = false;
  };

  error.clear();
  bool const deleted = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
     true, [&](QSqlDatabase& db, QString& failure)
     {
      Noggit::Sql::WorldDatabaseSchema schema;
      if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
      std::array<QVariant, 4> removed_model_info{};
      bool model_info_removed = false;
      QSqlQuery begin(db);
      if (!begin.exec("START TRANSACTION"))
      {
        failure = "Could not start the custom NPC deletion transaction: "
          + begin.lastError().text();
        return false;
      }
      auto fail_transaction = [&](QString message)
      {
        QSqlQuery rollback(db);
        rollback.exec("ROLLBACK");
        if (model_info_removed && !schema.uses_creature_template_model)
        {
          // MyISAM deletions are not restored by ROLLBACK.
          QSqlQuery restore(db);
          restore.prepare("INSERT IGNORE INTO creature_model_info "
                          "(DisplayID, BoundingRadius, CombatReach, Gender, "
                          "DisplayID_Other_Gender) VALUES (?, ?, ?, ?, ?)");
          restore.addBindValue(display_id);
          for (QVariant const& value : removed_model_info) restore.addBindValue(value);
          if (!restore.exec())
            message += " Could not restore the creature_model_info row: "
              + restore.lastError().text();
        }
        if (client_assets_changed)
        {
          try { restore_client_assets(); }
          catch (std::exception const& exception)
          {
            message += QString(" The client DBC backup also could not be restored: %1")
              .arg(exception.what());
          }
        }
        failure = std::move(message);
        return false;
      };

      QSqlQuery current(db);
      current.prepare("SELECT entry FROM creature_template WHERE entry = ? FOR UPDATE");
      current.addBindValue(entry);
      if (!current.exec() || !current.next())
        return fail_transaction("The selected NPC template no longer exists.");
      current.finish();
      unsigned current_display = 0;
      QString display_error;
      if (!Noggit::Sql::loadTemplateDisplay(db, schema, entry, current_display,
                                             display_error, true)
          || current_display != display_id)
        return fail_transaction("The selected NPC template changed before it could be deleted.");

      QSqlQuery remaining_spawns(db);
      remaining_spawns.prepare(QString("SELECT 1 FROM creature WHERE %1 LIMIT 1 FOR UPDATE")
                                 .arg(schema.creatureReferencesTemplate()));
      remaining_spawns.addBindValue(entry);
      if (!remaining_spawns.exec())
        return fail_transaction("Could not recheck placed NPC spawns: "
                                + remaining_spawns.lastError().text());
      if (remaining_spawns.next())
        return fail_transaction("This NPC gained a placed spawn before deletion. Delete the spawn first.");

      struct RelatedTable { char const* table; char const* key; char const* condition = nullptr; };
      static RelatedTable const related[] = {
        {"creature_template_model", "CreatureID"},
        {"creature_equip_template", "CreatureID"},
        {"npc_vendor", "entry"},
        {"creature_queststarter", "id"},
        {"creature_questender", "id"},
        {"creature_default_trainer", "CreatureId"},
        {"creature_template_addon", "entry"},
        {"creature_template_spell", "CreatureID"},
        {"creature_template_resistance", "CreatureID"},
        {"creature_template_movement", "CreatureId"},
        {"creature_template_locale", "entry"},
        {"creature_text", "CreatureID"},
        {"smart_scripts", "entryorguid", " AND `source_type` = 0"}
      };
      for (RelatedTable const& table : related)
      {
        if (!tableExists(db, table.table)) continue;
        QSqlQuery remove(db);
        remove.prepare(QString("DELETE FROM `%1` WHERE `%2` = ?%3")
          .arg(table.table, table.key, table.condition ? table.condition : ""));
        remove.addBindValue(entry);
        if (!remove.exec())
          return fail_transaction(QString("Could not delete related rows from %1: %2")
                                  .arg(table.table, remove.lastError().text()));
      }

      QSqlQuery remove_template(db);
      remove_template.prepare("DELETE FROM creature_template WHERE entry = ?");
      remove_template.addBindValue(entry);
      if (!remove_template.exec() || remove_template.numRowsAffected() != 1)
        return fail_transaction("Could not delete the creature template: "
                                + remove_template.lastError().text());

      try
      {
        client_assets_changed = true;
        _display_info->removeRecord(display_id);
        _display_extra->removeRecord(extra_id);
        _display_info->save();
        _display_extra->save();
      }
      catch (std::exception const& exception)
      {
        return fail_transaction(QString("Could not remove the generated client display records: %1")
                                .arg(exception.what()));
      }

      QSqlQuery model_info(db);
      model_info.prepare("SELECT BoundingRadius, CombatReach, Gender, DisplayID_Other_Gender "
                         "FROM creature_model_info WHERE DisplayID = ?");
      model_info.addBindValue(display_id);
      if (!model_info.exec())
        return fail_transaction("Could not inspect creature_model_info: "
                                + model_info.lastError().text());
      if (model_info.next())
      {
        for (int column = 0; column < 4; ++column)
          removed_model_info[column] = model_info.value(column);
        model_info.finish();
        QSqlQuery remove_model_info(db);
        remove_model_info.prepare("DELETE FROM creature_model_info WHERE DisplayID = ?");
        remove_model_info.addBindValue(display_id);
        if (!remove_model_info.exec() || remove_model_info.numRowsAffected() != 1)
          return fail_transaction("Could not delete creature_model_info: "
                                  + remove_model_info.lastError().text());
        model_info_removed = true;
      }
      else model_info.finish();

      QSqlQuery commit(db);
      if (!commit.exec("COMMIT"))
        return fail_transaction("Could not commit the custom NPC deletion: "
                                + commit.lastError().text());
      return true;
    }, error);
  if (!deleted)
  {
    QMessageBox::critical(this, "Custom NPC was not deleted", error);
    _status->setText("Could not delete custom NPC: " + error);
    return;
  }

  cancelPlacement();
  QString dbc_deployment_error;
  bool const dbcs_deployed = deployNpcDbcs(dbc_deployment_error);
  QString const texture_path = QDir(QString::fromStdString(_client_data->projectPath()))
    .filePath(QString("Textures/BakedNpcTextures/%1.blp").arg(texture_stem));
  bool const texture_removed = !QFile::exists(texture_path) || QFile::remove(texture_path);
  loadTemplates();
  QString message = texture_removed
    ? QString("Deleted custom NPC template %1 and its generated client assets.").arg(entry)
    : QString("Deleted custom NPC template %1 and its DBC rows, but the unused texture could "
              "not be removed: %2").arg(entry).arg(texture_path);
  if (!dbcs_deployed)
    message += " Updated DBC deployment failed: " + dbc_deployment_error;
  _status->setText(message);
  if (!texture_removed || !dbcs_deployed)
    QMessageBox::warning(this, "Custom NPC deleted with cleanup warnings", message
      + (dbcs_deployed ? QString()
         : "\n\nCorrect the deployment folders and use Deploy NPC DBCs now."));
}

void NpcTemplateBrowser::saveServerEmote()
{
  if (!_selected_spawn_guid) return;
  QString error;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      if (!tableExists(db, "creature_addon"))
      {
        failure = "creature_addon is unavailable.";
        return false;
      }
      QSqlQuery query(db);
      query.prepare("INSERT INTO creature_addon (guid, emote) VALUES (?, ?) "
                    "ON DUPLICATE KEY UPDATE emote = VALUES(emote)");
      query.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      query.addBindValue(_edit_emote->currentData().toUInt());
      if (!query.exec())
      {
        failure = query.lastError().text();
        return false;
      }
      return true;
    }, error);
  if (saved)
  {
    commitNpcEdits(EditEmote);
    if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
      overlay->restartPreviewAnimation();
    updateMovementPreview();
    _status->setText("Server emote saved and applied to the Noggit preview. "
                     "Reload the world server to see it in game.");
  }
  else
  {
    _status->setText("Could not save the server emote: " + error);
  }
}

bool NpcTemplateBrowser::writeSpawnWeaponStance(QSqlDatabase& db, std::uint64_t guid,
                                                 unsigned entry, unsigned stance,
                                                 QString& error) const
{
  if (stance != 1 && stance != 2)
  {
    error = "Choose melee or ranged as the drawn weapon.";
    return false;
  }
  if (!tableExists(db, "creature_addon"))
  {
    error = "creature_addon is unavailable.";
    return false;
  }

  Noggit::Sql::WorldDatabaseSchema schema;
  if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, error)) return false;

  QSqlQuery spawn(db);
  spawn.prepare(QString("SELECT %1 FROM creature WHERE guid = ? LIMIT 1")
                  .arg(schema.creatureEntry()));
  spawn.addBindValue(QVariant::fromValue<qulonglong>(guid));
  if (!spawn.exec() || !spawn.next() || spawn.value(0).toUInt() != entry)
  {
    error = spawn.lastError().isValid() ? spawn.lastError().text()
      : "The NPC spawn no longer matches the selected template.";
    return false;
  }
  spawn.finish();

  QSqlQuery existing(db);
  existing.prepare("SELECT bytes2 FROM creature_addon WHERE guid = ? LIMIT 1");
  existing.addBindValue(QVariant::fromValue<qulonglong>(guid));
  if (!existing.exec())
  {
    error = "Could not inspect the NPC's weapon stance: " + existing.lastError().text();
    return false;
  }
  if (existing.next())
  {
    unsigned const bytes2 = (existing.value(0).toUInt() & ~0xffu) | stance;
    existing.finish();
    QSqlQuery update(db);
    update.prepare("UPDATE creature_addon SET bytes2 = ? WHERE guid = ?");
    update.addBindValue(bytes2);
    update.addBindValue(QVariant::fromValue<qulonglong>(guid));
    if (!update.exec())
    {
      error = "Could not save the NPC's weapon stance: " + update.lastError().text();
      return false;
    }
    return true;
  }
  existing.finish();

  // A spawn addon replaces its template addon on WotLK servers. Seed the new
  // row with every template addon field so mounts, auras, emotes, and paths
  // remain intact when only this spawn's sheath state changes.
  if (tableExists(db, "creature_template_addon"))
  {
    QSqlQuery templ(db);
    templ.prepare("SELECT path_id, mount, bytes1, bytes2, emote, "
                  "visibilityDistanceType, auras "
                  "FROM creature_template_addon WHERE entry = ? LIMIT 1");
    templ.addBindValue(entry);
    if (!templ.exec())
    {
      error = "Could not inspect the template addon: " + templ.lastError().text();
      return false;
    }
    if (templ.next())
    {
      std::array<QVariant, 7> values;
      for (int index = 0; index < 7; ++index) values[index] = templ.value(index);
      templ.finish();
      QSqlQuery insert(db);
      insert.prepare("INSERT INTO creature_addon "
        "(guid, path_id, mount, bytes1, bytes2, emote, visibilityDistanceType, auras) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
      insert.addBindValue(QVariant::fromValue<qulonglong>(guid));
      insert.addBindValue(values[0]);
      insert.addBindValue(values[1]);
      insert.addBindValue(values[2]);
      insert.addBindValue((values[3].toUInt() & ~0xffu) | stance);
      insert.addBindValue(values[4]);
      insert.addBindValue(values[5]);
      insert.addBindValue(values[6]);
      if (!insert.exec())
      {
        error = "Could not save the NPC's weapon stance: " + insert.lastError().text();
        return false;
      }
      return true;
    }
  }

  QSqlQuery insert(db);
  insert.prepare("INSERT INTO creature_addon (guid, bytes2) VALUES (?, ?)");
  insert.addBindValue(QVariant::fromValue<qulonglong>(guid));
  insert.addBindValue(stance);
  if (!insert.exec())
  {
    error = "Could not save the NPC's weapon stance: " + insert.lastError().text();
    return false;
  }
  return true;
}

void NpcTemplateBrowser::previewSelectedWeaponStance()
{
  if (!_selected_spawn_guid || !_map_view || !_map_view->getWorld()) return;
  unsigned const stance = _edit_weapon_stance->currentData().toUInt();
  if (stance != 1 && stance != 2) return;
  auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid);
  if (!overlay) return;
  bool const show_melee = stance == 1;
  overlay->setWeaponVisibility(show_melee, !show_melee);
  QSignalBlocker const main_blocker(_show_main_off_hand);
  QSignalBlocker const ranged_blocker(_show_ranged_weapon);
  _show_main_off_hand->setChecked(show_melee);
  _show_ranged_weapon->setChecked(!show_melee);
  _map_view->invalidate();
  _map_view->update();
}

void NpcTemplateBrowser::saveSelectedWeaponStance()
{
  if (!_selected_spawn_guid) return;
  int const stance = _edit_weapon_stance->currentData().toInt();
  if (stance != 1 && stance != 2)
  {
    _status->setText("Choose melee or ranged before saving the in-game weapon stance.");
    return;
  }
  QString error;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      return writeSpawnWeaponStance(db, *_selected_spawn_guid, _selected_spawn_entry,
                                    static_cast<unsigned>(stance), failure);
    }, error);
  if (!saved)
  {
    _status->setText("Could not save the in-game weapon stance: " + error);
    return;
  }
  previewSelectedWeaponStance();
  saveSelectedWeaponVisibility();
  _status->setText("In-game weapon stance saved for this spawn. Reload the world server "
                   "to see the drawn weapon in game.");
}

void NpcTemplateBrowser::openEditorFromToolbar()
{
  if (_selected_spawn_guid)
    openNpcEditor(true);
  else if (selectedTemplateEntry())
    openNpcEditor(false);
  else
    openNpcEditor(false, true);
}

void NpcTemplateBrowser::openNpcEditor(bool selected_spawn, bool character_creator)
{
  if (character_creator && (!_chr_races || !_char_sections || !_hair_geosets
      || !_facial_hair_styles || !_display_info || !_display_extra || !_model_data))
  {
    QMessageBox::critical(this, "Character creator is unavailable",
      "One or more required WotLK character DBC files could not be loaded.");
    return;
  }
  unsigned source_entry = selected_spawn ? _selected_spawn_entry : selectedTemplateEntry();
  if ((!source_entry && !character_creator) || (selected_spawn && !_selected_spawn_guid))
  {
    _status->setText(selected_spawn
      ? "Select a saved NPC spawn before opening the NPC editor."
      : "Select an NPC template to use as the starting point.");
    return;
  }

  struct Draft
  {
    QString name;
    QString subname;
    int min_level = 1;
    int max_level = 1;
    int faction = 0;
    int display = 0;
    int npc_flags = 0;
    int type = 7;
    int rank = 0;
    int main_hand = 0;
    int off_hand = 0;
    int ranged = 0;
  } draft;

  struct CharacterDraft
  {
    bool available = false;
    unsigned race = 0;
    unsigned sex = 0;
    unsigned skin = 0;
    unsigned face = 0;
    unsigned hair_style = 0;
    unsigned hair_color = 0;
    unsigned facial_hair = 0;
    std::array<unsigned, 11> armor_displays{};
  } character_draft;

  std::vector<NpcDialogueSoundRow> dialogue_sound_rows;
  NpcDialogueSoundContext dialogue_sound_context;

  QString error;
  bool const inspected = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
     false, [&](QSqlDatabase& db, QString& failure)
     {
       Noggit::Sql::WorldDatabaseSchema schema;
       if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
       if (!source_entry && character_creator)
      {
        QSqlQuery source(db);
        if (!source.exec("SELECT entry FROM creature_template WHERE type = 7 AND npcflag = 0 "
                         "AND COALESCE(AIName, '') = '' AND COALESCE(ScriptName, '') = '' "
                         "ORDER BY entry LIMIT 1") || !source.next())
        {
          failure = "Could not find a safe humanoid template to use for database defaults.";
          return false;
        }
        source_entry = source.value(0).toUInt();
      }
      QSqlQuery templ(db);
      templ.prepare("SELECT * FROM creature_template WHERE entry = ? LIMIT 1");
      templ.addBindValue(source_entry);
      if (!templ.exec() || !templ.next())
      {
        failure = templ.lastError().isValid() ? templ.lastError().text()
          : "The source creature template does not exist.";
        return false;
      }
      draft.name = field(templ, "name").toString();
      draft.subname = field(templ, "subname").toString();
      draft.min_level = std::max(1, field(templ, "minlevel").toInt());
      draft.max_level = std::max(draft.min_level, field(templ, "maxlevel").toInt());
       draft.faction = field(templ, "faction").toInt();
       draft.npc_flags = field(templ, "npcflag").toInt();
       draft.type = field(templ, "type").toInt();
       draft.rank = field(templ, "rank").toInt();
       dialogue_sound_context.source_entry = source_entry;
       dialogue_sound_context.ai_name = field(templ, "AIName").toString();
       dialogue_sound_context.script_name = field(templ, "ScriptName").toString();
       dialogue_sound_context.selected_spawn_guid = selected_spawn
         ? *_selected_spawn_guid : 0;
       templ.finish();
       unsigned source_display = 0;
       if (!Noggit::Sql::loadTemplateDisplay(db, schema, source_entry, source_display, failure))
         return false;
       draft.display = static_cast<int>(source_display);

       if (tableExists(db, "creature_equip_template"))
      {
        int equipment_id = 1;
        if (selected_spawn)
        {
          QSqlQuery spawn(db);
          spawn.prepare("SELECT equipment_id FROM creature WHERE guid = ? LIMIT 1");
          spawn.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
          if (spawn.exec() && spawn.next() && spawn.value(0).toInt() > 0)
            equipment_id = spawn.value(0).toInt();
        }
        QSqlQuery equipment(db);
        equipment.prepare("SELECT ItemID1, ItemID2, ItemID3 FROM creature_equip_template "
                          "WHERE CreatureID = ? AND ID = ? LIMIT 1");
        equipment.addBindValue(source_entry);
        equipment.addBindValue(equipment_id);
        if (equipment.exec() && equipment.next())
        {
          draft.main_hand = equipment.value(0).toInt();
          draft.off_hand = equipment.value(1).toInt();
          draft.ranged = equipment.value(2).toInt();
         }
       }

       dialogue_sound_context.creature_text_available =
         !character_creator && tableExists(db, "creature_text");
       dialogue_sound_context.broadcast_text_available =
         !character_creator && tableExists(db, "broadcast_text");
       if (dialogue_sound_context.creature_text_available)
       {
         bool const has_broadcast_text = dialogue_sound_context.broadcast_text_available;
         QSqlQuery dialogue(db);
         QString sql =
           "SELECT ct.GroupID, ct.ID, ct.Text, ct.Probability, ct.Sound, ct.BroadcastTextId, ";
         sql += has_broadcast_text ? "COALESCE(bt.SoundEntriesID, 0) " : "0 ";
         sql += "FROM creature_text ct ";
         if (has_broadcast_text)
           sql += "LEFT JOIN broadcast_text bt ON bt.ID = ct.BroadcastTextId ";
         sql += "WHERE ct.CreatureID = ? ORDER BY ct.GroupID, ct.ID";
         dialogue.prepare(sql);
         dialogue.addBindValue(source_entry);
         if (!dialogue.exec())
         {
           failure = "Could not load scripted dialogue sounds: " + dialogue.lastError().text();
           return false;
         }
          while (dialogue.next())
          {
            dialogue_sound_rows.push_back({
             dialogue.value(0).toUInt(), dialogue.value(1).toUInt(),
             dialogue.value(2).toString(), dialogue.value(3).toDouble(),
             dialogue.value(4).toUInt(), dialogue.value(5).toUInt(),
              dialogue.value(6).toUInt()});
          }
        }

        if (selected_spawn)
          dialogue_sound_context.spawn_sources.push_back(
            QString("Placed world spawn GUID %1 uses this template.").arg(*_selected_spawn_guid));

        bool trigger_scan_succeeded = false;
        dialogue_sound_context.smart_scripts_available =
          !character_creator && tableExists(db, "smart_scripts");
        if (dialogue_sound_context.smart_scripts_available)
        {
          QSqlQuery triggers(db);
          QString trigger_sql =
            "SELECT entryorguid, id, event_type, event_chance, event_param1, event_param2, "
            "event_param3, event_param4, action_param1, action_param4, "
            "action_param5, comment "
            "FROM smart_scripts WHERE source_type = 0 AND action_type IN (1, 84) "
            "AND (entryorguid = ?";
          if (selected_spawn) trigger_sql += " OR entryorguid = ?";
          trigger_sql += ") ORDER BY entryorguid DESC, id";
          triggers.prepare(trigger_sql);
          triggers.addBindValue(source_entry);
          if (selected_spawn)
            triggers.addBindValue(-static_cast<qlonglong>(*_selected_spawn_guid));
          if (triggers.exec())
          {
            trigger_scan_succeeded = true;
            while (triggers.next())
            {
              unsigned const group = triggers.value(8).toUInt();
              auto const found = std::find_if(dialogue_sound_rows.begin(), dialogue_sound_rows.end(),
                [group](NpcDialogueSoundRow const& row) { return row.group_id == group; });
              if (found == dialogue_sound_rows.end()) continue;
              qlonglong const owner = triggers.value(0).toLongLong();
              QString event = smartEventLabel(
                triggers.value(2).toUInt(), triggers.value(4).toUInt(),
                triggers.value(5).toUInt(), triggers.value(6).toUInt(),
                triggers.value(7).toUInt());
              unsigned const talk_delay = triggers.value(9).toUInt();
              if (talk_delay) event += QString("; speaks after %1 ms").arg(talk_delay);
              NpcDialogueTriggerReference const reference = {
                event,
                triggers.value(3).toUInt(),
                owner < 0 ? QString("selected spawn GUID %1").arg(-owner)
                          : QString("all template %1 instances").arg(source_entry),
                triggers.value(11).toString().trimmed(),
                false,
                0,
                owner,
                triggers.value(1).toUInt(),
                triggers.value(2).toUInt(),
                triggers.value(10).toUInt()};
              for (auto& row : dialogue_sound_rows)
                if (row.group_id == group) row.triggers.push_back(reference);
            }
          }

          QSqlQuery timed_callers(db);
          QString timed_sql =
            "SELECT entryorguid, id, event_type, event_chance, event_param1, event_param2, "
            "event_param3, event_param4, action_param1, comment "
            "FROM smart_scripts WHERE source_type = 0 AND action_type = 80 "
            "AND (entryorguid = ?";
          if (selected_spawn) timed_sql += " OR entryorguid = ?";
          timed_sql += ") ORDER BY entryorguid DESC, id";
          timed_callers.prepare(timed_sql);
          timed_callers.addBindValue(source_entry);
          if (selected_spawn)
            timed_callers.addBindValue(-static_cast<qlonglong>(*_selected_spawn_guid));
          if (timed_callers.exec())
          {
            while (timed_callers.next())
            {
              unsigned const list_id = timed_callers.value(8).toUInt();
              QSqlQuery timed_talks(db);
              timed_talks.prepare(
                "SELECT id, action_param1, action_param4, comment FROM smart_scripts "
                "WHERE source_type = 9 AND entryorguid = ? AND action_type IN (1, 84) "
                "ORDER BY id");
              timed_talks.addBindValue(list_id);
              if (!timed_talks.exec()) continue;
              while (timed_talks.next())
              {
                unsigned const group = timed_talks.value(1).toUInt();
                auto const found = std::find_if(dialogue_sound_rows.begin(),
                  dialogue_sound_rows.end(), [group](NpcDialogueSoundRow const& row)
                  { return row.group_id == group; });
                if (found == dialogue_sound_rows.end()) continue;
                QString event = smartEventLabel(
                  timed_callers.value(2).toUInt(), timed_callers.value(4).toUInt(),
                  timed_callers.value(5).toUInt(), timed_callers.value(6).toUInt(),
                  timed_callers.value(7).toUInt());
                event += QString(" → timed action list %1, step %2")
                  .arg(list_id).arg(timed_talks.value(0).toUInt());
                unsigned const talk_delay = timed_talks.value(2).toUInt();
                if (talk_delay) event += QString("; speaks after %1 ms").arg(talk_delay);
                qlonglong const owner = timed_callers.value(0).toLongLong();
                QString comment = timed_talks.value(3).toString().trimmed();
                if (comment.isEmpty()) comment = timed_callers.value(9).toString().trimmed();
                NpcDialogueTriggerReference const reference = {
                  event,
                  timed_callers.value(3).toUInt(),
                  owner < 0 ? QString("selected spawn GUID %1").arg(-owner)
                            : QString("all template %1 instances").arg(source_entry),
                  comment};
                for (auto& row : dialogue_sound_rows)
                  if (row.group_id == group) row.triggers.push_back(reference);
              }
            }
          }

          QSqlQuery smart_summons(db);
          smart_summons.prepare(
            "SELECT entryorguid, source_type, id, event_type, event_chance, event_param1, "
            "event_param2, event_param3, event_param4, comment FROM smart_scripts "
            "WHERE action_type = 12 AND action_param1 = ? ORDER BY source_type, entryorguid, id");
          smart_summons.addBindValue(source_entry);
          if (smart_summons.exec())
          {
            while (smart_summons.next())
            {
              QString source = QString("SmartAI source %1 (type %2), action %3: %4; "
                                       "summons this template.")
                .arg(smart_summons.value(0).toLongLong())
                .arg(smart_summons.value(1).toUInt())
                .arg(smart_summons.value(2).toUInt())
                .arg(smartEventLabel(
                  smart_summons.value(3).toUInt(), smart_summons.value(5).toUInt(),
                  smart_summons.value(6).toUInt(), smart_summons.value(7).toUInt(),
                  smart_summons.value(8).toUInt()));
              QString const comment = smart_summons.value(9).toString().trimmed();
              if (!comment.isEmpty()) source += " " + comment;
              if (!dialogue_sound_context.spawn_sources.contains(source))
                dialogue_sound_context.spawn_sources.push_back(source);
            }
          }
        }

        if (!character_creator && tableExists(db, "event_scripts"))
        {
          QSqlQuery summons(db);
          summons.prepare("SELECT id, delay, datalong2 FROM event_scripts "
                          "WHERE command = 10 AND datalong = ? ORDER BY id, delay");
          summons.addBindValue(source_entry);
          if (summons.exec())
          {
            while (summons.next())
            {
              unsigned const event_id = summons.value(0).toUInt();
              QStringList owners;
              if (tableExists(db, "gameobject_template"))
              {
                QSqlQuery objects(db);
                objects.prepare("SELECT entry, name FROM gameobject_template WHERE "
                                "(type = 3 AND Data6 = ?) OR (type = 10 AND Data2 = ?) "
                                "ORDER BY entry");
                objects.addBindValue(event_id);
                objects.addBindValue(event_id);
                if (objects.exec())
                  while (objects.next())
                    owners.push_back(QString("%1 (gameobject %2)")
                      .arg(objects.value(1).toString()).arg(objects.value(0).toUInt()));
              }
              if (owners.isEmpty()) owners.push_back("Unresolved event source");
              for (QString const& owner : owners)
              {
                QString const source = QString(
                  "%1 → event %2: temporary summon after %3 s for %4 s. "
                  "This event only summons the NPC; it does not trigger creature_text.")
                    .arg(owner).arg(event_id).arg(summons.value(1).toUInt())
                    .arg(summons.value(2).toUInt() / 1000.0, 0, 'g', 6);
                if (!dialogue_sound_context.spawn_sources.contains(source))
                  dialogue_sound_context.spawn_sources.push_back(source);
              }
            }
          }
        }

        if (!trigger_scan_succeeded)
          dialogue_sound_context.trigger_scan_note = tableExists(db, "smart_scripts")
            ? "The SmartAI trigger scan could not be completed."
            : "The world database has no smart_scripts table, so database triggers could not be scanned.";
        else if (!dialogue_sound_context.script_name.isEmpty())
          dialogue_sound_context.trigger_scan_note = QString(
            "No direct SmartAI TALK action references this group. C++ script %1 may call it, "
            "which cannot be proven from the world database.").arg(dialogue_sound_context.script_name);
        else if (dialogue_sound_context.ai_name.compare("SmartAI", Qt::CaseInsensitive) == 0)
          dialogue_sound_context.trigger_scan_note =
            "No direct SmartAI TALK action references this group. Stored dialogue does not run itself.";
        else
          dialogue_sound_context.trigger_scan_note =
            "No direct database TALK action references this group. Core or module code may still call it.";
        return true;
    }, error);
  if (!inspected)
  {
    QMessageBox::critical(this, "NPC editor could not open",
                          "Could not load the source NPC: " + error);
    return;
  }
  if (selected_spawn && _edit_display->value() > 0)
    draft.display = _edit_display->value();

  // Existing playable-race displays carry all character choices and armor
  // visuals in CreatureDisplayInfoExtra. Treat those spawns as full character
  // edits instead of reducing them to a raw display-ID field.
  if (selected_spawn && _display_info && _display_extra
      && _display_extra->getFieldCount() >= 19)
  {
    try
    {
      auto const display_record = _display_info->getByID(static_cast<unsigned>(draft.display));
      unsigned const extra_id = display_record.getUInt(3);
      if (extra_id)
      {
        auto const extra = _display_extra->getByID(extra_id);
        character_draft.available = extra.getUInt(1) != 0 && extra.getUInt(2) < 2;
        character_draft.race = extra.getUInt(1);
        character_draft.sex = extra.getUInt(2);
        character_draft.skin = extra.getUInt(3);
        character_draft.face = extra.getUInt(4);
        character_draft.hair_style = extra.getUInt(5);
        character_draft.hair_color = extra.getUInt(6);
        character_draft.facial_hair = extra.getUInt(7);
        for (std::size_t slot = 0; slot < character_draft.armor_displays.size(); ++slot)
          character_draft.armor_displays[slot] = extra.getUInt(8 + slot);
      }
    }
    catch (...) {}
  }
  bool const character_appearance_editor = character_creator || character_draft.available;
  if (character_appearance_editor && (!_chr_races || !_char_sections || !_hair_geosets
      || !_facial_hair_styles || !_display_info || !_display_extra || !_model_data))
  {
    QMessageBox::critical(this, "Character editor is unavailable",
      "One or more required WotLK character DBC files could not be loaded.");
    return;
  }
  if (character_appearance_editor)
  {
    QString deployment_error;
    if (!npcDbcDeploymentConfigured(deployment_error))
    {
      QMessageBox::critical(this, "DBC deployment folders required", deployment_error);
      _connection_toggle->setChecked(true);
      return;
    }
  }

  // Keep preview files alive until after the dialog's GL widget is destroyed.
  std::unique_ptr<QTemporaryDir> character_preview_files;
  QDialog dialog(this);
  dialog.setWindowTitle(selected_spawn ? "Customize Selected NPC"
                                       : character_creator ? "New Humanoid NPC" : "Create Custom NPC");
  dialog.resize(character_appearance_editor ? 1060 : 560,
                character_appearance_editor ? 620 : 470);
  auto* root = new QVBoxLayout(&dialog);
  auto* introduction = new QLabel(
     selected_spawn
       ? QString("Create unique NPC changes only this spawn. Save dialogue to template "
                 "shares dialogue lines with every spawn using template %1; triggers follow "
                 "their chosen scope. Other tabs require Create unique NPC.").arg(source_entry)
      : character_creator
        ? QString("Build a new playable-race NPC. Noggit will generate a baked body texture, "
                  "new client display records, and a clean world-database template using template "
                  "%1 only for safe schema defaults.").arg(source_entry)
      : QString("Template %1 is the starting point. Saving creates a new template and immediately "
                "starts the placement tool.").arg(source_entry),
    &dialog);
  introduction->setWordWrap(true);
  root->addWidget(introduction);

  auto* tabs = new QTabWidget(&dialog);
  auto* editor_row = new QHBoxLayout();
  editor_row->addWidget(tabs, 1);
  root->addLayout(editor_row, 1);

  auto* identity_page = new QWidget(tabs);
  auto* identity_form = new QFormLayout(identity_page);
  auto* name = new QLineEdit(draft.name, identity_page);
  auto* subname = new QLineEdit(draft.subname, identity_page);
  auto* min_level = new QSpinBox(identity_page);
  auto* max_level = new QSpinBox(identity_page);
  auto* faction = new NpcFactionSelector(identity_page);
  for (QSpinBox* level : {min_level, max_level}) level->setRange(1, 255);
  min_level->setValue(draft.min_level);
  max_level->setValue(draft.max_level);
  faction->setFactionId(static_cast<unsigned>(std::max(0, draft.faction)));
  identity_form->addRow("Name", name);
  identity_form->addRow("Title / subname", subname);
  identity_form->addRow("Minimum level", min_level);
  identity_form->addRow("Maximum level", max_level);
  identity_form->addRow("Faction template ID", faction);
  tabs->addTab(identity_page, "Identity");

  QImage composed_character_body;
  unsigned character_base_display = 0;
  unsigned character_preview_serial = 0;
  std::string loaded_character_model;
  QComboBox* character_race = nullptr;
  QComboBox* character_sex = nullptr;
  QComboBox* character_skin = nullptr;
  QComboBox* character_face = nullptr;
  QComboBox* character_hair_style = nullptr;
  QComboBox* character_hair_color = nullptr;
  QComboBox* character_facial_hair = nullptr;
  std::array<unsigned, 11> character_armor_displays = character_draft.armor_displays;
  Tools::AssetBrowser::ModelViewer* character_preview = nullptr;
  std::function<void()> rebuild_character_preview;
  std::function<void()> refresh_character_equipment;
  std::function<void()> refresh_selected_spawn_preview;

  auto* appearance_page = new QWidget(tabs);
  auto* appearance_layout = new QVBoxLayout(appearance_page);
  auto* appearance_form = new QFormLayout();
  auto* display = new QSpinBox(appearance_page);
  display->setRange(1, std::numeric_limits<int>::max());
  display->setValue(std::max(1, draft.display));
  auto* display_status = new QLabel(appearance_page);
  display_status->setWordWrap(true);
  if (!character_appearance_editor)
  {
    appearance_form->addRow("Creature display ID", display);
    appearance_layout->addLayout(appearance_form);
    auto* validate_display = new QPushButton("Validate display in client files", appearance_page);
    appearance_layout->addWidget(validate_display);
    appearance_layout->addWidget(display_status);
    auto* custom_features = new QLabel(
      "Use New humanoid NPC from the Create NPC menu to build a display from race, skin, "
      "face, hair, and facial-hair selections.", appearance_page);
    custom_features->setWordWrap(true);
    custom_features->setStyleSheet("color: palette(mid);");
    appearance_layout->addWidget(custom_features);
    appearance_layout->addStretch();
    connect(validate_display, &QPushButton::clicked, &dialog, [this, display, display_status]
    {
      QString failure;
      bool const valid = loadDisplay(static_cast<unsigned>(display->value()), false, &failure);
      display_status->setText(valid ? "Display is available and can be rendered."
                                    : "Display is unavailable: " + failure);
    });
  }
  else
  {
    QString const preview_parent = QDir(QString::fromStdString(_client_data->projectPath()))
      .filePath("Textures/BakedNpcTextures");
    if (QDir().mkpath(preview_parent))
      character_preview_files = std::make_unique<QTemporaryDir>(
        QDir(preview_parent).filePath("NoggitPreview_XXXXXX"));
    display->hide();
    auto* selectors = new QFormLayout();
    character_race = new QComboBox(appearance_page);
    character_sex = new QComboBox(appearance_page);
    character_skin = new QComboBox(appearance_page);
    character_face = new QComboBox(appearance_page);
    character_hair_style = new QComboBox(appearance_page);
    character_hair_color = new QComboBox(appearance_page);
    character_facial_hair = new QComboBox(appearance_page);
    character_sex->addItem("Male", 0);
    character_sex->addItem("Female", 1);
    selectors->addRow("Race", character_race);
    selectors->addRow("Sex", character_sex);
    selectors->addRow("Skin color", character_skin);
    selectors->addRow("Face", character_face);
    selectors->addRow("Hair style", character_hair_style);
    selectors->addRow("Hair color", character_hair_color);
    selectors->addRow("Facial hair", character_facial_hair);
    auto* selector_widget = new QWidget(appearance_page);
    selector_widget->setLayout(selectors);
    appearance_layout->addWidget(selector_widget);
    character_preview = new Tools::AssetBrowser::ModelViewer(
      &dialog, Noggit::NoggitRenderContext::NPC_CREATOR);
    character_preview->setMinimumSize(360, 390);
    editor_row->addWidget(character_preview, 1);
    appearance_layout->addWidget(display_status);
    auto* output_note = new QLabel(
      "Saving writes a new CreatureDisplayInfo row, CreatureDisplayInfoExtra row, and baked "
      "BLP into this Noggit project. Use Client > Pack client assets after creation, and copy "
      "the generated DBC files to the world server's DBC directory when its core validates "
      "display IDs.", appearance_page);
    output_note->setWordWrap(true);
    output_note->setStyleSheet("color: palette(mid);");
    appearance_layout->addWidget(output_note);
    auto* preview_note = new QLabel(
      "The 3D preview shows the selected race, sex, skin, face, hair, and facial hair. "
      "Drag to turn the model; use the mouse wheel to zoom.", appearance_page);
    preview_note->setWordWrap(true);
    preview_note->setStyleSheet("color: palette(mid);");
    appearance_layout->addWidget(preview_note);

    struct SectionTextures
    {
      std::array<std::string, 3> paths;
      bool found = false;
    };
    constexpr unsigned player_section_flag = 0x01;
    constexpr unsigned death_knight_section_flag = 0x04;
    auto is_standard_player_section = [=](unsigned flags)
    {
      return (flags & player_section_flag) && !(flags & death_knight_section_flag);
    };
    auto find_section = [this, is_standard_player_section]
      (unsigned race, unsigned sex, unsigned section, unsigned variation, unsigned color)
    {
      SectionTextures result;
      if (!_char_sections) return result;
      for (std::size_t row = 0; row < _char_sections->getRecordCount(); ++row)
      {
        auto const record = _char_sections->getRecord(row);
        if (record.getUInt(1) != race || record.getUInt(2) != sex
            || record.getUInt(3) != section || record.getUInt(8) != variation
            || record.getUInt(9) != color
            || !is_standard_player_section(record.getUInt(7)))
          continue;
        for (std::size_t index = 0; index < result.paths.size(); ++index)
          result.paths[index] = characterTexturePath(record.getString(4 + index));
        result.found = true;
        break;
      }
      return result;
    };
    auto values_for = [this, is_standard_player_section]
      (unsigned race, unsigned sex, unsigned section,
       int variation_filter, int color_filter, bool colors)
    {
      std::set<unsigned> values;
      if (!_char_sections) return values;
      for (std::size_t row = 0; row < _char_sections->getRecordCount(); ++row)
      {
        auto const record = _char_sections->getRecord(row);
        if (record.getUInt(1) != race || record.getUInt(2) != sex
            || record.getUInt(3) != section
            || !is_standard_player_section(record.getUInt(7))
            || (variation_filter >= 0 && record.getUInt(8) != static_cast<unsigned>(variation_filter))
            || (color_filter >= 0 && record.getUInt(9) != static_cast<unsigned>(color_filter)))
          continue;
        values.insert(record.getUInt(colors ? 9 : 8));
      }
      return values;
    };
    auto fill_combo = [](QComboBox* combo, std::set<unsigned> const& values,
                         QString const& prefix, unsigned preferred = 0)
    {
      QSignalBlocker const blocker(combo);
      combo->clear();
      for (unsigned value : values)
        combo->addItem(QString("%1 %2").arg(prefix).arg(value + 1), value);
      int index = combo->findData(preferred);
      combo->setCurrentIndex(index >= 0 ? index : (combo->count() ? 0 : -1));
    };

    if (_chr_races)
    {
      for (std::size_t row = 0; row < _chr_races->getRecordCount(); ++row)
      {
        auto const race = _chr_races->getRecord(row);
        unsigned const id = race.getUInt(0);
        if (!id || (!race.getUInt(4) && !race.getUInt(5))) continue;
        bool has_skin = false;
        for (std::size_t section_row = 0; section_row < _char_sections->getRecordCount(); ++section_row)
        {
          auto const section = _char_sections->getRecord(section_row);
          if (section.getUInt(1) == id && section.getUInt(3) == 0
              && is_standard_player_section(section.getUInt(7)))
          {
            has_skin = true;
            break;
          }
        }
        if (!has_skin) continue;
        auto has_model = [this](unsigned display_id)
        {
          if (!display_id) return false;
          try
          {
            auto const display_record = _display_info->getByID(display_id);
            auto const model_record = _model_data->getByID(display_record.getUInt(1));
            std::string model_path = model_record.getString(2);
            if (model_path.size() >= 4)
            {
              QString const extension = QString::fromStdString(model_path.substr(model_path.size() - 4));
              if (extension.compare(".mdx", Qt::CaseInsensitive) == 0
                  || extension.compare(".mdl", Qt::CaseInsensitive) == 0)
                model_path.replace(model_path.size() - 4, 4, ".m2");
            }
            return !model_path.empty()
              && _client_data->exists(BlizzardArchive::Listfile::FileKey(model_path));
          }
          catch (...) { return false; }
        };
        if (!has_model(race.getUInt(4)) && !has_model(race.getUInt(5))) continue;
        QString race_name;
        try { race_name = QString::fromUtf8(race.getLocalizedString(14)); }
        catch (...) {}
        if (race_name.isEmpty()) race_name = QString::fromUtf8(race.getString(6));
        if (race_name.isEmpty()) race_name = QString("Race %1").arg(id);
        character_race->addItem(race_name, id);
      }
    }
    if (character_draft.available)
    {
      int const race_index = character_race->findData(character_draft.race);
      if (race_index >= 0) character_race->setCurrentIndex(race_index);
      int const sex_index = character_sex->findData(character_draft.sex);
      if (sex_index >= 0) character_sex->setCurrentIndex(sex_index);
    }

    std::function<void()> rebuild_hair_colors;
    std::function<void()> rebuild_faces;
    std::function<void()> rebuild_all_options;
    rebuild_hair_colors = [=]
    {
      unsigned const race = character_race->currentData().toUInt();
      unsigned const sex = character_sex->currentData().toUInt();
      int const style = character_hair_style->currentData().toInt();
      unsigned const previous = character_hair_color->currentData().toUInt();
      fill_combo(character_hair_color, values_for(race, sex, 3, style, -1, true),
                 "Color", previous);
      std::set<unsigned> facial;
      if (_facial_hair_styles)
        for (std::size_t row = 0; row < _facial_hair_styles->getRecordCount(); ++row)
        {
          auto const record = _facial_hair_styles->getRecord(row);
          if (record.getUInt(0) == race && record.getUInt(1) == sex)
            facial.insert(record.getUInt(2));
        }
      if (facial.empty()) facial.insert(0);
      fill_combo(character_facial_hair, facial, "Style",
                 character_facial_hair->currentData().toUInt());
    };
    rebuild_faces = [=]
    {
      unsigned const race = character_race->currentData().toUInt();
      unsigned const sex = character_sex->currentData().toUInt();
      int const skin = character_skin->currentData().toInt();
      fill_combo(character_face, values_for(race, sex, 1, -1, skin, false), "Face",
                 character_face->currentData().toUInt());
    };
    rebuild_all_options = [=]
    {
      unsigned const race = character_race->currentData().toUInt();
      unsigned const sex = character_sex->currentData().toUInt();
      std::set<unsigned> skins = values_for(race, sex, 0, -1, -1, true);
      std::set<unsigned> const face_colors = values_for(race, sex, 1, -1, -1, true);
      for (auto skin = skins.begin(); skin != skins.end();)
      {
        if (!face_colors.contains(*skin)) skin = skins.erase(skin);
        else ++skin;
      }
      fill_combo(character_skin, skins, "Skin",
                  character_skin->currentData().toUInt());
      fill_combo(character_hair_style, values_for(race, sex, 3, -1, -1, false), "Style",
                 character_hair_style->currentData().toUInt());
      rebuild_faces();
      rebuild_hair_colors();
    };

    // The timer fires after this block has ended. Capture widget pointers by value;
    // references to the block-local pointer variables would dangle on any edit.
    rebuild_character_preview = [this, &dialog, &character_base_display,
                                 &composed_character_body, &character_preview_serial,
                                 &character_preview_files, &loaded_character_model,
                                 &refresh_character_equipment, &character_armor_displays,
                                 character_preview, display_status,
                                 character_race, character_sex, character_skin, character_face,
                                 character_hair_style, character_hair_color,
                                 character_facial_hair, find_section]
    {
      character_base_display = 0;
      composed_character_body = {};
      if (!character_race->currentData().isValid() || !_chr_races || !_display_info
          || !_model_data || !_char_sections)
      {
        display_status->setText("Character customization data is unavailable in these client files.");
        return;
      }
      unsigned const race_id = character_race->currentData().toUInt();
      unsigned const sex_id = character_sex->currentData().toUInt();
      unsigned const skin_id = character_skin->currentData().toUInt();
      unsigned const face_id = character_face->currentData().toUInt();
      unsigned const hair_style_id = character_hair_style->currentData().toUInt();
      unsigned const hair_color_id = character_hair_color->currentData().toUInt();
      unsigned const facial_hair_id = character_facial_hair->currentData().toUInt();

      SectionTextures const skin = find_section(race_id, sex_id, 0, 0, skin_id);
      SectionTextures const face = find_section(race_id, sex_id, 1, face_id, skin_id);
      SectionTextures const hair = find_section(race_id, sex_id, 3, hair_style_id, hair_color_id);
      SectionTextures const facial = find_section(race_id, sex_id, 2, facial_hair_id, hair_color_id);
      SectionTextures const underwear = find_section(race_id, sex_id, 4, 0, skin_id);
      if (!skin.found || skin.paths[0].empty()
          || !_client_data->exists(BlizzardArchive::Listfile::FileKey(skin.paths[0])))
      {
        display_status->setText("The selected race and skin have no usable base body texture.");
        return;
      }
      composed_character_body = characterTextureImage(skin.paths[0], QSize(256, 256));
      if (composed_character_body.isNull())
      {
        display_status->setText("The selected base skin texture could not be decoded.");
        return;
      }
      if (composed_character_body.size() != QSize(256, 256))
        composed_character_body = composed_character_body.scaled(
          256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

      QPainter painter(&composed_character_body);
      auto paint_layer = [&](std::string const& path, QRect const& target)
      {
        if (path.empty() || !_client_data->exists(BlizzardArchive::Listfile::FileKey(path))) return;
        QImage const layer = characterTextureImage(path, target.size());
        if (!layer.isNull()) painter.drawImage(target, layer);
      };
      paint_layer(underwear.paths[0], QRect(128, 96, 128, 64));
      paint_layer(underwear.paths[1], QRect(128, 0, 128, 64));
      paint_layer(face.paths[0], QRect(0, 192, 128, 64));
      paint_layer(face.paths[1], QRect(0, 160, 128, 32));
      paint_layer(facial.paths[0], QRect(0, 192, 128, 64));
      paint_layer(facial.paths[1], QRect(0, 160, 128, 32));
      paint_layer(hair.paths[1], QRect(0, 192, 128, 64));
      paint_layer(hair.paths[2], QRect(0, 160, 128, 32));

      // ItemDisplayInfo's eight component textures share the same 256x256
      // character atlas as the skin. Later slots cover earlier clothing.
      static constexpr std::array<QRect, 8> component_regions = {
        QRect(0, 0, 128, 64), QRect(0, 64, 128, 64),
        QRect(0, 128, 128, 32), QRect(128, 0, 128, 64),
        QRect(128, 64, 128, 32), QRect(128, 96, 128, 64),
        QRect(128, 160, 128, 64), QRect(128, 224, 128, 32)};
      static constexpr std::array<char const*, 8> component_folders = {
        "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
        "TorsoUpperTexture", "TorsoLowerTexture", "LegUpperTexture",
        "LegLowerTexture", "FootTexture"};
      if (_item_display_info)
      {
        std::size_t const component_base = _item_display_info->getFieldCount() >= 25 ? 15 : 14;
        if (_item_display_info->getFieldCount() >= component_base + component_regions.size())
          for (unsigned slot : {2u, 5u, 3u, 6u, 4u, 7u, 8u, 9u})
          {
            if (!character_armor_displays[slot]) continue;
            try
            {
              auto const item = _item_display_info->getByID(character_armor_displays[slot]);
              for (std::size_t component = 0; component < component_regions.size(); ++component)
              {
                std::string stem = item.getString(component_base + component);
                if (stem.empty()) continue;
                std::replace(stem.begin(), stem.end(), '/', '\\');
                std::string const prefix = stem.find('\\') == std::string::npos
                  ? std::string("Item\\TextureComponents\\") + component_folders[component] + "\\"
                  : std::string();
                QString const name = QString::fromStdString(stem);
                if (name.endsWith(".blp", Qt::CaseInsensitive))
                  stem.resize(stem.size() - 4);
                std::string chosen;
                for (std::string const& suffix :
                     {sex_id == 0 ? std::string("_M") : std::string("_F"),
                      std::string("_U"), std::string()})
                {
                  std::string const candidate = prefix + stem + suffix + ".blp";
                  if (_client_data->exists(BlizzardArchive::Listfile::FileKey(candidate)))
                  {
                    chosen = candidate;
                    break;
                  }
                }
                if (!chosen.empty()) paint_layer(chosen, component_regions[component]);
              }
            }
            catch (DBCFile::NotFound const&) {}
          }
      }
      painter.end();

      try
      {
        auto const race = _chr_races->getByID(race_id);
        unsigned const base_display_id = race.getUInt(sex_id == 0 ? 4 : 5);
        auto const base_display = _display_info->getByID(base_display_id);
        auto const model = _model_data->getByID(base_display.getUInt(1));
        std::string filename = model.getString(2);
        if (filename.size() >= 4)
        {
          QString extension = QString::fromStdString(filename.substr(filename.size() - 4));
          if (extension.compare(".mdx", Qt::CaseInsensitive) == 0
              || extension.compare(".mdl", Qt::CaseInsensitive) == 0)
            filename.replace(filename.size() - 4, 4, ".m2");
        }
        if (filename.empty() || !_client_data->exists(BlizzardArchive::Listfile::FileKey(filename)))
        {
          display_status->setText("The selected race has no usable character model.");
          return;
        }
        character_base_display = base_display_id;
        if (!dialog.isVisible()) return; // Save may recompose after the dialog closes.
        if (!character_preview->isValid() || !character_preview_files
            || !character_preview_files->isValid())
        {
          display_status->setText("Appearance composed, but the 3D preview is unavailable.");
          return;
        }

        QString const preview_disk_path = character_preview_files->filePath(
          QString("Body_%1.blp").arg(++character_preview_serial));
        QString preview_logical_path = QDir(QString::fromStdString(_client_data->projectPath()))
          .relativeFilePath(preview_disk_path);
        preview_logical_path.replace('/', '\\');
        QString preview_error;
        if (!writeCharacterBlp(composed_character_body, preview_logical_path,
                               _client_data, preview_error))
        {
          display_status->setText("Could not update 3D preview: " + preview_error);
          return;
        }

        if (loaded_character_model != filename)
        {
          character_preview->setModel(filename);
          loaded_character_model = filename;
        }
        bool const body_applied = character_preview->setCreatureTexture(
          1, preview_logical_path.toStdString());
        std::string hair_texture = hair.paths[0];
        if (!hair_texture.empty()
            && !_client_data->exists(BlizzardArchive::Listfile::FileKey(hair_texture)))
        {
          auto const separator = filename.find_last_of("\\/");
          if (separator != std::string::npos)
          {
            std::string race_directory = filename.substr(0, separator + 1);
            auto const parent = race_directory.find_last_of("\\/", race_directory.size() - 2);
            if (parent != std::string::npos) race_directory.resize(parent + 1);
            hair_texture = race_directory + hair_texture;
          }
        }
        if (!hair_texture.empty()
            && _client_data->exists(BlizzardArchive::Listfile::FileKey(hair_texture)))
          character_preview->setCreatureTexture(6, hair_texture);

        std::map<unsigned, unsigned> geosets;
        std::map<unsigned, unsigned> base_facial_variants{{1, 0}, {2, 0}, {3, 0}};
        std::map<unsigned, unsigned> selected_facial_variants;
        bool show_scalp = false;
        if (_hair_geosets)
          for (std::size_t row = 0; row < _hair_geosets->getRecordCount(); ++row)
          {
            auto const hair_style = _hair_geosets->getRecord(row);
            if (hair_style.getUInt(1) == race_id && hair_style.getUInt(2) == sex_id
                && hair_style.getUInt(3) == hair_style_id)
            {
              geosets[0] = hair_style.getUInt(4) % 100;
              show_scalp = _hair_geosets->getFieldCount() > 5
                && hair_style.getUInt(5) != 0;
              break;
            }
          }
        if (_facial_hair_styles && _facial_hair_styles->getFieldCount() >= 6)
        {
          // WotLK's eight-field DBC stores facial geosets at 3-5.
          // Some nine-field clients have padding there and use 6-8.
          std::size_t const first = _facial_hair_styles->getFieldCount() >= 9 ? 6 : 3;
          for (std::size_t row = 0; row < _facial_hair_styles->getRecordCount(); ++row)
          {
            auto const facial_style = _facial_hair_styles->getRecord(row);
            if (facial_style.getUInt(0) != race_id || facial_style.getUInt(1) != sex_id)
              continue;
            unsigned const style = facial_style.getUInt(2);
            if (style != 0 && style != facial_hair_id) continue;
            auto& target = style == 0 ? base_facial_variants : selected_facial_variants;
            unsigned const group1 = facial_style.getUInt(first);
            unsigned const group2 = facial_style.getUInt(first + 2);
            unsigned const group3 = facial_style.getUInt(first + 1);
            if (group1 < 100) target[1] = group1;
            if (group2 < 100) target[2] = group2;
            if (group3 < 100) target[3] = group3;
          }
        }
        for (unsigned group = 1; group < 4; ++group)
          geosets[group] = selected_facial_variants.contains(group)
            ? selected_facial_variants[group] : base_facial_variants[group];
        if (_item_display_info)
        {
          auto apply_armor = [this, &geosets, &character_armor_displays]
            (unsigned slot, unsigned first, unsigned second = 0, unsigned third = 0)
          {
            if (!character_armor_displays[slot]) return;
            try
            {
              auto const item = _item_display_info->getByID(character_armor_displays[slot]);
              if (first) geosets[first] = 1 + item.getUInt(7);
              if (second) geosets[second] = 1 + item.getUInt(8);
              if (third && item.getUInt(9))
                geosets[third] = 1 + item.getUInt(9);
            }
            catch (DBCFile::NotFound const&) {}
          };
          apply_armor(5, 11, 9, 13); // legs
          apply_armor(2, 8, 10, 13); // shirt
          apply_armor(3, 8, 10, 13); // chest
          apply_armor(4, 18);         // belt
          apply_armor(6, 5);          // boots
          apply_armor(8, 4, 23);      // gloves
          apply_armor(9, 12);         // tabard
          apply_armor(10, 15);        // cape
          if (character_armor_displays[6])
          {
            try
            {
              auto const boots = _item_display_info->getByID(character_armor_displays[6]);
              geosets[20] = 2 + boots.getUInt(8);
            }
            catch (DBCFile::NotFound const&) {}
          }
          if (_helmet_geoset_vis && character_armor_displays[0] && race_id < 32 && sex_id < 2)
          {
            try
            {
              auto const helm = _item_display_info->getByID(character_armor_displays[0]);
              unsigned const visibility_id = helm.getUInt(13 + sex_id);
              if (visibility_id)
              {
                auto const visibility = _helmet_geoset_vis->getByID(visibility_id);
                auto hide_selected_if_masked = [&geosets, &visibility]
                  (unsigned group, unsigned mask_field, unsigned default_variant,
                   unsigned replacement_variant)
                {
                  unsigned variant = default_variant;
                  if (auto const selected = geosets.find(group); selected != geosets.end())
                    variant = selected->second;
                  if (variant < 32 && (visibility.getUInt(mask_field) & (1u << variant)))
                    geosets[group] = replacement_variant;
                };
                hide_selected_if_masked(0, 1, 0, 0); // hair
                for (unsigned group = 1; group < 4; ++group)
                  hide_selected_if_masked(group, 1 + group, base_facial_variants[group],
                                          base_facial_variants[group]);
                hide_selected_if_masked(7, 5, 2, 0); // ears
              }
            }
            catch (DBCFile::NotFound const&) {}
          }
        }
        character_preview->setCreatureGeosets(geosets, true, show_scalp);
        if (refresh_character_equipment) refresh_character_equipment();
        display_status->setText(body_applied
          ? "3D character preview updated. Equipment selections are shown live."
          : "The character model loaded, but its body texture could not be shown.");
      }
      catch (std::exception const& exception)
      {
        display_status->setText(QString("Could not preview this character: %1").arg(exception.what()));
      }
    };

    rebuild_all_options();
    if (character_draft.available)
    {
      auto select_value = [](QComboBox* combo, unsigned value)
      {
        int const index = combo->findData(value);
        if (index >= 0) combo->setCurrentIndex(index);
      };
      select_value(character_skin, character_draft.skin);
      rebuild_faces();
      select_value(character_face, character_draft.face);
      select_value(character_hair_style, character_draft.hair_style);
      rebuild_hair_colors();
      select_value(character_hair_color, character_draft.hair_color);
      select_value(character_facial_hair, character_draft.facial_hair);
    }

    auto* preview_timer = new QTimer(&dialog);
    preview_timer->setSingleShot(true);
    preview_timer->setInterval(80);
    auto schedule_preview = [preview_timer] { preview_timer->start(); };
    connect(preview_timer, &QTimer::timeout, &dialog,
            [rebuild_character_preview] { rebuild_character_preview(); });
    connect(character_race, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [=](int) { rebuild_all_options(); schedule_preview(); });
    connect(character_sex, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [=](int) { rebuild_all_options(); schedule_preview(); });
    connect(character_skin, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [=](int) { rebuild_faces(); schedule_preview(); });
    connect(character_hair_style, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [=](int) { rebuild_hair_colors(); schedule_preview(); });
    for (QComboBox* combo : {character_face, character_hair_color, character_facial_hair})
      connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
              [=](int) { schedule_preview(); });
    schedule_preview();
  }
  tabs->addTab(appearance_page, "Appearance");

  auto* equipment_page = new QWidget(tabs);
  auto* equipment_layout = new QVBoxLayout(equipment_page);
  auto* equipment_form = new QFormLayout();
  auto* armor_page = new QWidget(tabs);
  auto* armor_layout = new QVBoxLayout(armor_page);
  auto* armor_scroll = new QScrollArea(armor_page);
  armor_scroll->setWidgetResizable(true);
  auto* armor_contents = new QWidget(armor_scroll);
  auto* armor_form = new QFormLayout(armor_contents);
  armor_scroll->setWidget(armor_contents);
  armor_layout->addWidget(armor_scroll, 1);
  auto make_item_field = [](QWidget* parent, int value)
  {
    auto* field = new QSpinBox(parent);
    field->setRange(0, std::numeric_limits<int>::max());
    field->setSpecialValueText("None");
    field->setValue(value);
    return field;
  };
  auto* main_hand = make_item_field(equipment_page, draft.main_hand);
  auto* off_hand = make_item_field(equipment_page, draft.off_hand);
  auto* ranged = make_item_field(equipment_page, draft.ranged);
  for (QSpinBox* field : {main_hand, off_hand, ranged}) field->hide();

  struct EquipmentItem
  {
    unsigned entry = 0;
    unsigned display = 0;
    unsigned quality = 0;
    unsigned item_level = 0;
    unsigned required_level = 0;
    unsigned inventory_type = 0;
    unsigned subclass = 0;
    QString name;
    QString summary;
    QString details;
    QIcon icon;
  };
  struct EquipmentChoice
  {
    QSpinBox* value = nullptr;
    int armor_slot = -1;
    unsigned display = 0;
    unsigned inventory_type = 0;
    unsigned subclass = 0;
    QWidget* row = nullptr;
    QLabel* icon = nullptr;
    QLabel* text = nullptr;
    QPushButton* browse = nullptr;
    QPushButton* clear = nullptr;
  };
  std::array<EquipmentChoice, 11> armor_choices;

  auto stat_name = [](int type)
  {
    switch (type)
    {
      case 3: return QString("Agility");
      case 4: return QString("Strength");
      case 5: return QString("Intellect");
      case 6: return QString("Spirit");
      case 7: return QString("Stamina");
      case 12: return QString("Defense rating");
      case 13: return QString("Dodge rating");
      case 14: return QString("Parry rating");
      case 15: return QString("Block rating");
      case 31: return QString("Hit rating");
      case 32: return QString("Critical strike rating");
      case 35: return QString("Resilience rating");
      case 36: return QString("Haste rating");
      case 37: return QString("Expertise rating");
      case 38: return QString("Attack power");
      case 39: return QString("Ranged attack power");
      case 43: return QString("Mana per 5 sec");
      case 44: return QString("Armor penetration");
      case 45: return QString("Spell power");
      case 47: return QString("Spell penetration");
      case 48: return QString("Block value");
      default: return QString("Stat %1").arg(type);
    }
  };
  auto quality_color = [](unsigned quality)
  {
    static std::array<QColor, 7> const colors = {
      QColor(157, 157, 157), QColor(255, 255, 255), QColor(30, 255, 0),
      QColor(0, 112, 255), QColor(163, 53, 238), QColor(255, 128, 0),
      QColor(230, 204, 128)};
    return colors[std::min<std::size_t>(quality, colors.size() - 1)];
  };
  auto item_icon = [this](unsigned display_id)
  {
    if (!display_id || !_item_display_info) return QIcon{};
    try
    {
      auto const display_record = _item_display_info->getByID(display_id);
      std::string icon_path = display_record.getString(5);
      if (icon_path.empty()) return QIcon{};
      std::replace(icon_path.begin(), icon_path.end(), '/', '\\');
      if (icon_path.find('\\') == std::string::npos)
        icon_path = "Interface\\Icons\\" + icon_path;
      if (!QString::fromStdString(icon_path).endsWith(".blp", Qt::CaseInsensitive))
        icon_path += ".blp";
      if (!_client_data->exists(BlizzardArchive::Listfile::FileKey(icon_path))) return QIcon{};
      QImage const image = characterTextureImage(icon_path, QSize(64, 64));
      return image.isNull() ? QIcon{} : QIcon(QPixmap::fromImage(image));
    }
    catch (...) { return QIcon{}; }
  };
  auto read_item = [stat_name, item_icon](QSqlQuery const& query)
  {
    EquipmentItem item;
    item.entry = field(query, "entry").toUInt();
    item.display = field(query, "displayid").toUInt();
    item.quality = field(query, "Quality").toUInt();
    item.item_level = field(query, "ItemLevel").toUInt();
    item.required_level = field(query, "RequiredLevel").toUInt();
    item.inventory_type = field(query, "InventoryType").toUInt();
    item.subclass = field(query, "subclass").toUInt();
    item.name = field(query, "name").toString().trimmed();
    if (item.name.isEmpty()) item.name = QString("Item %1").arg(item.entry);

    QStringList short_stats;
    QStringList all_stats;
    double const min_damage = field(query, "dmg_min1").toDouble();
    double const max_damage = field(query, "dmg_max1").toDouble();
    unsigned const delay = field(query, "delay").toUInt();
    if (max_damage > 0.0)
    {
      QString damage = QString("%1-%2 damage").arg(min_damage, 0, 'f', 0)
        .arg(max_damage, 0, 'f', 0);
      if (delay) damage += QString(" · %1 speed").arg(delay / 1000.0, 0, 'f', 2);
      short_stats << damage;
      all_stats << damage;
    }
    for (int index = 1; index <= 10; ++index)
    {
      int const type = field(query, QString("stat_type%1").arg(index)).toInt();
      int const value = field(query, QString("stat_value%1").arg(index)).toInt();
      if (!type || !value) continue;
      QString const stat = QString("%1%2 %3").arg(value > 0 ? "+" : "")
        .arg(value).arg(stat_name(type));
      all_stats << stat;
      if (short_stats.size() < 3) short_stats << stat;
    }
    QString level = QString("Item level %1").arg(item.item_level);
    if (item.required_level) level += QString(" · Requires %1").arg(item.required_level);
    item.summary = short_stats.isEmpty() ? level : level + "\n" + short_stats.join(" · ");
    item.details = QString("%1\nEntry %2 · %3").arg(item.name).arg(item.entry).arg(level);
    if (!all_stats.isEmpty()) item.details += "\n" + all_stats.join("\n");
    item.icon = item_icon(item.display);
    return item;
  };

  auto make_equipment_choice = [](QWidget* parent, QSpinBox* value)
  {
    EquipmentChoice choice;
    choice.value = value;
    choice.row = new QWidget(parent);
    auto* layout = new QHBoxLayout(choice.row);
    layout->setContentsMargins(0, 0, 0, 0);
    choice.icon = new QLabel(choice.row);
    choice.icon->setFixedSize(52, 52);
    choice.icon->setAlignment(Qt::AlignCenter);
    choice.icon->setStyleSheet("background: #191b1e; border: 1px solid palette(mid);");
    choice.text = new QLabel(choice.row);
    choice.text->setWordWrap(true);
    choice.text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    choice.browse = new QPushButton("Browse…", choice.row);
    choice.clear = new QPushButton("Clear", choice.row);
    layout->addWidget(choice.icon);
    layout->addWidget(choice.text, 1);
    layout->addWidget(choice.browse);
    layout->addWidget(choice.clear);
    return choice;
  };
  EquipmentChoice main_choice = make_equipment_choice(equipment_page, main_hand);
  EquipmentChoice off_choice = make_equipment_choice(equipment_page, off_hand);
  EquipmentChoice ranged_choice = make_equipment_choice(equipment_page, ranged);
  equipment_form->addRow("Main hand", main_choice.row);
  equipment_form->addRow("Off hand", off_choice.row);
  equipment_form->addRow("Ranged", ranged_choice.row);
  static constexpr std::array<char const*, 11> armor_names = {
    "Head", "Shoulders", "Shirt", "Chest / robe", "Waist", "Legs",
    "Feet", "Wrists", "Hands", "Tabard", "Back / cape"};
  for (std::size_t slot = 0; slot < armor_choices.size(); ++slot)
  {
    auto* value = make_item_field(armor_contents, 0);
    value->hide();
    armor_choices[slot] = make_equipment_choice(armor_contents, value);
    armor_choices[slot].armor_slot = static_cast<int>(slot);
    armor_form->addRow(armor_names[slot], armor_choices[slot].row);
  }

  auto* equipment_preview_status = new QLabel(equipment_page);
  equipment_preview_status->setWordWrap(true);
  auto apply_weapon_choice = [this](Tools::AssetBrowser::ModelViewer* preview,
                                    EquipmentChoice const& choice,
                                    unsigned attachment_id)
  {
    if (!preview || !choice.value->value()) return true;
    return setWeaponAttachment(preview, _item_display_info.get(), _client_data,
                               choice.display, choice.inventory_type, attachment_id,
                               attachment_id == 12
                                 ? rangedHandAttachment(choice.value->value(), choice.subclass)
                                 : 0);
  };
  refresh_character_equipment = [this, &dialog, character_preview, &loaded_character_model,
                                 &main_choice, &off_choice, &ranged_choice, &armor_choices,
                                 character_race, character_sex,
                                 equipment_preview_status, apply_weapon_choice,
                                 &refresh_selected_spawn_preview]
  {
    if (!character_preview || !dialog.isVisible() || !character_preview->isValid()
        || loaded_character_model.empty())
      return;

    character_preview->clearCreatureAttachments();
    if (!_item_display_info) return;

    int unavailable = 0;
    for (auto const& [choice, attachment_id] :
         {std::pair{&main_choice, 1u}, std::pair{&off_choice, 2u},
          std::pair{&ranged_choice, 12u}})
    {
      if (!apply_weapon_choice(character_preview, *choice, attachment_id))
        ++unavailable;
    }
    auto attach_armor = [this, character_preview, &unavailable]
      (unsigned display_id, unsigned model_field, unsigned texture_field,
       unsigned attachment_id, std::string model_path, std::string const& texture_folder)
    {
      if (!display_id) return;
      try
      {
        auto const item = _item_display_info->getByID(display_id);
        if (model_path.empty()) model_path = item.getString(model_field);
        std::replace(model_path.begin(), model_path.end(), '/', '\\');
        if (model_path.empty()) return;
        QString const model_name = QString::fromStdString(model_path);
        if (model_name.endsWith(".mdx", Qt::CaseInsensitive)
            || model_name.endsWith(".mdl", Qt::CaseInsensitive)
            || model_name.endsWith(".m2", Qt::CaseInsensitive))
          model_path.resize(model_path.find_last_of('.'));
        if (model_path.find('\\') == std::string::npos)
          model_path = texture_folder + model_path;
        model_path += ".m2";
        if (!_client_data->exists(BlizzardArchive::Listfile::FileKey(model_path)))
        {
          ++unavailable;
          return;
        }
        std::string texture = item.getString(texture_field);
        std::replace(texture.begin(), texture.end(), '/', '\\');
        if (!texture.empty())
        {
          if (texture.find('\\') == std::string::npos)
            texture = texture_folder + texture;
          if (!QString::fromStdString(texture).endsWith(".blp", Qt::CaseInsensitive))
            texture += ".blp";
          if (!_client_data->exists(BlizzardArchive::Listfile::FileKey(texture)))
            texture.clear();
        }
        if (!character_preview->setCreatureAttachment(attachment_id, model_path, texture))
          ++unavailable;
      }
      catch (...) { ++unavailable; }
    };
    if (character_race && character_sex && _chr_races && armor_choices[0].display)
    {
      try
      {
        auto const race = _chr_races->getByID(character_race->currentData().toUInt());
        auto const helm = _item_display_info->getByID(armor_choices[0].display);
        std::string model = helm.getString(1);
        QString const model_name = QString::fromStdString(model);
        if (model_name.endsWith(".mdx", Qt::CaseInsensitive)
            || model_name.endsWith(".mdl", Qt::CaseInsensitive)
            || model_name.endsWith(".m2", Qt::CaseInsensitive))
          model.resize(model.find_last_of('.'));
        if (!model.empty() && model.find_first_of("\\/") == std::string::npos)
          model += "_" + std::string(race.getString(6))
            + (character_sex->currentData().toUInt() == 0 ? "M" : "F");
        attach_armor(armor_choices[0].display, 1, 3, 11, model,
                     "Item\\ObjectComponents\\Head\\");
      }
      catch (...) { ++unavailable; }
    }
    if (armor_choices[1].display)
    {
      attach_armor(armor_choices[1].display, 1, 3, 6, {},
                   "Item\\ObjectComponents\\Shoulder\\");
      attach_armor(armor_choices[1].display, 2, 4, 5, {},
                   "Item\\ObjectComponents\\Shoulder\\");
    }
    if (armor_choices[10].display)
    {
      try
      {
        auto const cape = _item_display_info->getByID(armor_choices[10].display);
        std::string stem = cape.getString(3);
        std::replace(stem.begin(), stem.end(), '/', '\\');
        if (!stem.empty())
        {
          if (QString::fromStdString(stem).endsWith(".blp", Qt::CaseInsensitive))
            stem.resize(stem.size() - 4);
          bool applied = false;
          for (std::string const& folder :
               {std::string("Item\\ObjectComponents\\Cape\\"),
                std::string("Item\\TextureComponents\\Cape\\")})
          {
            for (std::string const& suffix :
                 {character_sex && character_sex->currentData().toUInt() == 0
                    ? std::string("_M") : std::string("_F"),
                  std::string("_U"), std::string()})
            {
              std::string const path = (stem.find('\\') == std::string::npos ? folder : "")
                + stem + suffix + ".blp";
              if (_client_data->exists(BlizzardArchive::Listfile::FileKey(path))
                  && character_preview->setCreatureTexture(2, path))
              {
                applied = true;
                break;
              }
            }
            if (applied) break;
          }
        }
      }
      catch (...) {}
    }
    equipment_preview_status->setText(unavailable
      ? QString("%1 selected item(s) have no usable client model or attachment in this preview.")
          .arg(unavailable)
      : "Equipment changes appear in the 3D preview immediately.");
    if (refresh_selected_spawn_preview) refresh_selected_spawn_preview();
  };

  refresh_selected_spawn_preview = [this, selected_spawn, source_entry,
                                     character_appearance_editor, &dialog,
                                     character_preview, display, &main_choice,
                                     &off_choice, &ranged_choice, apply_weapon_choice]
  {
    if (!selected_spawn || !dialog.isVisible()
        || !_selected_spawn_guid || !_map_view || !_map_view->getWorld()) return;
    std::optional<Noggit::NpcAppearance> appearance;
    if (character_appearance_editor && character_preview)
      appearance = character_preview->creatureAppearance();
    else
    {
      if (!loadDisplay(static_cast<unsigned>(display->value()), false)) return;
      for (auto const& [choice, attachment_id] :
           {std::pair{&main_choice, 1u}, std::pair{&off_choice, 2u},
            std::pair{&ranged_choice, 12u}})
        apply_weapon_choice(_preview, *choice, attachment_id);
      appearance = _preview->creatureAppearance();
    }
    if (!appearance) return;
    World* world = _map_view->getWorld();
    auto* old = world->findNpcSpawnOverlay(*_selected_spawn_guid);
    if (!old) return;
    glm::vec3 const position = old->anchorPosition();
    float const yaw = old->anchorYaw();
    float const scale = old->scale();
    try
    {
      world->addNpcSpawnOverlay(*_selected_spawn_guid, source_entry, position, yaw,
                                scale, *appearance);
      world->selectNpcSpawnOverlay(*_selected_spawn_guid);
      updateMovementPreview();
      _map_view->invalidate();
      _map_view->update();
    }
    catch (...) {}
  };

  auto show_choice = [&dialog, character_appearance_editor,
                      &refresh_character_equipment, &refresh_selected_spawn_preview,
                      &rebuild_character_preview, &character_armor_displays](EquipmentChoice* choice,
                                                 EquipmentItem const& item)
  {
    choice->display = item.display;
    choice->inventory_type = item.inventory_type;
    choice->subclass = item.subclass;
    choice->value->setValue(static_cast<int>(item.entry));
    if (choice->armor_slot >= 0)
      character_armor_displays[choice->armor_slot] = item.display;
    auto update_preview = [&]
    {
      if (!dialog.isVisible()) return;
      if (choice->armor_slot >= 0 && rebuild_character_preview)
        rebuild_character_preview();
      else if (character_appearance_editor && refresh_character_equipment)
        refresh_character_equipment();
      else if (refresh_selected_spawn_preview)
        refresh_selected_spawn_preview();
    };
    if (!item.entry && !item.display)
    {
      choice->icon->clear();
      choice->icon->setText("None");
      choice->text->setText("No item equipped");
      choice->clear->setEnabled(false);
      update_preview();
      return;
    }
    choice->icon->setText({});
    choice->icon->setPixmap(item.icon.pixmap(48, 48));
    choice->text->setText(QString("<b>%1</b><br><span style='color: palette(mid);'>%2</span>")
                            .arg(item.name.toHtmlEscaped(),
                                 item.summary.toHtmlEscaped().replace("\n", "<br>")));
    choice->clear->setEnabled(true);
    update_preview();
  };
  EquipmentItem no_item;
  for (EquipmentChoice* choice : {&main_choice, &off_choice, &ranged_choice})
  {
    EquipmentItem initial;
    initial.entry = static_cast<unsigned>(choice->value->value());
    initial.name = initial.entry ? QString("Item %1").arg(initial.entry) : QString();
    initial.summary = initial.entry ? "Loading item details…" : QString();
    show_choice(choice, initial);
  }
  if (character_appearance_editor)
    for (std::size_t slot = 0; slot < armor_choices.size(); ++slot)
    {
      EquipmentItem initial;
      initial.display = character_armor_displays[slot];
      initial.name = initial.display
        ? QString("Armor visual %1").arg(initial.display) : QString();
      initial.summary = initial.display
        ? "Loading the equipped item details…" : QString();
      initial.icon = item_icon(initial.display);
      show_choice(&armor_choices[slot], initial);
    }

  auto open_equipment_browser = [this, &dialog, read_item, quality_color, show_choice]
                                (EquipmentChoice* choice, int slot, QString const& title)
  {
    QDialog picker(&dialog);
    picker.setWindowTitle(title);
    picker.resize(760, 620);
    auto* picker_layout = new QVBoxLayout(&picker);
    auto* search_row = new QHBoxLayout();
    auto* search = new QLineEdit(&picker);
    search->setPlaceholderText("Search by item name or entry ID…");
    auto* search_button = new QPushButton("Search", &picker);
    search_row->addWidget(search, 1);
    search_row->addWidget(search_button);
    picker_layout->addLayout(search_row);

    auto* items = new QListWidget(&picker);
    items->setViewMode(QListView::ListMode);
    items->setIconSize(QSize(64, 64));
    items->setSpacing(3);
    items->setSelectionMode(QAbstractItemView::SingleSelection);
    items->setWordWrap(true);
    picker_layout->addWidget(items, 1);
    auto* details = new QLabel("Select an item to see its complete stats.", &picker);
    details->setMinimumHeight(92);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setStyleSheet("background: #191b1e; padding: 8px;");
    picker_layout->addWidget(details);

    auto* navigation = new QHBoxLayout();
    auto* previous = new QPushButton("Previous", &picker);
    auto* page_label = new QLabel(&picker);
    auto* next = new QPushButton("Next", &picker);
    navigation->addWidget(previous);
    navigation->addWidget(page_label);
    navigation->addStretch();
    navigation->addWidget(next);
    picker_layout->addLayout(navigation);
    auto* picker_buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &picker);
    picker_buttons->button(QDialogButtonBox::Ok)->setText("Equip selected");
    picker_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    picker_layout->addWidget(picker_buttons);

    int page = 0;
    constexpr int page_size = 60;
    std::function<void()> load_page;
    load_page = [&, slot]
    {
      items->clear();
      details->setText("Loading items…");
      picker_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
      QString failure;
      bool has_next = false;
      bool const loaded = useWorldDatabase(
        _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
        false, [&](QSqlDatabase& db, QString& query_error)
        {
          if (!tableExists(db, "item_template"))
          {
            query_error = "This world database has no item_template table.";
            return false;
          }
          QString slot_filter;
          if (slot == 0)
            slot_filter = "(`class` = 2 AND `InventoryType` IN (13, 17, 21))";
          else if (slot == 1)
            slot_filter = "((`class` = 2 AND `InventoryType` IN (13, 22)) "
                          "OR `InventoryType` IN (14, 23))";
          else if (slot == 2)
            slot_filter = "(`class` = 2 AND `InventoryType` IN (15, 25, 26))";
          else
          {
            static constexpr std::array<char const*, 11> armor_inventory = {
              "1", "3", "4", "5, 20", "6", "7", "8", "9", "10", "19", "16"};
            slot_filter = QString("`InventoryType` IN (%1)")
              .arg(armor_inventory[static_cast<std::size_t>(slot - 3)]);
          }

          QString const entered = search->text().trimmed();
          bool numeric = false;
          unsigned const requested_entry = entered.toUInt(&numeric);
          QString sql = "SELECT * FROM `item_template` WHERE " + slot_filter;
          if (numeric && requested_entry)
            sql += " AND `entry` = ?";
          else if (!entered.isEmpty())
            sql += " AND `name` LIKE ?";
          sql += QString(" ORDER BY `Quality` DESC, `ItemLevel` DESC, `entry` "
                         "LIMIT %1 OFFSET %2")
                   .arg(page_size + 1).arg(page * page_size);
          QSqlQuery query(db);
          query.prepare(sql);
          if (numeric && requested_entry) query.addBindValue(requested_entry);
          else if (!entered.isEmpty()) query.addBindValue("%" + entered + "%");
          if (!query.exec())
          {
            query_error = query.lastError().text();
            return false;
          }
          int result_count = 0;
          while (query.next())
          {
            if (result_count++ == page_size)
            {
              has_next = true;
              break;
            }
            EquipmentItem const item = read_item(query);
            auto* row = new QListWidgetItem(item.icon,
              QString("%1\n%2").arg(item.name, item.summary), items);
            row->setData(Qt::UserRole, item.entry);
            row->setData(Qt::UserRole + 1, item.name);
            row->setData(Qt::UserRole + 2, item.summary);
            row->setData(Qt::UserRole + 3, item.details);
            row->setData(Qt::UserRole + 4, item.display);
            row->setData(Qt::UserRole + 5, item.inventory_type);
            row->setData(Qt::UserRole + 6, item.subclass);
            row->setForeground(quality_color(item.quality));
            row->setSizeHint(QSize(0, 82));
            if (item.entry == static_cast<unsigned>(choice->value->value()))
              items->setCurrentItem(row);
          }
          return true;
        }, failure);
      if (!loaded)
      {
        details->setText("Could not load equipment: " + failure);
        previous->setEnabled(false);
        next->setEnabled(false);
        return;
      }
      previous->setEnabled(page > 0);
      next->setEnabled(has_next);
      page_label->setText(QString("Page %1 · %2 item(s)").arg(page + 1).arg(items->count()));
      if (!items->count()) details->setText("No compatible items matched this search.");
      else if (!items->currentItem()) details->setText("Select an item to see its complete stats.");
    };

    connect(search_button, &QPushButton::clicked, &picker, [&] { page = 0; load_page(); });
    connect(search, &QLineEdit::returnPressed, &picker, [&] { page = 0; load_page(); });
    connect(previous, &QPushButton::clicked, &picker, [&] { --page; load_page(); });
    connect(next, &QPushButton::clicked, &picker, [&] { ++page; load_page(); });
    connect(items, &QListWidget::currentItemChanged, &picker,
            [details, picker_buttons](QListWidgetItem* current)
    {
      details->setText(current ? current->data(Qt::UserRole + 3).toString()
                               : "Select an item to see its complete stats.");
      picker_buttons->button(QDialogButtonBox::Ok)->setEnabled(current != nullptr);
    });
    connect(items, &QListWidget::itemDoubleClicked, &picker,
            [&picker](QListWidgetItem*) { picker.accept(); });
    connect(picker_buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(picker_buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    load_page();
    if (picker.exec() != QDialog::Accepted || !items->currentItem()) return;

    QListWidgetItem const* selected = items->currentItem();
    EquipmentItem result;
    result.entry = selected->data(Qt::UserRole).toUInt();
    result.name = selected->data(Qt::UserRole + 1).toString();
    result.summary = selected->data(Qt::UserRole + 2).toString();
    result.details = selected->data(Qt::UserRole + 3).toString();
    result.display = selected->data(Qt::UserRole + 4).toUInt();
    result.inventory_type = selected->data(Qt::UserRole + 5).toUInt();
    result.subclass = selected->data(Qt::UserRole + 6).toUInt();
    result.icon = selected->icon();
    show_choice(choice, result);
  };

  connect(main_choice.browse, &QPushButton::clicked, &dialog,
          [&] { open_equipment_browser(&main_choice, 0, "Choose Main-hand Item"); });
  connect(off_choice.browse, &QPushButton::clicked, &dialog,
          [&] { open_equipment_browser(&off_choice, 1, "Choose Off-hand Item"); });
  connect(ranged_choice.browse, &QPushButton::clicked, &dialog,
          [&] { open_equipment_browser(&ranged_choice, 2, "Choose Ranged Item"); });
  if (character_appearance_editor)
    for (std::size_t slot = 0; slot < armor_choices.size(); ++slot)
    {
      EquipmentChoice* choice = &armor_choices[slot];
      connect(choice->browse, &QPushButton::clicked, &dialog,
              [&, choice, slot]
      {
        open_equipment_browser(choice, static_cast<int>(slot + 3),
          QString("Choose %1 Item").arg(armor_names[slot]));
      });
      connect(choice->clear, &QPushButton::clicked, &dialog,
              [&, choice] { show_choice(choice, no_item); });
    }
  connect(main_choice.clear, &QPushButton::clicked, &dialog,
          [&] { show_choice(&main_choice, no_item); });
  connect(off_choice.clear, &QPushButton::clicked, &dialog,
          [&] { show_choice(&off_choice, no_item); });
  connect(ranged_choice.clear, &QPushButton::clicked, &dialog,
          [&] { show_choice(&ranged_choice, no_item); });

  QString equipment_load_error;
  useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(), false,
    [&](QSqlDatabase& db, QString& query_error)
    {
      std::map<unsigned, EquipmentChoice*> requested;
      for (EquipmentChoice* choice : {&main_choice, &off_choice, &ranged_choice})
        if (choice->value->value())
          requested.emplace(static_cast<unsigned>(choice->value->value()), choice);
      if (!requested.empty())
      {
        QStringList ids;
        for (auto const& [entry, choice] : requested) ids << QString::number(entry);
        QSqlQuery query(db);
        if (!query.exec("SELECT * FROM `item_template` WHERE `entry` IN (" + ids.join(',') + ")"))
        {
          query_error = query.lastError().text();
          return false;
        }
        while (query.next())
        {
          EquipmentItem const item = read_item(query);
          if (auto found = requested.find(item.entry); found != requested.end())
            show_choice(found->second, item);
        }
      }

      // Armor stored in CreatureDisplayInfoExtra is a client display ID, not
      // a world item entry. Resolve each display back to a representative item
      // so existing Blizzard and Noggit NPCs open with named, editable gear.
      std::map<unsigned, std::vector<EquipmentChoice*>> armor_by_display;
      if (character_appearance_editor)
        for (EquipmentChoice& choice : armor_choices)
          if (choice.display)
            armor_by_display[choice.display].push_back(&choice);
      if (!armor_by_display.empty())
      {
        QStringList displays;
        for (auto const& [display_id, choices] : armor_by_display)
          displays << QString::number(display_id);
        QSqlQuery armor(db);
        if (!armor.exec("SELECT * FROM `item_template` WHERE `displayid` IN ("
                        + displays.join(',') + ") ORDER BY `entry`"))
        {
          query_error = armor.lastError().text();
          return false;
        }
        std::set<unsigned> resolved_displays;
        while (armor.next())
        {
          EquipmentItem const item = read_item(armor);
          if (!item.display || resolved_displays.contains(item.display)) continue;
          auto const found = armor_by_display.find(item.display);
          if (found == armor_by_display.end()) continue;
          for (EquipmentChoice* choice : found->second) show_choice(choice, item);
          resolved_displays.insert(item.display);
        }
      }
      return true;
    }, equipment_load_error);
  for (EquipmentChoice* choice : {&main_choice, &off_choice, &ranged_choice})
    if (choice->value->value() && choice->text->text().contains("Loading item details"))
    {
      EquipmentItem missing;
      missing.entry = static_cast<unsigned>(choice->value->value());
      auto const [display_id, inventory_type] = itemVisual(missing.entry);
      missing.display = display_id;
      missing.inventory_type = inventory_type;
      missing.icon = item_icon(display_id);
      missing.name = QString("NPC-only item %1").arg(missing.entry);
      missing.summary = display_id
        ? QString("Client display %1 · no item_template row").arg(display_id)
        : "Item details are unavailable in item_template and Item.dbc.";
      show_choice(choice, missing);
    }
  if (character_appearance_editor)
    for (EquipmentChoice& choice : armor_choices)
      if (choice.display && !choice.value->value())
      {
        EquipmentItem visual;
        visual.display = choice.display;
        visual.name = QString("Armor visual %1").arg(visual.display);
        visual.summary = "This client armor visual has no matching named item in item_template.";
        visual.icon = item_icon(visual.display);
        show_choice(&choice, visual);
      }

  equipment_layout->addLayout(equipment_form);
  auto* drawn_weapon = new QComboBox(equipment_page);
  drawn_weapon->addItem("Use server / template default", -1);
  drawn_weapon->addItem("Melee drawn (main-hand / off-hand)", 1);
  drawn_weapon->addItem("Ranged drawn (bow / gun / crossbow)", 2);
  if (selected_spawn && _edit_weapon_stance)
    drawn_weapon->setCurrentIndex(std::max(0,
      drawn_weapon->findData(_edit_weapon_stance->currentData().toInt())));
  auto* drawn_weapon_form = new QFormLayout();
  drawn_weapon_form->addRow("In-game drawn weapon", drawn_weapon);
  equipment_layout->addLayout(drawn_weapon_form);
  if (character_appearance_editor) equipment_layout->addWidget(equipment_preview_status);
  auto* equipment_note = new QLabel(
    "Browse searches compatible world-database items and shows their client icon and stats. "
    "Selections preview immediately and are written as equipment set 1 for the world server "
    "to load after its creature data is reloaded.", equipment_page);
  equipment_note->setWordWrap(true);
  equipment_layout->addWidget(equipment_note);
  equipment_layout->addStretch();
  tabs->addTab(equipment_page, "Equipment");
  if (character_appearance_editor)
  {
    auto* armor_note = new QLabel(
      "Armor is saved as client display IDs, with its textures baked into the new NPC's "
      "body. Helmets and shoulders are separate models. The world database's equipment "
      "set only stores the three weapon slots.", armor_page);
    armor_note->setWordWrap(true);
    armor_layout->addWidget(armor_note);
    tabs->addTab(armor_page, "Armor");
  }

  auto* behavior_page = new QWidget(tabs);
  auto* behavior_form = new QFormLayout(behavior_page);
  auto* npc_flags = new QSpinBox(behavior_page);
  auto* creature_type = new QSpinBox(behavior_page);
  auto* rank = new QSpinBox(behavior_page);
  npc_flags->setRange(0, std::numeric_limits<int>::max());
  creature_type->setRange(0, 13);
  rank->setRange(0, 4);
  npc_flags->setValue(std::max(0, draft.npc_flags));
  creature_type->setValue(std::clamp(draft.type, 0, 13));
  rank->setValue(std::clamp(draft.rank, 0, 4));
  behavior_form->addRow("NPC role flags", npc_flags);
  behavior_form->addRow("Creature type", creature_type);
  behavior_form->addRow("Rank", rank);
  tabs->addTab(behavior_page, "Behavior");

  auto* sound_sections = new QTabWidget(tabs);
  auto* sound_editor = new NpcSoundEditor(_display_info.get(), _model_data.get(),
    _npc_sounds.get(), _creature_sound_data.get(), _footstep_terrain_lookup.get(),
    sound_sections);
  sound_editor->setDisplay(static_cast<unsigned>(std::max(0, draft.display)));
  sound_sections->addTab(sound_editor, "Display actions");
  auto* dialogue_sound_editor = new NpcDialogueSoundEditor(
    std::move(dialogue_sound_rows), std::move(dialogue_sound_context), sound_sections);
  sound_sections->addTab(dialogue_sound_editor, "Scripted dialogue");
  int const sound_tab_index = tabs->addTab(sound_sections, "Sounds");
  if (!_npc_sounds || !_creature_sound_data)
  {
    tabs->setTabToolTip(sound_tab_index,
      "NPCSounds.dbc or CreatureSoundData.dbc is unavailable in the configured client files.");
  }
  connect(tabs, &QTabWidget::currentChanged, &dialog,
          [=, &character_base_display, &rebuild_character_preview](int index)
  {
    if (index != sound_tab_index || sound_editor->voiceChanged()
        || sound_editor->actionChanged()) return;
    if (character_appearance_editor && rebuild_character_preview)
      rebuild_character_preview();
    unsigned const source_display = character_appearance_editor && character_base_display
      ? character_base_display : static_cast<unsigned>(display->value());
    sound_editor->setDisplay(source_display);
  });
  if (!character_appearance_editor)
    connect(display, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
            [sound_editor, display, &dialog](int id)
    {
      if ((sound_editor->voiceChanged() || sound_editor->actionChanged())
          && QMessageBox::question(&dialog, "Change sound source display",
               "Changing the display reloads its sound assignments and discards the "
               "sound changes staged in this dialog. Continue?") != QMessageBox::Yes)
      {
        QSignalBlocker const blocker(display);
        display->setValue(static_cast<int>(sound_editor->displayId()));
        return;
      }
      sound_editor->setDisplay(static_cast<unsigned>(id));
    });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                       Qt::Horizontal, &dialog);
  buttons->button(QDialogButtonBox::Save)->setText(
    selected_spawn ? "Create unique NPC" : "Save and place");
  QPushButton* shared_dialogue_button = nullptr;
  if (selected_spawn)
  {
    shared_dialogue_button = buttons->addButton("Save dialogue to template",
                                                 QDialogButtonBox::ActionRole);
    shared_dialogue_button->setToolTip(
      QString("Save only Scripted dialogue changes to template %1. Lines are shared "
              "by its spawns; triggers follow their chosen scope. Other tabs are not saved.")
        .arg(source_entry));
  }
  root->addWidget(buttons);
  bool shared_dialogue_saved = false;
  if (shared_dialogue_button)
    connect(shared_dialogue_button, &QPushButton::clicked, &dialog,
            [this, &dialog, &shared_dialogue_saved, dialogue_sound_editor,
             source_entry, selected_spawn_guid = dialogue_sound_context.selected_spawn_guid]
    {
      QString validation_error;
      if (!dialogue_sound_editor->changed())
      {
        QMessageBox::information(&dialog, "No scripted dialogue changes",
          "Add a dialogue line, trigger, or sound change before saving this template.");
        return;
      }
      if (!dialogue_sound_editor->validate(validation_error))
      {
        QMessageBox::warning(&dialog, "Dialogue cannot be saved", validation_error);
        return;
      }
      QString save_error;
      bool const saved = useWorldDatabase(
        _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
        true, [&](QSqlDatabase& db, QString& failure)
        {
          return saveDialogueToSharedTemplate(db, source_entry, selected_spawn_guid,
            dialogue_sound_editor->newDialogueLines(),
            dialogue_sound_editor->dialogueLineUpdates(), dialogue_sound_editor->changes(),
            dialogue_sound_editor->triggerChanges(), dialogue_sound_editor->facingChanges(),
            _database->text(), failure);
        }, save_error);
      if (!saved)
      {
        QMessageBox::critical(&dialog, "Scripted dialogue was not saved", save_error);
        return;
      }
      shared_dialogue_saved = true;
      _status->setText(QString("Saved scripted dialogue to template %1. Its spawns share "
                               "the lines; triggers follow their chosen scope. Reload the "
                               "world server data to see it in game.").arg(source_entry));
      dialog.accept();
    });

  std::optional<Noggit::NpcAppearance> original_spawn_appearance;
  glm::vec3 original_spawn_position{};
  float original_spawn_yaw = 0.0f;
  float original_spawn_scale = 1.0f;
  if (selected_spawn && _selected_spawn_guid && _map_view && _map_view->getWorld())
  {
    if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
    {
      original_spawn_position = overlay->anchorPosition();
      original_spawn_yaw = overlay->anchorYaw();
      original_spawn_scale = overlay->scale();
    }
    if (loadDisplay(static_cast<unsigned>(draft.display), false))
    {
      for (auto const& [choice, attachment_id] :
           {std::pair{&main_choice, 1u}, std::pair{&off_choice, 2u},
            std::pair{&ranged_choice, 12u}})
        apply_weapon_choice(_preview, *choice, attachment_id);
      original_spawn_appearance = _preview->creatureAppearance();
    }
  }
  if (selected_spawn && !character_appearance_editor)
    connect(display, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
            [&refresh_selected_spawn_preview](int)
    {
      if (refresh_selected_spawn_preview) refresh_selected_spawn_preview();
    });

  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dialog,
          [&dialog, name, min_level, max_level, display, drawn_weapon, ranged,
           sound_editor, dialogue_sound_editor, this]
  {
    if (name->text().trimmed().isEmpty())
    {
      QMessageBox::warning(&dialog, "NPC name required", "Enter a name for the custom NPC.");
      return;
    }
    if (min_level->value() > max_level->value())
    {
      QMessageBox::warning(&dialog, "Invalid level range",
                           "Minimum level cannot be higher than maximum level.");
      return;
    }
    if (display->value() <= 0)
    {
      QMessageBox::warning(&dialog, "Display required", "Choose a creature display ID.");
      return;
    }
    if (drawn_weapon->currentData().toInt() == 2 && ranged->value() <= 0)
    {
      QMessageBox::warning(&dialog, "Ranged weapon required",
        "Choose a ranged item before setting this NPC to draw it in game.");
      return;
    }
    if ((sound_editor->voiceChanged() && !_npc_sounds)
        || (sound_editor->actionChanged() && !_creature_sound_data)
        || ((sound_editor->voiceChanged() || sound_editor->actionChanged())
            && (!_display_info || !_model_data)))
    {
      QMessageBox::warning(&dialog, "Sound DBC unavailable",
        "The configured client is missing a DBC required for the chosen sound changes.");
      return;
    }
    auto valid_sound = [](unsigned id) { return !id || gSoundEntriesDB.CheckIfIdExists(id); };
    for (unsigned slot = 0; slot < 4; ++slot)
      if (sound_editor->voiceSlotOverridden(slot)
          && !valid_sound(sound_editor->voiceSound(slot)))
      {
        QMessageBox::warning(&dialog, "Sound entry missing",
          QString("Voice slot %1 references a missing SoundEntries row.").arg(slot + 1));
        return;
      }
    for (unsigned field = 1; field < 38; ++field)
      if (field != 9 && field != 23 && field != 25 && field != 31
          && field != 32 && field != 37 && sound_editor->actionSlotOverridden(field)
          && !valid_sound(sound_editor->actionSound(field)))
      {
        QMessageBox::warning(&dialog, "Sound entry missing",
          QString("An action references missing SoundEntries row %1.")
            .arg(sound_editor->actionSound(field)));
        return;
      }
    QString dialogue_sound_error;
    if (!dialogue_sound_editor->validate(dialogue_sound_error))
    {
      QMessageBox::warning(&dialog, "Dialogue sound entry missing", dialogue_sound_error);
      return;
    }
    dialog.accept();
  });
  int const dialog_result = dialog.exec();
  if (dialog_result != QDialog::Accepted || shared_dialogue_saved)
  {
    if (selected_spawn && original_spawn_appearance && _selected_spawn_guid
        && _map_view && _map_view->getWorld())
    {
      try
      {
        _map_view->getWorld()->addNpcSpawnOverlay(*_selected_spawn_guid, source_entry,
          original_spawn_position, original_spawn_yaw, original_spawn_scale,
          *original_spawn_appearance);
        _map_view->getWorld()->selectNpcSpawnOverlay(*_selected_spawn_guid);
        updateMovementPreview();
        _map_view->invalidate();
        _map_view->update();
      }
      catch (...) {}
    }
    return;
  }

  std::unique_ptr<DBCFile> previous_display_info;
  std::unique_ptr<DBCFile> previous_display_extra;
  std::unique_ptr<DBCFile> previous_npc_sounds;
  std::unique_ptr<DBCFile> previous_creature_sound_data;
  bool sound_display_cloned = false;
  unsigned sound_display_source = 0;
  bool const sound_changed = sound_editor->voiceChanged() || sound_editor->actionChanged();
  QString generated_texture_path;
  auto restore_character_assets = [&]
  {
    if (previous_display_info)
    {
      _display_info->overwriteWith(*previous_display_info);
      _display_info->save();
    }
    if (previous_display_extra)
    {
      _display_extra->overwriteWith(*previous_display_extra);
      _display_extra->save();
    }
    if (previous_npc_sounds)
    {
      _npc_sounds->overwriteWith(*previous_npc_sounds);
      _npc_sounds->save();
    }
    if (previous_creature_sound_data)
    {
      _creature_sound_data->overwriteWith(*previous_creature_sound_data);
      _creature_sound_data->save();
    }
    if (!generated_texture_path.isEmpty()) QFile::remove(generated_texture_path);
  };

  if (character_appearance_editor)
  {
    rebuild_character_preview();
    if (composed_character_body.isNull() || !character_base_display
        || !character_race || !character_sex || !character_skin || !character_face
        || !character_hair_style || !character_hair_color || !character_facial_hair
        || !_display_info || !_display_extra || _display_extra->getFieldCount() < 21)
    {
      QMessageBox::critical(this, "Humanoid NPC was not created",
        "The selected appearance could not be composed into the WotLK client format.");
      return;
    }

    auto next_dbc_id = [](DBCFile& file)
    {
      unsigned next = 1;
      for (std::size_t row = 0; row < file.getRecordCount(); ++row)
        next = std::max(next, file.getRecord(row).getUInt(0) + 1);
      return next;
    };
    unsigned const extra_id = next_dbc_id(*_display_extra);
    unsigned custom_display_id = next_dbc_id(*_display_info);
    QString const project_path = QString::fromStdString(_client_data->projectPath());
    while (QFile::exists(QDir(project_path).filePath(
      QString("Textures/BakedNpcTextures/NoggitNpc_%1.blp").arg(custom_display_id))))
      ++custom_display_id;
    QString const bake_stem = QString("NoggitNpc_%1").arg(custom_display_id);
    QString const bake_path = QString("Textures\\BakedNpcTextures\\%1.blp").arg(bake_stem);
    generated_texture_path = QDir(project_path).filePath(
      QString("Textures/BakedNpcTextures/%1.blp").arg(bake_stem));
    previous_display_info = std::make_unique<DBCFile>(*_display_info);
    previous_display_extra = std::make_unique<DBCFile>(*_display_extra);
    QString texture_error;
    if (!writeCharacterBlp(composed_character_body, bake_path, _client_data, texture_error))
    {
      generated_texture_path.clear();
      QMessageBox::critical(this, "Humanoid NPC was not created", texture_error);
      return;
    }

    try
    {
      auto extra = _display_extra->addRecord(extra_id);
      extra.write(1, character_race->currentData().toUInt());
      extra.write(2, character_sex->currentData().toUInt());
      extra.write(3, character_skin->currentData().toUInt());
      extra.write(4, character_face->currentData().toUInt());
      extra.write(5, character_hair_style->currentData().toUInt());
      extra.write(6, character_hair_color->currentData().toUInt());
      extra.write(7, character_facial_hair->currentData().toUInt());
      for (std::size_t slot = 0; slot < character_armor_displays.size(); ++slot)
        extra.write(8 + slot, character_armor_displays[slot]);
      extra.write(19, 0u);
      extra.writeString(20, bake_stem.toStdString());

      auto const base_display = _display_info->getByID(character_base_display);
      std::vector<unsigned> base_display_fields(_display_info->getFieldCount());
      for (std::size_t field_index = 1; field_index < base_display_fields.size(); ++field_index)
        base_display_fields[field_index] = base_display.getUInt(field_index);
      auto custom_display = _display_info->addRecord(custom_display_id);
      for (std::size_t field_index = 1; field_index < _display_info->getFieldCount(); ++field_index)
        custom_display.write(field_index, base_display_fields[field_index]);
      custom_display.write(3, extra_id);
      _display_extra->save();
      _display_info->save();
      display->setValue(static_cast<int>(custom_display_id));
    }
    catch (std::exception const& exception)
    {
      restore_character_assets();
      QMessageBox::critical(this, "Humanoid NPC was not created",
        QString("Could not write the custom client display records: %1").arg(exception.what()));
      return;
    }
  }

  if (sound_changed)
  {
    QString deployment_error;
    if (!npcDbcDeploymentConfigured(deployment_error))
    {
      if (character_appearance_editor) restore_character_assets();
      QMessageBox::critical(this, "Sound DBC deployment folders required", deployment_error);
      _connection_toggle->setChecked(true);
      return;
    }
    auto next_dbc_id = [](DBCFile& file)
    {
      unsigned next = 1;
      for (std::size_t row = 0; row < file.getRecordCount(); ++row)
        next = std::max(next, file.getRecord(row).getUInt(0) + 1);
      return next;
    };
    try
    {
      if (!character_appearance_editor)
      {
        sound_display_source = static_cast<unsigned>(display->value());
        auto const original = _display_info->getByID(sound_display_source);
        std::vector<unsigned> fields(_display_info->getFieldCount());
        for (std::size_t field = 1; field < fields.size(); ++field)
          fields[field] = original.getUInt(field);
        previous_display_info = std::make_unique<DBCFile>(*_display_info);
        unsigned const new_id = next_dbc_id(*_display_info);
        auto private_display = _display_info->addRecord(new_id);
        for (std::size_t field = 1; field < fields.size(); ++field)
          private_display.write(field, fields[field]);
        QSignalBlocker const display_blocker(display);
        display->setValue(static_cast<int>(new_id));
        sound_display_cloned = true;
      }

      auto private_display = _display_info->getByID(static_cast<unsigned>(display->value()));
      if (sound_editor->voiceChanged())
      {
        if (sound_editor->voiceOverridesChanged())
        {
          previous_npc_sounds = std::make_unique<DBCFile>(*_npc_sounds);
          unsigned const new_id = next_dbc_id(*_npc_sounds);
          auto private_voices = _npc_sounds->addRecord(new_id);
          for (unsigned slot = 0; slot < 4; ++slot)
            private_voices.write(slot + 1, sound_editor->voiceSound(slot));
          private_display.write(12, new_id);
          _npc_sounds->save();
        }
        else
          private_display.write(12, sound_editor->selectedVoiceBaseId());
      }
      if (sound_editor->actionChanged())
      {
        if (sound_editor->actionOverridesChanged())
        {
          previous_creature_sound_data = std::make_unique<DBCFile>(*_creature_sound_data);
          unsigned const new_id = next_dbc_id(*_creature_sound_data);
          std::vector<unsigned> fields(_creature_sound_data->getFieldCount());
          if (sound_editor->selectedActionBaseId()
              && _creature_sound_data->CheckIfIdExists(sound_editor->selectedActionBaseId()))
          {
            auto const base = _creature_sound_data->getByID(sound_editor->selectedActionBaseId());
            for (std::size_t field = 1; field < fields.size(); ++field)
              fields[field] = base.getUInt(field);
          }
          auto private_actions = _creature_sound_data->addRecord(new_id);
          for (std::size_t field = 1; field < fields.size(); ++field)
            private_actions.write(field, fields[field]);
          for (unsigned field = 1; field < 38; ++field)
            if (field != 23 && field != 25 && field != 31 && field != 32 && field != 37)
              private_actions.write(field, sound_editor->actionSound(field));
          private_display.write(2, new_id);
          _creature_sound_data->save();
        }
        else
          private_display.write(2, sound_editor->selectedActionBaseId());
      }
      _display_info->save();
    }
    catch (std::exception const& exception)
    {
      restore_character_assets();
      QMessageBox::critical(this, "NPC sounds were not saved",
        QString("Could not write the private sound records: %1").arg(exception.what()));
      return;
    }
  }

  QString display_failure;
  if (!loadDisplay(static_cast<unsigned>(display->value()), false, &display_failure))
  {
    if (character_appearance_editor || sound_changed) restore_character_assets();
    QMessageBox::critical(this, "Custom NPC was not created",
                          "The chosen display cannot be rendered: " + display_failure);
    return;
  }
  for (auto const& [choice, attachment_id] :
       {std::pair{&main_choice, 1u}, std::pair{&off_choice, 2u},
        std::pair{&ranged_choice, 12u}})
    apply_weapon_choice(_preview, *choice, attachment_id);

  std::uint64_t const spawn_guid = selected_spawn ? *_selected_spawn_guid : 0;
  unsigned new_entry = 0;
  bool const has_equipment = main_hand->value() || off_hand->value() || ranged->value();
  error.clear();
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
     true, [&](QSqlDatabase& db, QString& failure)
     {
       Noggit::Sql::WorldDatabaseSchema schema;
       if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
       QString const lock_name = QString("noggit_npc_editor_%1")
        .arg(_database->text().trimmed()).left(64);
      QSqlQuery lock(db);
      lock.prepare("SELECT GET_LOCK(?, 5)");
      lock.addBindValue(lock_name);
      if (!lock.exec() || !lock.next() || lock.value(0).toInt() != 1)
      {
        failure = "Could not acquire the NPC template allocation lock.";
        return false;
       }
       lock.finish();
        bool model_info_inserted = false;
        std::vector<std::pair<qlonglong, unsigned>> inserted_dialogue_triggers;
        std::vector<std::tuple<qlonglong, unsigned, unsigned>> original_spawn_facing;
        bool copied_creature_text = false;
       auto unlock = [&]
      {
        QSqlQuery release(db);
        release.prepare("SELECT RELEASE_LOCK(?)");
        release.addBindValue(lock_name);
        release.exec();
      };
      auto rollback = [&](QString message)
      {
         QSqlQuery query(db);
         query.exec("ROLLBACK");
         for (auto const& [owner, smart_id] : inserted_dialogue_triggers)
         {
           QSqlQuery remove_trigger(db);
           remove_trigger.prepare("DELETE FROM smart_scripts WHERE entryorguid = ? "
                                  "AND source_type = 0 AND id = ? AND link = 0");
           remove_trigger.addBindValue(owner);
           remove_trigger.addBindValue(smart_id);
           if (!remove_trigger.exec())
              message += " Could not remove a staged dialogue trigger: "
                + remove_trigger.lastError().text();
         }
         for (auto const& [owner, smart_id, duration] : original_spawn_facing)
         {
           QSqlQuery restore(db);
           restore.prepare("UPDATE smart_scripts SET action_param5 = ? WHERE entryorguid = ? "
                           "AND source_type = 0 AND id = ? AND action_type = 1");
           restore.addBindValue(duration);
           restore.addBindValue(owner);
           restore.addBindValue(smart_id);
           if (!restore.exec())
             message += " Could not restore spawn dialogue facing: "
               + restore.lastError().text();
         }
         // creature_text is MyISAM in legacy world databases. Rollback leaves
         // both the cloned rows and any newly inserted dialogue behind.
         if (copied_creature_text)
         {
           QSqlQuery remove_line(db);
           remove_line.prepare("DELETE FROM creature_text WHERE CreatureID = ?");
           remove_line.addBindValue(new_entry);
           if (!remove_line.exec())
             message += " Could not remove the copied dialogue rows: "
               + remove_line.lastError().text();
         }
         // creature_model_info is MyISAM in WotLK world databases, so a SQL
        // rollback does not remove a row inserted there.
        if (model_info_inserted && !schema.uses_creature_template_model)
        {
          QSqlQuery remove(db);
          remove.prepare("DELETE FROM creature_model_info WHERE DisplayID = ?");
          remove.addBindValue(display->value());
          if (!remove.exec())
            message += " Could not remove the new creature_model_info row: "
              + remove.lastError().text();
        }
        unlock();
        failure = std::move(message);
        return false;
      };

      QSqlQuery begin(db);
      if (!begin.exec("START TRANSACTION"))
      {
        failure = "Could not start the NPC editor transaction: " + begin.lastError().text();
        unlock();
        return false;
      }

      QSqlQuery allocate(db);
      if (!allocate.exec("SELECT COALESCE(MAX(entry), 0) + 1 FROM creature_template")
          || !allocate.next() || !allocate.value(0).toUInt())
        return rollback("Could not allocate a custom creature template ID: "
                        + allocate.lastError().text());
      new_entry = allocate.value(0).toUInt();
      allocate.finish();

      QSqlQuery columns(db);
      if (!columns.exec("SHOW COLUMNS FROM creature_template"))
        return rollback("Could not inspect creature_template: " + columns.lastError().text());
      QStringList template_columns;
      std::set<QString> available_template_columns;
      while (columns.next())
      {
        QString const column = columns.value(0).toString();
        available_template_columns.insert(column.toCaseFolded());
        if (column.compare("entry", Qt::CaseInsensitive) == 0
            || columns.value(5).toString().contains("auto_increment", Qt::CaseInsensitive))
          continue;
        template_columns.push_back("`" + QString(column).replace('`', "``") + "`");
      }
      columns.finish();

      QSqlQuery clone(db);
      clone.prepare(QString("INSERT INTO creature_template (`entry`, %1) "
                            "SELECT ?, %1 FROM creature_template WHERE entry = ?")
                      .arg(template_columns.join(", ")));
      clone.addBindValue(new_entry);
      clone.addBindValue(source_entry);
      if (!clone.exec() || clone.numRowsAffected() != 1)
        return rollback("Could not clone the source creature template: " + clone.lastError().text());
      clone.finish();

       QSqlQuery edit(db);
       edit.prepare("UPDATE creature_template SET name = ?, subname = ?, minlevel = ?, "
                    "maxlevel = ?, faction = ?, npcflag = ?, type = ?, `rank` = ? "
                    "WHERE entry = ?");
      edit.addBindValue(name->text().trimmed());
      edit.addBindValue(subname->text().trimmed());
      edit.addBindValue(min_level->value());
      edit.addBindValue(max_level->value());
      edit.addBindValue(faction->factionId());
       edit.addBindValue(npc_flags->value());
      edit.addBindValue(creature_type->value());
      edit.addBindValue(rank->value());
      edit.addBindValue(new_entry);
       if (!edit.exec())
         return rollback("Could not save the custom NPC fields: " + edit.lastError().text());
       QString model_error;
       if (!Noggit::Sql::replaceTemplateDisplay(db, schema, new_entry,
                                                 display->value(), model_error))
         return rollback(model_error);

       auto const dialogue_line_changes = dialogue_sound_editor->newDialogueLines();
       auto const dialogue_line_updates = dialogue_sound_editor->dialogueLineUpdates();
       auto const dialogue_trigger_changes = dialogue_sound_editor->triggerChanges();
       auto const dialogue_facing_changes = dialogue_sound_editor->facingChanges();
       QSet<QString> creature_text_columns;
       if (!dialogue_line_changes.empty() || !dialogue_line_updates.empty())
       {
         if (!tableExists(db, "creature_text"))
           return rollback("This world database has no creature_text table for new dialogue.");
         QString column_error;
         if (!Noggit::Sql::worldTableColumns(db, "creature_text", creature_text_columns,
                                             column_error))
           return rollback("Could not inspect creature_text for new dialogue: " + column_error);
         for (char const* required_name : {
                "creatureid", "groupid", "id", "text", "type", "language",
                "probability", "emote", "duration", "sound"})
         {
           QString const required = QString::fromLatin1(required_name);
           if (!creature_text_columns.contains(required))
             return rollback("The creature_text table is missing required column "
                             + required + ".");
         }
       }
       QSet<QString> smart_script_columns;
       if (!dialogue_trigger_changes.empty() || !dialogue_facing_changes.empty())
       {
         if (!tableExists(db, "smart_scripts"))
           return rollback("This world database has no smart_scripts table for dialogue triggers.");
         QString column_error;
         if (!Noggit::Sql::worldTableColumns(db, "smart_scripts", smart_script_columns,
                                             column_error))
           return rollback("Could not inspect smart_scripts for dialogue triggers: "
                           + column_error);
         for (char const* required_name : {
                "entryorguid", "source_type", "id", "link", "event_type",
                "event_phase_mask", "event_chance", "event_flags", "event_param1",
                "event_param2", "event_param3", "event_param4", "action_type",
                "action_param1", "action_param2", "action_param3", "action_param4",
                "action_param5", "action_param6", "target_type", "target_param1",
                "target_param2", "target_param3", "target_x", "target_y", "target_z",
                "target_o", "comment"})
         {
           QString const required = QString::fromLatin1(required_name);
           if (!smart_script_columns.contains(required))
             return rollback("The smart_scripts table is missing required column "
                             + required + ".");
         }
         QSqlQuery enable_smart_ai(db);
         enable_smart_ai.prepare("UPDATE creature_template SET AIName = 'SmartAI' "
                                 "WHERE entry = ? AND COALESCE(ScriptName, '') = ''");
         enable_smart_ai.addBindValue(new_entry);
         if (!enable_smart_ai.exec())
           return rollback("Could not enable SmartAI for the new dialogue trigger: "
                           + enable_smart_ai.lastError().text());
       }

      if (character_creator)
      {
        QStringList clean_fields;
        for (char const* field_name : {"lootid", "pickpocketloot", "skinloot", "KillCredit1",
                                       "KillCredit2", "mingold", "maxgold"})
          if (available_template_columns.count(QString::fromLatin1(field_name).toCaseFolded()))
            clean_fields.push_back(QString("`%1` = 0").arg(field_name));
        for (char const* field_name : {"AIName", "ScriptName"})
          if (available_template_columns.count(QString::fromLatin1(field_name).toCaseFolded()))
            clean_fields.push_back(QString("`%1` = ''").arg(field_name));
        for (int spell = 1; spell <= 8; ++spell)
        {
          QString const field_name = QString("spell%1").arg(spell);
          if (available_template_columns.count(field_name.toCaseFolded()))
            clean_fields.push_back(QString("`%1` = 0").arg(field_name));
        }
        if (!clean_fields.isEmpty())
        {
          QSqlQuery clean(db);
          clean.prepare(QString("UPDATE creature_template SET %1 WHERE entry = ?")
                          .arg(clean_fields.join(", ")));
          clean.addBindValue(new_entry);
          if (!clean.exec())
            return rollback("Could not clear inherited NPC scripts and loot: "
                            + clean.lastError().text());
        }
      }

      struct RelatedTable { char const* table; char const* key; bool smart = false; };
      static RelatedTable const related[] = {
        {"npc_vendor", "entry"},
        {"creature_queststarter", "id"},
        {"creature_questender", "id"},
        {"creature_default_trainer", "CreatureId"},
        {"creature_template_addon", "entry"},
        {"creature_template_spell", "CreatureID"},
        {"creature_template_resistance", "CreatureID"},
        {"creature_template_movement", "CreatureId"},
        {"creature_template_locale", "entry"},
        {"creature_text", "CreatureID"},
        {"smart_scripts", "entryorguid", true}
      };
      for (RelatedTable const& table : related)
      {
        if (character_creator) continue;
        if (!tableExists(db, table.table)) continue;
        QSqlQuery fields(db);
        if (!fields.exec(QString("SHOW COLUMNS FROM `%1`").arg(table.table)))
          return rollback(QString("Could not inspect %1: %2")
                          .arg(table.table, fields.lastError().text()));
        QStringList other_columns;
        bool has_key = false;
        while (fields.next())
        {
          QString const column = fields.value(0).toString();
          if (column.compare(table.key, Qt::CaseInsensitive) == 0)
          {
            has_key = true;
            continue;
          }
          if (fields.value(5).toString().contains("auto_increment", Qt::CaseInsensitive))
            continue;
          other_columns.push_back("`" + QString(column).replace('`', "``") + "`");
        }
        fields.finish();
        if (!has_key) continue;
        QString const extra = other_columns.isEmpty() ? QString()
          : ", " + other_columns.join(", ");
        QSqlQuery copy(db);
        copy.prepare(QString("INSERT INTO `%1` (`%2`%3) SELECT ?%3 FROM `%1` "
                             "WHERE `%2` = ?%4")
          .arg(table.table, table.key, extra,
               table.smart ? " AND `source_type` = 0" : ""));
        copy.addBindValue(new_entry);
        copy.addBindValue(source_entry);
         if (!copy.exec())
           return rollback(QString("Could not copy %1: %2")
                           .arg(table.table, copy.lastError().text()));
         if (QString::fromLatin1(table.table) == "creature_text")
           copied_creature_text = true;
       }

       for (NpcDialogueLineChange const& change : dialogue_line_changes)
       {
         if (change.broadcast_text_id)
         {
           if (!tableExists(db, "broadcast_text"))
             return rollback("The world database has no broadcast_text table for this dialogue link.");
           QSqlQuery broadcast(db);
           broadcast.prepare("SELECT 1 FROM broadcast_text WHERE ID = ? LIMIT 1");
           broadcast.addBindValue(change.broadcast_text_id);
           if (!broadcast.exec() || !broadcast.next())
             return rollback(QString("BroadcastText %1 does not exist: %2")
               .arg(change.broadcast_text_id)
               .arg(broadcast.lastError().isValid() ? broadcast.lastError().text()
                                                  : "No matching row."));
         }
         QStringList insert_columns;
         QStringList placeholders;
         QVariantList values;
         auto add_value = [&](QString const& column, QVariant const& value)
         {
           if (!creature_text_columns.contains(column.toLower())) return;
           insert_columns.push_back("`" + column + "`");
           placeholders.push_back("?");
           values.push_back(value);
         };
         add_value("CreatureID", new_entry);
         add_value("GroupID", change.group_id);
         add_value("ID", change.line_id);
         add_value("Text", change.text);
         add_value("Type", change.chat_type);
         add_value("Language", change.language);
         add_value("Probability", change.probability);
         add_value("Emote", change.emote);
         add_value("Duration", change.duration);
         add_value("Sound", change.sound);
         add_value("BroadcastTextId", change.broadcast_text_id);
         add_value("TextRange", change.text_range);
         add_value("comment", change.comment);
         QSqlQuery insert_line(db);
         insert_line.prepare(QString("INSERT INTO creature_text (%1) VALUES (%2)")
           .arg(insert_columns.join(", "), placeholders.join(", ")));
         for (QVariant const& value : values) insert_line.addBindValue(value);
         if (!insert_line.exec())
           return rollback(QString("Could not save dialogue group %1, line %2: %3")
             .arg(change.group_id).arg(change.line_id).arg(insert_line.lastError().text()));
       }

       for (NpcDialogueLineUpdate const& change : dialogue_line_updates)
       {
         auto const& line = change.replacement;
         if (line.broadcast_text_id)
         {
           if (!tableExists(db, "broadcast_text"))
             return rollback("The world database has no broadcast_text table for this dialogue link.");
           QSqlQuery broadcast(db);
           broadcast.prepare("SELECT 1 FROM broadcast_text WHERE ID = ? LIMIT 1");
           broadcast.addBindValue(line.broadcast_text_id);
           if (!broadcast.exec() || !broadcast.next())
             return rollback(QString("BroadcastText %1 does not exist: %2")
               .arg(line.broadcast_text_id)
               .arg(broadcast.lastError().isValid() ? broadcast.lastError().text()
                                                    : "No matching row."));
         }
         QStringList assignments;
         QVariantList values;
         auto add_value = [&](QString const& column, QVariant const& value)
         {
           if (!creature_text_columns.contains(column.toLower())) return;
           assignments.push_back("`" + column + "` = ?");
           values.push_back(value);
         };
         add_value("Text", line.text);
         add_value("Type", line.chat_type);
         add_value("Language", line.language);
         add_value("Probability", line.probability);
         add_value("Emote", line.emote);
         add_value("Duration", line.duration);
         add_value("Sound", line.sound);
         add_value("BroadcastTextId", line.broadcast_text_id);
         add_value("TextRange", line.text_range);
         add_value("comment", line.comment);
         QSqlQuery update_line(db);
         update_line.prepare(QString("UPDATE creature_text SET %1 WHERE CreatureID = ? "
                                     "AND GroupID = ? AND ID = ?")
           .arg(assignments.join(", ")));
         for (QVariant const& value : values) update_line.addBindValue(value);
         update_line.addBindValue(new_entry);
         update_line.addBindValue(line.group_id);
         update_line.addBindValue(line.line_id);
         if (!update_line.exec() || update_line.numRowsAffected() != 1)
           return rollback(QString("Could not update dialogue group %1, line %2: %3")
             .arg(line.group_id).arg(line.line_id)
             .arg(update_line.lastError().isValid() ? update_line.lastError().text()
                                                    : "The copied dialogue row was not found."));
       }

       for (NpcDialogueSoundChange const& change : dialogue_sound_editor->changes())
       {
         QSqlQuery dialogue_sound(db);
         dialogue_sound.prepare("UPDATE creature_text SET Sound = ? "
                                "WHERE CreatureID = ? AND GroupID = ? AND ID = ?");
         dialogue_sound.addBindValue(change.sound);
         dialogue_sound.addBindValue(new_entry);
         dialogue_sound.addBindValue(change.group_id);
         dialogue_sound.addBindValue(change.line_id);
         if (!dialogue_sound.exec() || dialogue_sound.numRowsAffected() != 1)
           return rollback(QString("Could not save dialogue sound for group %1, line %2: %3")
             .arg(change.group_id).arg(change.line_id)
             .arg(dialogue_sound.lastError().isValid()
               ? dialogue_sound.lastError().text() : "The copied dialogue row was not found."));
        }

       for (NpcDialogueTriggerChange const& change : dialogue_trigger_changes)
       {
         if (change.selected_spawn_only && !selected_spawn)
           return rollback("A selected-spawn dialogue trigger requires an existing NPC spawn.");
         qlonglong const owner = change.selected_spawn_only
           ? -static_cast<qlonglong>(spawn_guid) : static_cast<qlonglong>(new_entry);
         QSqlQuery next_id(db);
         next_id.prepare("SELECT COALESCE(MAX(id) + 1, 0) FROM smart_scripts "
                         "WHERE entryorguid = ? AND source_type = 0");
         next_id.addBindValue(owner);
         if (!next_id.exec() || !next_id.next())
           return rollback("Could not allocate a SmartAI dialogue trigger ID: "
                           + next_id.lastError().text());
         unsigned const smart_id = next_id.value(0).toUInt();

         QStringList insert_columns;
         QStringList placeholders;
         QVariantList values;
         auto add_value = [&](QString const& column, QVariant const& value)
         {
           if (!smart_script_columns.contains(column.toLower())) return;
           insert_columns.push_back("`" + column + "`");
           placeholders.push_back("?");
           values.push_back(value);
         };
         add_value("entryorguid", owner);
         add_value("source_type", 0);
         add_value("id", smart_id);
         add_value("link", 0);
         add_value("event_type", change.event_type);
         add_value("event_phase_mask", 0);
         add_value("event_chance", change.event_chance);
         add_value("event_flags", 0);
         for (unsigned param = 0; param < change.event_params.size(); ++param)
           add_value(QString("event_param%1").arg(param + 1), change.event_params[param]);
         add_value("action_type", 1);
         add_value("action_param1", change.group_id);
         add_value("action_param2", 0);
         add_value("action_param3", 0);
         add_value("action_param4", change.speech_delay);
         add_value("action_param5", change.face_player_duration);
         add_value("action_param6", 0);
         add_value("target_type", 1);
         add_value("target_param1", 0);
         add_value("target_param2", 0);
         add_value("target_param3", 0);
         add_value("target_param4", 0);
         add_value("target_x", 0);
         add_value("target_y", 0);
         add_value("target_z", 0);
         add_value("target_o", 0);
         add_value("comment", QString("Noggit: %1 -> dialogue group %2")
           .arg(change.event_label).arg(change.group_id));
         QSqlQuery insert_trigger(db);
         insert_trigger.prepare(QString("INSERT INTO smart_scripts (%1) VALUES (%2)")
           .arg(insert_columns.join(", "), placeholders.join(", ")));
         for (QVariant const& value : values) insert_trigger.addBindValue(value);
         if (!insert_trigger.exec())
           return rollback(QString("Could not save the %1 dialogue trigger: %2")
             .arg(change.event_label, insert_trigger.lastError().text()));
         inserted_dialogue_triggers.emplace_back(owner, smart_id);
       }

       if (has_equipment)
      {
        if (!tableExists(db, "creature_equip_template"))
          return rollback("This world database has no creature_equip_template table.");
        QSqlQuery equipment(db);
        equipment.prepare("INSERT INTO creature_equip_template "
                          "(CreatureID, ID, ItemID1, ItemID2, ItemID3) VALUES (?, 1, ?, ?, ?)");
        equipment.addBindValue(new_entry);
        equipment.addBindValue(main_hand->value());
        equipment.addBindValue(off_hand->value());
        equipment.addBindValue(ranged->value());
        if (!equipment.exec())
          return rollback("Could not save NPC equipment: " + equipment.lastError().text());
      }

       if (selected_spawn)
       {
         QSqlQuery attach(db);
         QString attach_sql = QString("UPDATE creature SET %1 = ?, equipment_id = ?")
           .arg(schema.creatureEntry());
         if (schema.creature_entry_column == "id1") attach_sql += ", `id2` = 0, `id3` = 0";
         if (!schema.creature_spawn_display_column.isEmpty())
           attach_sql += ", " + schema.creatureSpawnDisplay() + " = 0";
         attach_sql += QString(" WHERE guid = ? AND %1 = ?").arg(schema.creatureEntry());
         attach.prepare(attach_sql);
        attach.addBindValue(new_entry);
        attach.addBindValue(has_equipment ? 1 : 0);
        attach.addBindValue(QVariant::fromValue<qulonglong>(spawn_guid));
        attach.addBindValue(source_entry);
        if (!attach.exec() || attach.numRowsAffected() != 1)
          return rollback("Could not assign the custom template to the selected spawn: "
                          + attach.lastError().text());
      }

      if (character_appearance_editor || sound_display_cloned)
      {
        // The server requires model dimensions and gender for every new
        // CreatureDisplayInfo display, even when the client can render it.
        QSqlQuery model_info(db);
        model_info.prepare("INSERT INTO creature_model_info "
                           "(DisplayID, BoundingRadius, CombatReach, Gender, DisplayID_Other_Gender) "
                           "SELECT ?, BoundingRadius, CombatReach, Gender, 0 "
                           "FROM creature_model_info WHERE DisplayID = ?");
        model_info.addBindValue(display->value());
        model_info.addBindValue(character_appearance_editor
          ? character_base_display : sound_display_source);
        if (!model_info.exec() || model_info.numRowsAffected() != 1)
          return rollback(QString("Could not create creature_model_info for display %1 "
                                  "from base display %2: %3")
                            .arg(display->value()).arg(character_appearance_editor
                              ? character_base_display : sound_display_source)
                            .arg(model_info.lastError().isValid()
                                   ? model_info.lastError().text()
                                   : "The base display has no creature_model_info row."));
        model_info_inserted = true;
      }

      for (NpcDialogueFacingChange const& change : dialogue_facing_changes)
      {
        qlonglong const owner = change.smart_owner < 0
          ? static_cast<qlonglong>(change.smart_owner) : static_cast<qlonglong>(new_entry);
        if (change.smart_owner != static_cast<std::int64_t>(source_entry)
            && (!selected_spawn
                || change.smart_owner != -static_cast<std::int64_t>(spawn_guid)))
          return rollback("An existing dialogue trigger has a different owner.");
        QSqlQuery previous(db);
        previous.prepare("SELECT action_param5 FROM smart_scripts WHERE entryorguid = ? "
                         "AND source_type = 0 AND id = ? AND action_type = 1 "
                         "AND action_param1 = ? FOR UPDATE");
        previous.addBindValue(owner);
        previous.addBindValue(change.smart_id);
        previous.addBindValue(change.group_id);
        if (!previous.exec() || !previous.next()
            || previous.value(0).toUInt() != change.original_duration)
          return rollback("An existing dialogue trigger changed before facing could be saved.");
        previous.finish();
        QSqlQuery update(db);
        update.prepare("UPDATE smart_scripts SET action_param5 = ? WHERE entryorguid = ? "
                       "AND source_type = 0 AND id = ? AND action_type = 1 "
                       "AND action_param1 = ? AND action_param5 = ?");
        update.addBindValue(change.duration);
        update.addBindValue(owner);
        update.addBindValue(change.smart_id);
        update.addBindValue(change.group_id);
        update.addBindValue(change.original_duration);
        if (!update.exec() || update.numRowsAffected() != 1)
          return rollback("Could not update dialogue facing: " + update.lastError().text());
        if (owner < 0)
          original_spawn_facing.emplace_back(owner, change.smart_id,
                                             change.original_duration);
      }

      QSqlQuery commit(db);
      if (!commit.exec("COMMIT"))
        return rollback("Could not commit the custom NPC: " + commit.lastError().text());
      unlock();
      return true;
    }, error);

  if (!saved)
  {
    if (character_appearance_editor || sound_changed) restore_character_assets();
    QMessageBox::critical(this, "Custom NPC was not created",
                          "Could not save the custom NPC: " + error);
    _status->setText("Could not save the custom NPC: " + error);
    return;
  }

  QString dbc_deployment_warning;
  if ((character_appearance_editor || sound_changed)
      && !deployNpcDbcs(dbc_deployment_warning))
  {
    QMessageBox::warning(this, "NPC created with DBC deployment failure",
      "The NPC and project DBCs were saved, but the updated DBCs could not be deployed: "
      + dbc_deployment_warning
      + "\n\nCorrect the deployment folders and use Deploy NPC DBCs now.");
  }
  QString const deployment_suffix = dbc_deployment_warning.isEmpty() ? QString()
    : " DBC deployment failed: " + dbc_deployment_warning;

  QString stance_warning;
  bool stance_saved = true;
  int const chosen_stance = drawn_weapon->currentData().toInt();
  if (selected_spawn && (chosen_stance == 1 || chosen_stance == 2))
    stance_saved = useWorldDatabase(
      _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
      true, [&](QSqlDatabase& db, QString& failure)
      {
        return writeSpawnWeaponStance(db, spawn_guid, new_entry,
                                      static_cast<unsigned>(chosen_stance), failure);
      }, stance_warning);

  loadTemplates();
  if (selected_spawn)
  {
    _selected_spawn_guid.reset();
    selectSpawn(spawn_guid, new_entry);
    bool preview_loaded = loadDisplay(static_cast<unsigned>(display->value()), false);
    if (preview_loaded)
      for (auto const& [choice, attachment_id] :
           {std::pair{&main_choice, 1u}, std::pair{&off_choice, 2u},
            std::pair{&ranged_choice, 12u}})
        apply_weapon_choice(_preview, *choice, attachment_id);
    if (preview_loaded)
      refreshSpawnAppearance(static_cast<unsigned>(display->value()), new_entry, true);
    if (_selected_spawn_guid)
      updateCachedSpawn(spawn_guid, new_entry, _edit_spawn_position,
                        static_cast<unsigned>(display->value()),
                        static_cast<float>(_edit_yaw->value()));
    if (stance_saved && (chosen_stance == 1 || chosen_stance == 2))
    {
      previewSelectedWeaponStance();
      saveSelectedWeaponVisibility();
    }
    QString const message = QString("Created unique template %1 and assigned it to spawn GUID %2. "
                              "Equipment set %3 will appear after the world server reloads.")
                        .arg(new_entry).arg(spawn_guid).arg(has_equipment ? 1 : 0);
    _status->setText((stance_saved ? message
      : message + " The drawn weapon was not saved: " + stance_warning)
      + deployment_suffix);
    if (!stance_saved)
      QMessageBox::warning(this, "NPC created without weapon stance",
        "The NPC was created, but its in-game drawn weapon could not be saved: "
        + stance_warning);
  }
  else if (selectTemplate(new_entry, false))
  {
    loadDisplay(static_cast<unsigned>(display->value()), false);
    startPlacement();
    _placement_weapon_stance = drawn_weapon->currentData().toInt();
    if (_placement_overlay_guid && (_placement_weapon_stance == 1
                                    || _placement_weapon_stance == 2))
      if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_placement_overlay_guid))
        overlay->setWeaponVisibility(_placement_weapon_stance == 1,
                                     _placement_weapon_stance == 2);
    if (!_placement_equipment_id && has_equipment) _placement_equipment_id = 1;
    _status->setText((character_creator
      ? QString("Created humanoid NPC template %1 with custom display %2. Click terrain or a WMO "
                "to place it. Updated display DBCs were deployed to the client and server.")
          .arg(new_entry).arg(display->value())
      : QString("Created custom NPC template %1. Click terrain or a WMO to place it.")
          .arg(new_entry)) + deployment_suffix);
  }
  else
  {
    _status->setText(QString("Created custom NPC template %1, but it could not be selected in "
                             "the refreshed browser.").arg(new_entry));
  }
}

void NpcTemplateBrowser::saveUniqueTemplate()
{
  if (!_selected_spawn_guid) return;
  if (_edit_name->text().trimmed().isEmpty()
      || _edit_minlevel->value() > _edit_maxlevel->value()
      || _edit_template_display->value() == 0)
  {
    _status->setText("Enter a name, valid level range, and display ID.");
    return;
  }
  unsigned const display = static_cast<unsigned>(_edit_template_display->value());
  if (!loadDisplay(display, false))
  {
    _status->setText("The display ID is unavailable in the current client files.");
    return;
  }

  unsigned const source_entry = _selected_spawn_entry;
  unsigned new_entry = 0;
  QString error;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
     true, [&](QSqlDatabase& db, QString& failure)
     {
       Noggit::Sql::WorldDatabaseSchema schema;
       if (!Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure)) return false;
       QString const lock_name = QString("noggit_npc_template_%1")
        .arg(_database->text().trimmed()).left(64);
      QSqlQuery lock(db);
      lock.prepare("SELECT GET_LOCK(?, 5)");
      lock.addBindValue(lock_name);
      if (!lock.exec() || !lock.next() || lock.value(0).toInt() != 1)
      {
        failure = "Could not acquire the NPC template allocation lock.";
        return false;
      }
      lock.finish();
      auto unlock = [&]
      {
        QSqlQuery query(db);
        query.prepare("SELECT RELEASE_LOCK(?)");
        query.addBindValue(lock_name);
        query.exec();
      };
      struct RelatedTable { char const* table; char const* key; bool smart = false; };
      static RelatedTable const related[] = {
        {"npc_vendor", "entry"},
        {"creature_queststarter", "id"},
        {"creature_questender", "id"},
        {"creature_default_trainer", "CreatureId"},
        {"creature_equip_template", "CreatureID"},
        {"creature_template_addon", "entry"},
        {"creature_template_spell", "CreatureID"},
        {"creature_template_resistance", "CreatureID"},
        {"creature_template_movement", "CreatureId"},
        {"creature_template_locale", "entry"},
        {"creature_text", "CreatureID"},
        {"smart_scripts", "entryorguid", true}
      };
       std::vector<RelatedTable> copied;
       bool template_created = false;
       bool model_created = false;
       auto cleanup = [&]
       {
         if (!new_entry) return;
         if (model_created)
         {
           QSqlQuery remove(db);
           remove.prepare("DELETE FROM `creature_template_model` WHERE `CreatureID` = ?");
           remove.addBindValue(new_entry);
           remove.exec();
         }
        for (RelatedTable const& table : copied)
        {
          QSqlQuery remove(db);
          remove.prepare(QString("DELETE FROM `%1` WHERE `%2` = ?")
            .arg(table.table, table.key));
          remove.addBindValue(new_entry);
          remove.exec();
        }
        if (template_created)
        {
          QSqlQuery remove(db);
          remove.prepare("DELETE FROM creature_template WHERE entry = ?");
          remove.addBindValue(new_entry);
          remove.exec();
        }
      };
      auto fail = [&]() -> bool { cleanup(); unlock(); return false; };

      QSqlQuery allocate(db);
      if (!allocate.exec("SELECT COALESCE(MAX(entry), 0) + 1 FROM creature_template")
          || !allocate.next() || !allocate.value(0).toUInt())
      {
        failure = "Could not allocate a unique creature template ID.";
        unlock();
        return false;
      }
      new_entry = allocate.value(0).toUInt();
      allocate.finish();

      QSqlQuery columns(db);
      if (!columns.exec("SHOW COLUMNS FROM creature_template"))
      {
        failure = columns.lastError().text();
        unlock();
        return false;
      }
      QStringList names;
      while (columns.next())
      {
        QString const name = columns.value(0).toString();
        if (name.compare("entry", Qt::CaseInsensitive) == 0
            || columns.value(5).toString().contains("auto_increment", Qt::CaseInsensitive))
          continue;
        names.push_back("`" + QString(name).replace('`', "``") + "`");
      }
      columns.finish();
      QSqlQuery clone(db);
      clone.prepare(QString("INSERT INTO creature_template (`entry`, %1) "
                            "SELECT ?, %1 FROM creature_template WHERE entry = ?")
                      .arg(names.join(", ")));
      clone.addBindValue(new_entry);
      clone.addBindValue(source_entry);
      if (!clone.exec() || clone.numRowsAffected() != 1)
      {
        failure = "Could not copy the creature template: " + clone.lastError().text();
        unlock();
        return false;
      }
      template_created = true;
      clone.finish();

       QSqlQuery edit(db);
       edit.prepare("UPDATE creature_template SET name = ?, subname = ?, minlevel = ?, "
                    "maxlevel = ?, faction = ? WHERE entry = ?");
      edit.addBindValue(_edit_name->text().trimmed());
      edit.addBindValue(_edit_subname->text().trimmed());
      edit.addBindValue(_edit_minlevel->value());
      edit.addBindValue(_edit_maxlevel->value());
      edit.addBindValue(_edit_faction->factionId());
       edit.addBindValue(new_entry);
      if (!edit.exec())
      {
        failure = "Could not edit the unique template: " + edit.lastError().text();
        return fail();
       }
       edit.finish();
       QString model_error;
       if (!Noggit::Sql::replaceTemplateDisplay(db, schema, new_entry, display, model_error))
       {
         failure = model_error;
         return fail();
       }
       model_created = schema.uses_creature_template_model;

      for (RelatedTable const& table : related)
      {
        if (!tableExists(db, table.table)) continue;
        QSqlQuery fields(db);
        if (!fields.exec(QString("SHOW COLUMNS FROM `%1`").arg(table.table)))
        {
          failure = QString("Could not inspect %1: %2")
            .arg(table.table, fields.lastError().text());
          return fail();
        }
        QStringList other_columns;
        bool has_key = false;
        while (fields.next())
        {
          QString const column = fields.value(0).toString();
          if (column.compare(table.key, Qt::CaseInsensitive) == 0)
          {
            has_key = true;
            continue;
          }
          if (fields.value(5).toString().contains("auto_increment", Qt::CaseInsensitive))
            continue;
          other_columns.push_back("`" + QString(column).replace('`', "``") + "`");
        }
        fields.finish();
        if (!has_key) continue;
        QString const extra = other_columns.isEmpty() ? QString()
          : ", " + other_columns.join(", ");
        QSqlQuery copy(db);
        copy.prepare(QString("INSERT INTO `%1` (`%2`%3) SELECT ?%3 FROM `%1` "
                             "WHERE `%2` = ?%4")
          .arg(table.table, table.key, extra,
               table.smart ? " AND `source_type` = 0" : ""));
        copy.addBindValue(new_entry);
        copy.addBindValue(source_entry);
        if (!copy.exec())
        {
          failure = QString("Could not copy %1: %2").arg(table.table, copy.lastError().text());
          return fail();
        }
        copied.push_back(table);
      }

       QSqlQuery attach(db);
       QString attach_sql = QString("UPDATE creature SET %1 = ?")
         .arg(schema.creatureEntry());
       if (schema.creature_entry_column == "id1") attach_sql += ", `id2` = 0, `id3` = 0";
       if (!schema.creature_spawn_display_column.isEmpty())
         attach_sql += ", " + schema.creatureSpawnDisplay() + " = 0";
       attach_sql += QString(" WHERE guid = ? AND %1 = ?").arg(schema.creatureEntry());
       attach.prepare(attach_sql);
      attach.addBindValue(new_entry);
      attach.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      attach.addBindValue(source_entry);
      if (!attach.exec() || attach.numRowsAffected() != 1)
      {
        failure = "Could not assign the unique template to this spawn: "
          + attach.lastError().text();
        return fail();
      }
      unlock();
      return true;
    }, error);
  if (!saved)
  {
    _status->setText("Could not create a unique NPC template: " + error);
    return;
  }
  _selected_spawn_entry = new_entry;
  _template_spawn_count = 1;
  _speed_usage->setText(
    "This template is used by one placed NPC; speed changes affect only this spawn.");
  _edit_display->setValue(0);
  _spawn_identity->setText(QString("GUID %1 · Unique template %2")
    .arg(*_selected_spawn_guid).arg(new_entry));
  if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
    overlay->setIdentity(*_selected_spawn_guid, new_entry);
  bool const shown = refreshSpawnAppearance(display, new_entry);
  updateCachedSpawn(*_selected_spawn_guid, new_entry, _edit_spawn_position,
                    display, static_cast<float>(_edit_yaw->value()));
  commitNpcEdits(EditTemplate | EditSpawn);
  _status->setText(shown
    ? QString("Unique template %1 created for this NPC. Reload the world server to see it in game.").arg(new_entry)
    : QString("Unique template %1 was created, but Noggit could not refresh its display.").arg(new_entry));
}

void NpcTemplateBrowser::saveWaypoints()
{
  if (!_selected_spawn_guid) return;
  if (_waypoints.size() < 2)
  {
    _status->setText("Add at least two waypoints before saving a path.");
    return;
  }
  // Surface-conformance samples exist solely to make live Noggit playback
  // follow visible terrain and WMO floors. Saving them loses the distinction
  // between authored nodes and preview samples and causes paths to multiply on
  // every edit/save cycle. Persist authored patrol nodes only (plus the
  // intentional generated return half required by retrace behavior).
  std::vector<WaypointDraft> const saved_waypoints = patrolWaypoints();
  std::size_t const action_count = static_cast<std::size_t>(std::count_if(
    saved_waypoints.begin(), saved_waypoints.end(),
    [](WaypointDraft const& point) { return point.emote_id != 0; }));
  QString error;
  unsigned new_path_id = 0;
  bool const saved = useWorldDatabase(
    _host->text(), _port->value(), _database->text(), _user->text(), _password->text(),
    true, [&](QSqlDatabase& db, QString& failure)
    {
      if (!tableExists(db, "waypoint_data") || !tableExists(db, "creature_addon"))
      {
        failure = "The world database needs waypoint_data and creature_addon tables.";
        return false;
      }
      if (action_count && !tableExists(db, "waypoint_scripts"))
      {
        failure = "Arrival animations require the waypoint_scripts table.";
        return false;
      }
      QString const lock_name = QString("noggit_npc_waypoint_%1")
        .arg(_database->text().trimmed()).left(64);
      QSqlQuery lock(db);
      lock.prepare("SELECT GET_LOCK(?, 5)");
      lock.addBindValue(lock_name);
      if (!lock.exec() || !lock.next() || lock.value(0).toInt() != 1)
      {
        failure = "Could not acquire the waypoint path allocation lock.";
        return false;
      }
      lock.finish();
      auto unlock = [&]
      {
        QSqlQuery release(db);
        release.prepare("SELECT RELEASE_LOCK(?)");
        release.addBindValue(lock_name);
        release.exec();
      };
      QSqlQuery previous_addon(db);
      previous_addon.prepare("SELECT path_id FROM creature_addon WHERE guid = ? LIMIT 1");
      previous_addon.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      if (!previous_addon.exec())
      {
        failure = previous_addon.lastError().text();
        unlock();
        return false;
      }
      bool const had_addon = previous_addon.next();
      previous_addon.finish();

      QSqlQuery allocate(db);
      if (!allocate.exec("SELECT COALESCE(MAX(id), 0) + 1 FROM waypoint_data")
          || !allocate.next() || !allocate.value(0).toUInt())
      {
        failure = "Could not allocate a waypoint path ID.";
        unlock();
        return false;
      }
      new_path_id = allocate.value(0).toUInt();
      allocate.finish();

      unsigned next_action_id = 0;
      unsigned next_action_guid = 0;
      if (action_count)
      {
        QSqlQuery allocate_action(db);
        if (!allocate_action.exec("SELECT COALESCE(MAX(id), 0) + 1, "
                                  "COALESCE(MAX(guid), 0) + 1 FROM waypoint_scripts")
            || !allocate_action.next() || !allocate_action.value(0).toUInt()
            || !allocate_action.value(1).toUInt())
        {
          failure = "Could not allocate waypoint animation script IDs.";
          unlock();
          return false;
        }
        next_action_id = allocate_action.value(0).toUInt();
        next_action_guid = allocate_action.value(1).toUInt();
      }

      // Match spawn deletion: the deployed Qt MySQL driver can report that
      // QSqlDatabase transactions are unsupported while the server itself
      // accepts transaction commands normally.
      QSqlQuery transaction(db);
      if (!transaction.exec("START TRANSACTION"))
      {
        failure = "Could not start the waypoint save transaction: "
          + transaction.lastError().text();
        unlock();
        return false;
      }
      auto fail_transaction = [&](QString const& message)
      {
        failure = message;
        QSqlQuery rollback(db);
        rollback.exec("ROLLBACK");
        unlock();
        return false;
      };

      struct ScriptRow
      {
        unsigned id;
        unsigned delay;
        unsigned emote;
        unsigned guid;
      };
      std::vector<ScriptRow> script_rows;
      script_rows.reserve(action_count * 2);
      std::vector<unsigned> waypoint_action_ids(saved_waypoints.size(), 0);
      for (std::size_t i = 0; i < saved_waypoints.size(); ++i)
      {
        WaypointDraft const& point = saved_waypoints[i];
        if (point.emote_id)
        {
          unsigned const action_id = next_action_id++;
          waypoint_action_ids[i] = action_id;
          script_rows.push_back({action_id, 0, point.emote_id, next_action_guid++});
          if (point.delay_ms)
            script_rows.push_back({action_id,
              std::max(1u, (point.delay_ms + 999u) / 1000u), 0, next_action_guid++});
        }
      }

      // Multi-row inserts keep a long path to a handful of database round
      // trips. The transaction also avoids forcing a disk commit per point.
      constexpr std::size_t batch_size = 250;
      for (std::size_t first = 0; first < script_rows.size(); first += batch_size)
      {
        std::size_t const last = std::min(first + batch_size, script_rows.size());
        QStringList values;
        for (std::size_t row = first; row < last; ++row)
          values.push_back("(?, ?, 1, ?, 1, 0, 0, 0, 0, 0, ?)");
        QSqlQuery insert(db);
        insert.prepare("INSERT INTO waypoint_scripts "
                       "(id, delay, command, datalong, datalong2, dataint, x, y, z, o, guid) VALUES "
                       + values.join(", "));
        for (std::size_t row = first; row < last; ++row)
        {
          insert.addBindValue(script_rows[row].id);
          insert.addBindValue(script_rows[row].delay);
          insert.addBindValue(script_rows[row].emote);
          insert.addBindValue(script_rows[row].guid);
        }
        if (!insert.exec())
          return fail_transaction("Could not save waypoint arrival animations: "
                                  + insert.lastError().text());
      }

      for (std::size_t first = 0; first < saved_waypoints.size(); first += batch_size)
      {
        std::size_t const last = std::min(first + batch_size, saved_waypoints.size());
        QStringList values;
        for (std::size_t row = first; row < last; ++row)
          values.push_back("(?, ?, ?, ?, ?, 0, ?, ?, ?, 100, 0)");
        QSqlQuery insert(db);
        insert.prepare("INSERT INTO waypoint_data "
                       "(id, point, position_x, position_y, position_z, orientation, "
                       "delay, move_type, action, action_chance, wpguid) VALUES "
                       + values.join(", "));
        for (std::size_t row = first; row < last; ++row)
        {
          WaypointDraft const& point = saved_waypoints[row];
          insert.addBindValue(new_path_id);
          insert.addBindValue(static_cast<unsigned>(row + 1));
          insert.addBindValue(ZEROPOINT - point.position.z);
          insert.addBindValue(ZEROPOINT - point.position.x);
          insert.addBindValue(point.position.y);
          insert.addBindValue(point.delay_ms);
          insert.addBindValue(point.run ? 1 : 0);
          insert.addBindValue(waypoint_action_ids[row]);
        }
        if (!insert.exec())
          return fail_transaction("Could not save waypoint batch: " + insert.lastError().text());
      }

      QSqlQuery attach(db);
      if (had_addon)
      {
        attach.prepare("UPDATE creature_addon SET path_id = ? WHERE guid = ?");
        attach.addBindValue(new_path_id);
        attach.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
      }
      else
      {
        attach.prepare("INSERT INTO creature_addon (guid, path_id) VALUES (?, ?)");
        attach.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
        attach.addBindValue(new_path_id);
      }
      if (!attach.exec())
        return fail_transaction("Could not link the path to this NPC: "
                                + attach.lastError().text());
      attach.finish();

       Noggit::Sql::WorldDatabaseSchema schema;
       Noggit::Sql::inspectWorldDatabaseSchema(db, schema, failure);
       if (failure.isEmpty())
       {
         QSqlQuery movement(db);
         movement.prepare(QString("UPDATE creature SET MovementType = 2, %1 = 0 "
                                  "WHERE guid = ? AND %2 = ?")
                            .arg(schema.creatureRadius(), schema.creatureEntry()));
        movement.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
        movement.addBindValue(_selected_spawn_entry);
        if (!movement.exec())
          failure = "Could not enable waypoint movement: " + movement.lastError().text();
      }
      if (failure.isEmpty())
      {
        QSqlQuery verify(db);
         verify.prepare(QString(
           "SELECT c.MovementType, %1, ca.path_id, "
           "(SELECT COUNT(*) FROM waypoint_data wd WHERE wd.id = ca.path_id) "
           "FROM creature c JOIN creature_addon ca ON ca.guid = c.guid "
           "WHERE c.guid = ? AND %2 = ? LIMIT 1")
           .arg(schema.creatureRadius("c"), schema.creatureEntry("c")));
        verify.addBindValue(QVariant::fromValue<qulonglong>(*_selected_spawn_guid));
        verify.addBindValue(_selected_spawn_entry);
        if (!verify.exec() || !verify.next()
            || verify.value(0).toInt() != 2
            || std::abs(verify.value(1).toDouble()) > 0.001
            || verify.value(2).toUInt() != new_path_id
            || verify.value(3).toUInt() != saved_waypoints.size())
        {
          failure = verify.lastError().isValid()
            ? "Could not verify the saved waypoint path: " + verify.lastError().text()
            : "The database did not retain the complete waypoint path.";
        }
      }
      if (!failure.isEmpty())
        return fail_transaction(failure);
      QSqlQuery commit(db);
      if (!commit.exec("COMMIT"))
        return fail_transaction("Could not commit the waypoint path: "
                                + commit.lastError().text());
      unlock();
      return true;
    }, error);
  if (!saved)
  {
    _status->setText("Could not save NPC waypoints: " + error);
    return;
  }
  _edit_movement->setCurrentIndex(_edit_movement->findData(2));
  _edit_wander->setValue(0.0);
  _capturing_waypoints = false;
  _capture_waypoint_button->setText("Add points by clicking terrain");
  commitNpcEdits(EditPath | EditSpawn);
  updateMovementPreview();
  if (saved_waypoints.size() == _waypoints.size())
    _status->setText(QString("Saved %1 waypoints with %2 arrival action(s) as path %3 for GUID %4. "
                             "Reload the world server to use the path in game.")
                       .arg(_waypoints.size()).arg(action_count).arg(new_path_id)
                       .arg(*_selected_spawn_guid));
  else
    _status->setText(QString("Saved %1 authored waypoints with %2 arrival action(s) as %3 "
                             "retrace path nodes for GUID %4. Reload the world server to use "
                             "path %5 in game.")
                       .arg(_waypoints.size()).arg(action_count).arg(saved_waypoints.size())
                       .arg(*_selected_spawn_guid).arg(new_path_id));
}
}
