#include <noggit/ui/NpcSoundEditor.hpp>

#include <noggit/DBC.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/ui/windows/EditorWindows/SoundEntryPickerWindow.h>
#include <noggit/ui/windows/SoundPlayer/SoundEntryPlayer.h>

#include <QHBoxLayout>
#include <QComboBox>
#include <QCompleter>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <map>

namespace Noggit::Ui
{
namespace
{
  enum RowKind { VoiceRow, ActionRow, FootstepSetRow, FootstepSoundRow };

  struct ActionSlot { unsigned field; char const* name; };
  constexpr ActionSlot action_slots[] = {
    {1, "Exertion"}, {2, "Critical exertion"}, {3, "Injury"},
    {4, "Critical injury"}, {5, "Crushing injury"}, {6, "Death"},
    {7, "Stun"}, {8, "Stand"}, {10, "Aggro"},
    {11, "Wing flap"}, {12, "Wing glide"}, {13, "Alert"},
    {14, "Fidget 1"}, {15, "Fidget 2"}, {16, "Fidget 3"},
    {17, "Fidget 4"}, {18, "Fidget 5"},
    {19, "Custom attack 1"}, {20, "Custom attack 2"},
    {21, "Custom attack 3"}, {22, "Custom attack 4"},
    {24, "Loop"}, {26, "Jump start"}, {27, "Jump end"},
    {28, "Pet attack"}, {29, "Pet order"}, {30, "Pet dismiss"},
    {33, "Birth"}, {34, "Directed spell cast"},
    {35, "Submerge"}, {36, "Submerged"}
  };

  QString soundName(unsigned id)
  {
    if (!id) return "None";
    if (!gSoundEntriesDB.CheckIfIdExists(id)) return "Missing SoundEntries row";
    return QString::fromStdString(gSoundEntriesDB.getByID(id).getString(SoundEntriesDB::Name));
  }
}

NpcSoundEditor::NpcSoundEditor(DBCFile* displays, DBCFile* models,
                               DBCFile* voices, DBCFile* actions,
                               DBCFile* footstep_terrain, QWidget* parent)
  : QWidget(parent), _displays(displays), _models(models), _voices(voices), _actions(actions),
    _footstep_terrain(footstep_terrain)
{
  auto* layout = new QVBoxLayout(this);
  auto* note = new QLabel(
    "Choose a named preset or customize individual client sounds for this NPC. "
    "Saving uses a private display and keeps shared sound records intact.", this);
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* preset_row = new QHBoxLayout();
  preset_row->addWidget(new QLabel("Sound preset:", this));
  _presets = new QComboBox(this);
  _presets->setEditable(true);
  _presets->setInsertPolicy(QComboBox::NoInsert);
  _presets->setMinimumWidth(280);
  _presets->setToolTip("Choose a named sound set. Individual sounds remain editable below.");
  _presets->completer()->setFilterMode(Qt::MatchContains);
  _presets->completer()->setCompletionMode(QCompleter::PopupCompletion);
  preset_row->addWidget(_presets, 1);
  layout->addLayout(preset_row);
  _preset_status = new QLabel(this);
  _preset_status->setWordWrap(true);
  layout->addWidget(_preset_status);
  _tree = new QTreeWidget(this);
  _tree->setColumnCount(3);
  _tree->setHeaderLabels({"Action / voice", "Assignment / SoundEntries ID and name", "Source"});
  _tree->setRootIsDecorated(true);
  _tree->setAlternatingRowColors(false);
  _tree->setColumnWidth(0, 210);
  _tree->setColumnWidth(1, 290);
  layout->addWidget(_tree, 1);

  auto* controls = new QHBoxLayout();
  _sound_id = new QSpinBox(this);
  _sound_id->setRange(0, std::numeric_limits<int>::max());
  _sound_id->setSpecialValueText("None");
  _choose = new QPushButton("Browse SoundEntries...", this);
  _clear = new QPushButton("Clear", this);
  _revert = new QPushButton("Revert to preset", this);
  _play = new QPushButton("Play", this);
  _selected_label = new QLabel("Selected sound:", this);
  controls->addWidget(_selected_label);
  controls->addWidget(_sound_id);
  controls->addWidget(_choose);
  controls->addWidget(_clear);
  controls->addWidget(_revert);
  controls->addWidget(_play);
  controls->addStretch();
  layout->addLayout(controls);
  _details = new QLabel(this);
  _details->setWordWrap(true);
  layout->addWidget(_details);

  connect(_tree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem*, QTreeWidgetItem*) { refreshSelection(); });
  connect(_sound_id, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int id) { setSelectedSound(static_cast<unsigned>(id)); });
  connect(_presets, QOverload<int>::of(&QComboBox::activated), this,
          [this](int index) { selectPreset(index); });
  connect(_clear, &QPushButton::clicked, this, [this] { _sound_id->setValue(0); });
  connect(_revert, &QPushButton::clicked, this, [this]
  {
    auto* item = _tree->currentItem();
    if (!item) return;
    unsigned const index = item->data(0, Qt::UserRole + 1).toUInt();
    int const kind = item->data(0, Qt::UserRole).toInt();
    if (kind == VoiceRow) setSelectedSound(_preset_voice[index]);
    else if (kind == ActionRow) setSelectedSound(_preset_action[index]);
  });
  connect(_choose, &QPushButton::clicked, this, [this]
  {
    auto* item = _tree->currentItem();
    if (!item || item->data(0, Qt::UserRole).toInt() > ActionRow) return;
    _tree->setEnabled(false);
    auto* target = new QPushButton(this);
    target->hide();
    target->setProperty("id", _sound_id->value());
    auto* picker = new SoundEntryPickerWindow(target, -1, true, this, true);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &QObject::destroyed, this, [this, target]
    {
      _tree->setEnabled(true);
      _sound_id->setValue(target->property("id").toInt());
      target->deleteLater();
    });
    picker->show();
  });
  connect(_play, &QPushButton::clicked, this, [this]
  {
    unsigned const id = static_cast<unsigned>(_sound_id->value());
    if (!id || !gSoundEntriesDB.CheckIfIdExists(id)) return;
    auto const entry = gSoundEntriesDB.getByID(id);
    bool has_file = false;
    for (unsigned file = 0; file < 10; ++file)
      has_file |= *entry.getString(SoundEntriesDB::Filenames + file) != '\0';
    if (!has_file) return;
    auto* player = new SoundEntryPlayer(this);
    player->setAttribute(Qt::WA_DeleteOnClose);
    player->setWindowFlag(Qt::Window);
    player->LoadSoundsFromSoundEntry(static_cast<int>(id));
    player->show();
  });
  buildPresets();
  refreshSelection();
}

void NpcSoundEditor::buildPresets()
{
  _preset_catalog.clear();
  _presets->clear();
  _presets->addItem("Current sounds");
  _presets->setEnabled(false);
  if (!_displays || !_models || !_actions || !_voices) return;

  std::map<std::pair<unsigned, unsigned>, QString> combinations;
  for (std::size_t index = 0; index < _displays->getRecordCount(); ++index)
  {
    try
    {
      auto const display = _displays->getRecord(index);
      unsigned const model_id = display.getUInt(1);
      unsigned action_id = display.getUInt(2);
      if (!action_id && model_id && _models->CheckIfIdExists(model_id))
        action_id = _models->getByID(model_id).getUInt(13);
      unsigned voice_id = display.getUInt(12);
      if (!voice_id && action_id && _actions->CheckIfIdExists(action_id))
        voice_id = _actions->getByID(action_id).getUInt(23);
      if ((!action_id && !voice_id)
          || (action_id && !_actions->CheckIfIdExists(action_id))
          || (voice_id && !_voices->CheckIfIdExists(voice_id)))
        continue;
      auto const key = std::pair{action_id, voice_id};
      if (combinations.contains(key)) continue;

      QString name;
      if (model_id && _models->CheckIfIdExists(model_id))
      {
        QString path = QString::fromStdString(_models->getByID(model_id).getString(2));
        path.replace('\\', '/');
        name = QFileInfo(path).completeBaseName();
        name.replace('_', ' ');
        name.replace(QRegularExpression("([a-z])([A-Z])"), "\\1 \\2");
        name.remove(QRegularExpression("[ -]*[0-9]+$"));
        name = name.simplified();
      }
      if (name.isEmpty()) continue;
      combinations.emplace(key, name);
    }
    catch (std::exception const&) {}
  }

  QHash<QString, int> variants;
  for (auto const& [ids, base_name] : combinations)
  {
    int const variant = ++variants[base_name.toCaseFolded()];
    QString const name = variant == 1 ? base_name
      : QString("%1 · Variant %2").arg(base_name).arg(variant);
    _preset_catalog.push_back({name, ids.first, ids.second});
  }
  std::sort(_preset_catalog.begin(), _preset_catalog.end(),
            [](SoundPreset const& left, SoundPreset const& right)
            { return QString::localeAwareCompare(left.name, right.name) < 0; });
  for (SoundPreset const& preset : _preset_catalog)
    _presets->addItem(preset.name);
  _presets->setEnabled(!_preset_catalog.empty());
}

void NpcSoundEditor::selectPreset(int index)
{
  if (index < 0 || index > static_cast<int>(_preset_catalog.size())) return;
  if (index != _active_preset_index
      && (voiceOverridesChanged() || actionOverridesChanged())
      && QMessageBox::question(this, "Replace customized sounds",
           "Choosing another preset replaces the individual sound changes staged here. Continue?")
           != QMessageBox::Yes)
  {
    QSignalBlocker const blocker(_presets);
    _presets->setCurrentIndex(_active_preset_index);
    return;
  }
  if (!index)
  {
    _selected_action_base_id = _action_base_id;
    _selected_voice_base_id = _voice_base_id;
    _action = _original_action;
    _voice = _original_voice;
    _action_source = _original_action_source;
    _voice_source = _original_voice_source;
  }
  else
  {
    SoundPreset const& preset = _preset_catalog[index - 1];
    _selected_action_base_id = preset.action_id;
    _selected_voice_base_id = preset.voice_id;
    _action.fill(0);
    _voice.fill(0);
    if (_selected_action_base_id)
    {
      auto const row = _actions->getByID(_selected_action_base_id);
      for (ActionSlot const& slot : action_slots) _action[slot.field] = row.getUInt(slot.field);
      _action[9] = row.getUInt(9);
    }
    if (_selected_voice_base_id)
    {
      auto const row = _voices->getByID(_selected_voice_base_id);
      for (unsigned slot = 0; slot < 4; ++slot) _voice[slot] = row.getUInt(slot + 1);
    }
    _action_source = "Preset: " + preset.name;
    _voice_source = _action_source;
  }
  _preset_action = _action;
  _preset_voice = _voice;
  _active_preset_index = index;
  refreshRows();
}

void NpcSoundEditor::refreshPresetStatus()
{
  if (_presets->currentIndex() > 0)
  {
    QString const name = _preset_catalog[_presets->currentIndex() - 1].name;
    _preset_status->setText(voiceOverridesChanged() || actionOverridesChanged()
      ? "Customized from " + name + ". Changed rows are marked below."
      : "Selected preset: " + name + ". Select a row to play or customize it.");
  }
  else
    _preset_status->setText(voiceOverridesChanged() || actionOverridesChanged()
      ? "Custom sound assignments. Changed rows are marked below."
      : "Current display sound assignments. Choose a preset or edit a sound below.");
}

void NpcSoundEditor::setDisplay(unsigned display_id)
{
  _display_id = display_id;
  _voice.fill(0);
  _action.fill(0);
  _voice_base_id = 0;
  _action_base_id = 0;
  _voice_source = "None";
  _action_source = "None";
  unsigned model_id = 0;
  try
  {
    if (!_displays || _displays->getFieldCount() < 16 || !display_id)
      throw DBCFile::NotFound();
    auto const display = _displays->getByID(display_id);
    model_id = display.getUInt(1);
    _action_base_id = display.getUInt(2);
    _voice_base_id = display.getUInt(12);
    if (_action_base_id)
      _action_source = QString("Display %1 / CreatureSoundData %2")
        .arg(display_id).arg(_action_base_id);
    if (_voice_base_id)
      _voice_source = QString("Display %1 / NPCSounds %2")
        .arg(display_id).arg(_voice_base_id);
  }
  catch (std::exception const&)
  {
    _voice_source = "Missing display";
    _action_source = "Missing display";
  }
  if (!_action_base_id && _models && _models->getFieldCount() > 13 && model_id)
  {
    try
    {
      _action_base_id = _models->getByID(model_id).getUInt(13);
      if (_action_base_id)
        _action_source = QString("Model %1 / CreatureSoundData %2")
          .arg(model_id).arg(_action_base_id);
    }
    catch (std::exception const&) { _action_source = "Missing creature model"; }
  }
  if (!_voice_base_id && _actions && _actions->getFieldCount() > 23 && _action_base_id)
  {
    try
    {
      _voice_base_id = _actions->getByID(_action_base_id).getUInt(23);
      if (_voice_base_id)
        _voice_source = QString("CreatureSoundData %1 / NPCSounds %2")
          .arg(_action_base_id).arg(_voice_base_id);
    }
    catch (std::exception const&) { _voice_source = "Missing creature sound data"; }
  }
  try
  {
    if (_voices && _voices->getFieldCount() >= 5 && _voice_base_id)
    {
      auto const row = _voices->getByID(_voice_base_id);
      for (unsigned slot = 0; slot < 4; ++slot) _voice[slot] = row.getUInt(slot + 1);
    }
  }
  catch (std::exception const&) { _voice_source += " (missing row)"; }
  try
  {
    if (_actions && _actions->getFieldCount() >= 38 && _action_base_id)
    {
      auto const row = _actions->getByID(_action_base_id);
      for (ActionSlot const& slot : action_slots) _action[slot.field] = row.getUInt(slot.field);
      _action[9] = row.getUInt(9);
    }
  }
  catch (std::exception const&) { _action_source += " (missing row)"; }
  _original_voice = _voice;
  _original_action = _action;
  _preset_voice = _voice;
  _preset_action = _action;
  _selected_voice_base_id = _voice_base_id;
  _selected_action_base_id = _action_base_id;
  _original_voice_source = _voice_source;
  _original_action_source = _action_source;
  int selected_index = 0;
  for (std::size_t index = 0; index < _preset_catalog.size(); ++index)
    if (_preset_catalog[index].action_id == _action_base_id
        && _preset_catalog[index].voice_id == _voice_base_id)
    {
      selected_index = static_cast<int>(index) + 1;
      break;
    }
  _presets->setCurrentIndex(selected_index);
  _active_preset_index = selected_index;
  refreshRows();
}

bool NpcSoundEditor::voiceChanged() const
{
  return _voice != _original_voice || _selected_voice_base_id != _voice_base_id;
}
bool NpcSoundEditor::actionChanged() const
{
  return _action != _original_action || _selected_action_base_id != _action_base_id;
}
bool NpcSoundEditor::voiceOverridesChanged() const { return _voice != _preset_voice; }
bool NpcSoundEditor::actionOverridesChanged() const { return _action != _preset_action; }
bool NpcSoundEditor::voiceSlotOverridden(unsigned slot) const
{
  return _voice[slot] != _preset_voice[slot];
}
bool NpcSoundEditor::actionSlotOverridden(unsigned field) const
{
  return _action[field] != _preset_action[field];
}
bool NpcSoundEditor::voiceSlotChanged(unsigned slot) const
{
  return _voice[slot] != _original_voice[slot];
}
bool NpcSoundEditor::actionSlotChanged(unsigned field) const
{
  return _action[field] != _original_action[field];
}

void NpcSoundEditor::refreshRows()
{
  _tree->clear();
  static char const* voice_names[] = {"Greeting", "Farewell", "Annoyed", "Voice slot 4"};
  for (unsigned slot = 0; slot < 4; ++slot)
  {
    auto* item = new QTreeWidgetItem(_tree);
    item->setText(0, voice_names[slot]);
    item->setData(0, Qt::UserRole, VoiceRow);
    item->setData(0, Qt::UserRole + 1, slot);
    item->setText(1, QString("%1 — %2").arg(_voice[slot]).arg(soundName(_voice[slot])));
    item->setText(2, voiceSlotOverridden(slot) ? "Customized" : _voice_source);
  }

  auto terrain_label = [](unsigned terrain_sound_id)
  {
    QStringList names;
    for (auto terrain = gTerrainTypeDB.begin(); terrain != gTerrainTypeDB.end(); ++terrain)
    {
      if (terrain->getUInt(TerrainTypeDB::Sound) != terrain_sound_id) continue;
      QString const name = QString::fromUtf8(terrain->getString(TerrainTypeDB::TerrainDesc));
      if (!name.isEmpty() && !names.contains(name)) names.push_back(name);
    }
    QString label = QString("Terrain sound %1").arg(terrain_sound_id);
    if (!names.isEmpty()) label += QString(" — %1").arg(names.join(", "));
    return label;
  };

  auto add_footstep_set = [this, &terrain_label]()
  {
    unsigned const set_id = _action[9];
    auto* root = new QTreeWidgetItem(_tree);
    root->setText(0, "Footsteps");
    root->setData(0, Qt::UserRole, FootstepSetRow);
    root->setData(0, Qt::UserRole + 1, set_id);
    root->setText(2, _action_source);

    unsigned mappings = 0;
    if (set_id && _footstep_terrain)
    {
      for (std::size_t row_index = 0; row_index < _footstep_terrain->getRecordCount(); ++row_index)
      {
        auto const lookup = _footstep_terrain->getRecord(row_index);
        if (lookup.getUInt(1) != set_id) continue;
        ++mappings;

        unsigned const lookup_id = lookup.getUInt(0);
        unsigned const terrain_sound_id = lookup.getUInt(2);
        QString const terrain = terrain_label(terrain_sound_id);
        for (unsigned field : {3u, 4u})
        {
          unsigned const sound_id = lookup.getUInt(field);
          if (!sound_id) continue;
          bool const splash = field == 4;
          auto* sound = new QTreeWidgetItem(root);
          sound->setText(0, splash ? terrain + " (splash)" : terrain);
          sound->setText(1, QString("%1 — %2").arg(sound_id).arg(soundName(sound_id)));
          sound->setText(2, QString("FootstepTerrainLookup %1").arg(lookup_id));
          sound->setData(0, Qt::UserRole, FootstepSoundRow);
          sound->setData(0, Qt::UserRole + 1, sound_id);
          sound->setData(0, Qt::UserRole + 2,
            QString("Footstep set %1; %2%3.")
              .arg(set_id).arg(terrain, splash ? "; splash sound" : "; normal sound"));
        }
      }
    }

    if (!set_id) root->setText(1, "0 — None");
    else if (!_footstep_terrain)
      root->setText(1, QString("Set %1 — lookup DBC unavailable").arg(set_id));
    else
      root->setText(1, QString("Set %1 — %2 terrain mapping%3")
        .arg(set_id).arg(mappings).arg(mappings == 1 ? "" : "s"));
    root->setExpanded(true);
  };

  for (ActionSlot const& slot : action_slots)
  {
    auto* item = new QTreeWidgetItem(_tree);
    item->setText(0, slot.name);
    item->setData(0, Qt::UserRole, ActionRow);
    item->setData(0, Qt::UserRole + 1, slot.field);
    item->setText(1, QString("%1 — %2").arg(_action[slot.field])
                       .arg(soundName(_action[slot.field])));
    item->setText(2, actionSlotOverridden(slot.field) ? "Customized" : _action_source);
    if (slot.field == 8) add_footstep_set();
  }
  if (_tree->topLevelItemCount()) _tree->setCurrentItem(_tree->topLevelItem(0));
  refreshPresetStatus();
  refreshSelection();
}

void NpcSoundEditor::refreshSelection()
{
  auto* item = _tree->currentItem();
  bool const selected = item != nullptr;
  int const kind = selected ? item->data(0, Qt::UserRole).toInt() : -1;
  bool const editable = kind == VoiceRow || kind == ActionRow;
  _selected_label->setText(kind == FootstepSetRow ? "Footstep set:"
    : kind == FootstepSoundRow ? "Resolved sound:" : "Selected sound:");
  _sound_id->setEnabled(editable);
  _choose->setEnabled(editable);
  _clear->setEnabled(editable);
  _revert->setEnabled(kind == VoiceRow ? voiceSlotOverridden(item->data(0, Qt::UserRole + 1).toUInt())
    : kind == ActionRow && actionSlotOverridden(item->data(0, Qt::UserRole + 1).toUInt()));
  _play->setEnabled(selected && kind != FootstepSetRow);
  if (!selected) { _details->clear(); return; }

  unsigned const index = item->data(0, Qt::UserRole + 1).toUInt();
  unsigned id = index;
  if (kind == VoiceRow) id = _voice[index];
  else if (kind == ActionRow) id = _action[index];
  QSignalBlocker const blocker(_sound_id);
  _sound_id->setValue(static_cast<int>(id));

  if (kind == FootstepSetRow)
  {
    _play->setEnabled(false);
    if (!id) _details->setText("No terrain-dependent footstep set is assigned.");
    else if (!_footstep_terrain)
      _details->setText(QString("Footstep set %1 is assigned, but FootstepTerrainLookup.dbc "
                                "is unavailable in the configured client.").arg(id));
    else if (!item->childCount())
      _details->setText(QString("Footstep set %1 has no sound mappings in "
                                "FootstepTerrainLookup.dbc.").arg(id));
    else
      _details->setText(QString("Footstep set %1 selects different sounds according to the "
                                "terrain. Expand this row and select a resolved sound to preview it.")
                          .arg(id));
    return;
  }

  if (!id) { _details->setText("No sound assigned."); _play->setEnabled(false); return; }
  if (!gSoundEntriesDB.CheckIfIdExists(id))
  {
    _details->setText(QString("SoundEntries %1 is missing from the configured client.").arg(id));
    _play->setEnabled(false);
    return;
  }
  auto const entry = gSoundEntriesDB.getByID(id);
  QStringList files;
  QStringList missing;
  QString const folder = QString::fromUtf8(entry.getString(SoundEntriesDB::FilePath));
  for (unsigned file = 0; file < 10; ++file)
  {
    QString const name = QString::fromUtf8(entry.getString(SoundEntriesDB::Filenames + file));
    if (name.isEmpty()) continue;
    files.push_back(name);
    std::string const path = (folder + "\\" + name).toStdString();
    auto const client = Noggit::Application::NoggitApplication::instance()->clientData();
    if (client && !client->exists(path)) missing.push_back(name);
  }
  QString const context = kind == FootstepSoundRow
    ? item->data(0, Qt::UserRole + 2).toString() + "   " : QString();
  _details->setText(QString("%1Folder: %2   Files: %3%4")
    .arg(context, folder, files.isEmpty() ? "None" : files.join(", "),
         missing.isEmpty() ? QString() : "   Missing: " + missing.join(", ")));
  _play->setEnabled(!files.isEmpty() && missing.size() < files.size());
}

void NpcSoundEditor::setSelectedSound(unsigned id)
{
  auto* item = _tree->currentItem();
  if (!item) return;
  int const kind = item->data(0, Qt::UserRole).toInt();
  if (kind != VoiceRow && kind != ActionRow) return;
  unsigned const index = item->data(0, Qt::UserRole + 1).toUInt();
  if (kind == VoiceRow) _voice[index] = id;
  else _action[index] = id;
  item->setText(1, QString("%1 — %2").arg(id).arg(soundName(id)));
  item->setText(2, kind == VoiceRow
    ? (voiceSlotOverridden(index) ? "Customized" : _voice_source)
    : (actionSlotOverridden(index) ? "Customized" : _action_source));
  refreshPresetStatus();
  refreshSelection();
}
}
