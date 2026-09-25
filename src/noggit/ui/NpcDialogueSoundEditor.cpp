#include <noggit/ui/NpcDialogueSoundEditor.hpp>

#include <noggit/DBC.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/ui/windows/EditorWindows/SoundEntryPickerWindow.h>
#include <noggit/ui/windows/SoundPlayer/SoundEntryPlayer.h>

#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <utility>

namespace Noggit::Ui
{
namespace
{
  QString soundLabel(unsigned id)
  {
    if (!id) return "0 — None";
    if (!gSoundEntriesDB.CheckIfIdExists(id))
      return QString("%1 — Missing SoundEntries row").arg(id);
    return QString("%1 — %2").arg(id).arg(
      QString::fromStdString(gSoundEntriesDB.getByID(id).getString(SoundEntriesDB::Name)));
  }

  bool soundHasPlayableFile(unsigned id)
  {
    if (!id || !gSoundEntriesDB.CheckIfIdExists(id)) return false;
    auto const entry = gSoundEntriesDB.getByID(id);
    for (unsigned file = 0; file < 10; ++file)
      if (*entry.getString(SoundEntriesDB::Filenames + file) != '\0') return true;
    return false;
  }

  NpcDialogueLineChange lineChange(NpcDialogueSoundRow const& row)
  {
    return {row.group_id, row.line_id, row.text, row.chat_type, row.language,
            row.probability, row.emote, row.duration, row.sound,
            row.broadcast_text_id, row.text_range, row.comment};
  }

  bool linePropertiesDiffer(NpcDialogueSoundRow const& left,
                            NpcDialogueSoundRow const& right)
  {
    return left.text != right.text || left.chat_type != right.chat_type
      || left.language != right.language || left.probability != right.probability
      || left.emote != right.emote || left.duration != right.duration
      || left.broadcast_text_id != right.broadcast_text_id
      || left.text_range != right.text_range || left.comment != right.comment;
  }
}

NpcDialogueSoundEditor::NpcDialogueSoundEditor(std::vector<NpcDialogueSoundRow> rows,
                                               NpcDialogueSoundContext context,
                                               QWidget* parent)
  : QWidget(parent)
  , _context(std::move(context))
{
  _rows.reserve(rows.size());
  for (auto& row : rows)
  {
    unsigned const original_sound = row.sound;
    NpcDialogueSoundRow original_row = row;
    _rows.push_back({std::move(row), std::move(original_row), original_sound, false});
  }

  auto* outer_layout = new QVBoxLayout(this);
  outer_layout->setContentsMargins(0, 0, 0, 0);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* content = new QWidget(scroll);
  auto* layout = new QVBoxLayout(content);
  scroll->setWidget(content);
  outer_layout->addWidget(scroll);
  auto* note = new QLabel(
    "These are stored creature_text lines copied to the new private NPC. A stored line does not "
    "play until server AI or a script calls its group. The world server then plays "
    "creature_text.Sound. A linked BroadcastText sound is a read-only reference.", this);
  note->setWordWrap(true);
  layout->addWidget(note);

  auto* context_note = new QLabel(this);
  context_note->setWordWrap(true);
  QStringList context_lines;
  context_lines.push_back(QString("Template %1 AI: %2%3")
    .arg(_context.source_entry)
    .arg(_context.ai_name.isEmpty() ? "None" : _context.ai_name)
    .arg(_context.script_name.isEmpty() ? QString()
      : QString("; C++ ScriptName: %1").arg(_context.script_name)));
  if (!_context.spawn_sources.isEmpty())
  {
    QStringList visible_sources = _context.spawn_sources.mid(0, 3);
    if (_context.spawn_sources.size() > visible_sources.size())
      visible_sources.push_back(QString("…and %1 more (hover to view all)")
        .arg(_context.spawn_sources.size() - visible_sources.size()));
    context_lines.push_back("Spawn sources:\n  • " + visible_sources.join("\n  • "));
    context_note->setToolTip(_context.spawn_sources.join("\n"));
  }
  context_note->setText(context_lines.join("\n"));
  layout->addWidget(context_note);

  _tree = new QTreeWidget(this);
  _tree->setColumnCount(6);
  _tree->setHeaderLabels(
    {"Set / line", "Dialogue", "First pick", "Trigger",
      "Played sound", "Broadcast reference"});
  _tree->setRootIsDecorated(false);
  _tree->setAlternatingRowColors(false);
  _tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  _tree->setColumnHidden(4, true);
  _tree->setColumnHidden(5, true);
  _tree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  _tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  _tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
  _tree->header()->setSectionResizeMode(3, QHeaderView::Fixed);
  _tree->setColumnWidth(0, 80);
  _tree->setColumnWidth(2, 70);
  _tree->setColumnWidth(3, 80);
  _tree->setMinimumHeight(220);
  layout->addWidget(_tree, 1);

  auto* controls = new QGridLayout();
  _sound_id = new QSpinBox(this);
  _sound_id->setRange(0, std::numeric_limits<int>::max());
  _sound_id->setSpecialValueText("None");
  _choose = new QPushButton("Browse SoundEntries...", this);
  _clear = new QPushButton("Clear", this);
  _play = new QPushButton("Play assigned", this);
  _play_broadcast = new QPushButton("Play broadcast reference", this);
  controls->addWidget(new QLabel("Assigned sound:", this), 0, 0);
  controls->addWidget(_sound_id, 0, 1);
  controls->addWidget(_choose, 0, 2, 1, 2);
  controls->addWidget(_clear, 1, 0);
  controls->addWidget(_play, 1, 1);
  controls->addWidget(_play_broadcast, 1, 2, 1, 2);
  layout->addLayout(controls);

  auto* dialogue_box = new QGroupBox("Dialogue line editor", this);
  auto* dialogue_layout = new QGridLayout(dialogue_box);
  _new_text = new QLineEdit(dialogue_box);
  _new_text->setPlaceholderText("Dialogue spoken by this NPC");
  _new_group_id = new QSpinBox(dialogue_box);
  _new_group_id->setRange(0, 255);
  _new_line_id = new QSpinBox(dialogue_box);
  _new_line_id->setRange(0, 255);
  if (!_rows.empty())
  {
    unsigned highest_group = 0;
    for (auto const& state : _rows)
      highest_group = std::max(highest_group, state.row.group_id);
    _new_group_id->setValue(static_cast<int>(std::min(255u, highest_group + 1)));
  }
  _new_chat_type = new QComboBox(dialogue_box);
  _new_chat_type->addItem("Say", 12);
  _new_chat_type->addItem("Yell", 14);
  _new_chat_type->addItem("Whisper", 15);
  _new_chat_type->addItem("Emote", 16);
  _new_chat_type->addItem("Boss emote", 41);
  _new_chat_type->addItem("Boss whisper", 42);
  _new_chat_type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _new_chat_type->setMinimumContentsLength(8);
  _new_language = new QSpinBox(dialogue_box);
  _new_language->setRange(0, 255);
  _new_language->setSpecialValueText("Universal");
  _new_probability = new QDoubleSpinBox(dialogue_box);
  _new_probability->setRange(0.0, 100.0);
  _new_probability->setDecimals(4);
  _new_probability->setValue(100.0);
  _new_probability->setToolTip("Relative line weight. The initial selection chance "
    "is this weight divided by the total weight of the set.");
  _new_emote = new QSpinBox(dialogue_box);
  _new_emote->setRange(0, std::numeric_limits<int>::max());
  _new_emote->setSpecialValueText("None");
  _new_duration = new QSpinBox(dialogue_box);
  _new_duration->setRange(0, std::numeric_limits<int>::max());
  _new_duration->setSpecialValueText("Server default");
  _new_duration->setSuffix(" ms");
  _new_sound = new QSpinBox(dialogue_box);
  _new_sound->setRange(0, std::numeric_limits<int>::max());
  _new_sound->setSpecialValueText("None");
  _new_broadcast = new QSpinBox(dialogue_box);
  _new_broadcast->setRange(0, std::numeric_limits<int>::max());
  _new_broadcast->setSpecialValueText("None");
  _new_broadcast->setEnabled(_context.broadcast_text_available);
  _new_broadcast->setToolTip(_context.broadcast_text_available
    ? "Optional shared BroadcastText ID for localized text. Its row is not edited here."
    : "The connected world database has no broadcast_text table.");
  for (auto* input : {_new_group_id, _new_line_id, _new_language, _new_emote,
                      _new_duration, _new_sound, _new_broadcast})
  {
    input->setMinimumWidth(78);
    input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  }
  _new_probability->setMinimumWidth(78);
  _new_probability->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  _new_sound_choose = new QPushButton("Browse sound...", dialogue_box);
  _new_sound_play = new QPushButton("Play", dialogue_box);
  _add_dialogue = new QPushButton("Add line", dialogue_box);
  _add_dialogue_with_trigger = new QPushButton("Add first line + trigger", dialogue_box);
  _edit_dialogue = new QPushButton("Edit selected line", dialogue_box);
  _apply_dialogue_edit = new QPushButton("Apply line changes", dialogue_box);
  _cancel_dialogue_edit = new QPushButton("Cancel edit", dialogue_box);
  _remove_dialogue = new QPushButton("Remove selected new line", dialogue_box);
  dialogue_layout->addWidget(new QLabel("Text:", dialogue_box), 0, 0);
  dialogue_layout->addWidget(_new_text, 0, 1, 1, 3);
  dialogue_layout->addWidget(new QLabel("Set / group:", dialogue_box), 1, 0);
  dialogue_layout->addWidget(_new_group_id, 1, 1);
  dialogue_layout->addWidget(new QLabel("Line:", dialogue_box), 1, 2);
  dialogue_layout->addWidget(_new_line_id, 1, 3);
  dialogue_layout->addWidget(new QLabel("Chat:", dialogue_box), 2, 0);
  dialogue_layout->addWidget(_new_chat_type, 2, 1);
  dialogue_layout->addWidget(new QLabel("Language:", dialogue_box), 2, 2);
  dialogue_layout->addWidget(_new_language, 2, 3);
  dialogue_layout->addWidget(new QLabel("Line weight:", dialogue_box), 3, 0);
  dialogue_layout->addWidget(_new_probability, 3, 1);
  dialogue_layout->addWidget(new QLabel("Emote:", dialogue_box), 3, 2);
  dialogue_layout->addWidget(_new_emote, 3, 3);
  dialogue_layout->addWidget(new QLabel("Duration:", dialogue_box), 4, 0);
  dialogue_layout->addWidget(_new_duration, 4, 1);
  dialogue_layout->addWidget(new QLabel("Broadcast ID:", dialogue_box), 4, 2);
  dialogue_layout->addWidget(_new_broadcast, 4, 3);
  dialogue_layout->addWidget(new QLabel("Sound:", dialogue_box), 5, 0);
  dialogue_layout->addWidget(_new_sound, 5, 1);
  dialogue_layout->addWidget(_new_sound_choose, 5, 2);
  dialogue_layout->addWidget(_new_sound_play, 5, 3);
  dialogue_layout->addWidget(_add_dialogue, 6, 0, 1, 2);
  dialogue_layout->addWidget(_add_dialogue_with_trigger, 6, 2, 1, 2);
  dialogue_layout->addWidget(_edit_dialogue, 7, 0, 1, 2);
  dialogue_layout->addWidget(_apply_dialogue_edit, 7, 2, 1, 2);
  _new_set = new QPushButton("Start another set", dialogue_box);
  dialogue_layout->addWidget(_new_set, 8, 0, 1, 2);
  dialogue_layout->addWidget(_cancel_dialogue_edit, 8, 2, 1, 2);
  dialogue_layout->addWidget(_remove_dialogue, 9, 0, 1, 4);
  _set_summary = new QLabel(dialogue_box);
  _set_summary->setWordWrap(true);
  dialogue_layout->addWidget(_set_summary, 10, 0, 1, 4);
  auto* group_note = new QLabel(
    "Select a row and choose Edit selected line to change its properties. "
    "For new sets, add the first line with its trigger, then add more lines to the same set. "
    "One trigger selects one line. Weights set the first pick; AzerothCore avoids "
    "repeating a line until all lines in the set have played.",
    dialogue_box);
  group_note->setWordWrap(true);
  dialogue_layout->addWidget(group_note, 11, 0, 1, 4);
  layout->addWidget(dialogue_box);

  auto* trigger_box = new QGroupBox("Create dialogue trigger", this);
  auto* trigger_layout = new QGridLayout(trigger_box);
  _trigger_type = new QComboBox(trigger_box);
  _trigger_type->addItem("Immediately after being summoned", 54);
  _trigger_type->addItem("On spawn or combat reset", 25);
  _trigger_type->addItem("On aggro", 4);
  _trigger_type->addItem("On death", 6);
  _trigger_type->addItem("On respawn", 11);
  _trigger_type->addItem("Player approaches while out of combat", 10);
  _trigger_type->addItem("Timer while out of combat", 1);
  _trigger_type->addItem("Quest accepted", 19);
  _trigger_type->addItem("Quest rewarded", 20);
  _trigger_type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _trigger_type->setMinimumContentsLength(12);
  _trigger_type->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  _trigger_chance = new QSpinBox(trigger_box);
  _trigger_chance->setRange(1, 100);
  _trigger_chance->setValue(100);
  _trigger_chance->setSuffix("%");
  _trigger_primary_label = new QLabel(trigger_box);
  _trigger_primary = new QSpinBox(trigger_box);
  _trigger_primary->setRange(0, std::numeric_limits<int>::max());
  _trigger_cooldown_label = new QLabel(trigger_box);
  _trigger_cooldown = new QSpinBox(trigger_box);
  _trigger_cooldown->setRange(0, std::numeric_limits<int>::max());
  _trigger_speech_delay = new QSpinBox(trigger_box);
  _trigger_speech_delay->setRange(0, std::numeric_limits<int>::max());
  _trigger_speech_delay->setSuffix(" ms");
  for (auto* input : {_trigger_chance, _trigger_primary, _trigger_cooldown,
                      _trigger_speech_delay})
  {
    input->setMinimumWidth(78);
    input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  }
  _trigger_scope = new QComboBox(trigger_box);
  _trigger_scope->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _trigger_scope->setMinimumContentsLength(12);
  _trigger_scope->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  _trigger_scope->addItem("NPC template", false);
  _trigger_scope->setToolTip("Template scope follows the save choice: the new private "
    "template with Create unique NPC, or the existing shared template with Save dialogue to template.");
  if (_context.selected_spawn_guid)
    _trigger_scope->addItem(QString("Selected spawn %1 only")
      .arg(_context.selected_spawn_guid), true);
  _add_trigger = new QPushButton("Add trigger", trigger_box);
  _staged_trigger_list = new QComboBox(trigger_box);
  _staged_trigger_list->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _staged_trigger_list->setMinimumContentsLength(12);
  _staged_trigger_list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  _remove_trigger = new QPushButton("Remove staged", trigger_box);
  _trigger_face_player = new QCheckBox("Face friendly player while speaking", trigger_box);
  _trigger_face_seconds = new QSpinBox(trigger_box);
  _trigger_face_seconds->setRange(1, 60);
  _trigger_face_seconds->setValue(3);
  _trigger_face_seconds->setSuffix(" s");
  _existing_trigger = new QComboBox(trigger_box);
  _existing_face_player = new QCheckBox("Face friendly player", trigger_box);
  _existing_face_seconds = new QSpinBox(trigger_box);
  _existing_face_seconds->setRange(1, 60);
  _existing_face_seconds->setValue(3);
  _existing_face_seconds->setSuffix(" s");
  _stage_existing_facing = new QPushButton("Apply to selected trigger", trigger_box);
  trigger_layout->addWidget(new QLabel("When:", trigger_box), 0, 0);
  trigger_layout->addWidget(_trigger_type, 0, 1, 1, 3);
  trigger_layout->addWidget(new QLabel("Trigger chance:", trigger_box), 1, 0);
  trigger_layout->addWidget(_trigger_chance, 1, 1);
  trigger_layout->addWidget(new QLabel("Speech delay:", trigger_box), 1, 2);
  trigger_layout->addWidget(_trigger_speech_delay, 1, 3);
  trigger_layout->addWidget(_trigger_primary_label, 2, 0);
  trigger_layout->addWidget(_trigger_primary, 2, 1);
  trigger_layout->addWidget(_trigger_cooldown_label, 2, 2);
  trigger_layout->addWidget(_trigger_cooldown, 2, 3);
  trigger_layout->addWidget(new QLabel("Scope:", trigger_box), 3, 0);
  trigger_layout->addWidget(_trigger_scope, 3, 1, 1, 3);
  trigger_layout->addWidget(_trigger_face_player, 4, 0, 1, 3);
  trigger_layout->addWidget(_trigger_face_seconds, 4, 3);
  trigger_layout->addWidget(_add_trigger, 5, 0, 1, 2);
  trigger_layout->addWidget(_remove_trigger, 5, 2, 1, 2);
  trigger_layout->addWidget(new QLabel("Staged:", trigger_box), 6, 0);
  trigger_layout->addWidget(_staged_trigger_list, 6, 1, 1, 3);
  trigger_layout->addWidget(new QLabel("Existing trigger:", trigger_box), 7, 0);
  trigger_layout->addWidget(_existing_trigger, 7, 1, 1, 3);
  trigger_layout->addWidget(_existing_face_player, 8, 0, 1, 3);
  trigger_layout->addWidget(_existing_face_seconds, 8, 3);
  trigger_layout->addWidget(_stage_existing_facing, 9, 0, 1, 4);
  layout->addWidget(trigger_box);

  _details = new QLabel(this);
  _details->setWordWrap(true);
  layout->addWidget(_details);

  connect(_tree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem*, QTreeWidgetItem*) { refreshSelection(); });
  connect(_sound_id, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int id) { setSelectedSound(static_cast<unsigned>(id)); });
  connect(_clear, &QPushButton::clicked, this, [this] { _sound_id->setValue(0); });
  connect(_play, &QPushButton::clicked, this, [this]
  {
    auto* item = _tree->currentItem();
    if (!item) return;
    unsigned const index = item->data(0, Qt::UserRole).toUInt();
    if (index < _rows.size()) playSound(_rows[index].row.sound);
  });
  connect(_play_broadcast, &QPushButton::clicked, this, [this]
  {
    auto* item = _tree->currentItem();
    if (!item) return;
    unsigned const index = item->data(0, Qt::UserRole).toUInt();
    if (index < _rows.size()) playSound(_rows[index].row.broadcast_sound);
  });
  connect(_choose, &QPushButton::clicked, this, [this]
  {
    if (!_tree->currentItem()) return;
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
  connect(_new_sound, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int id) { _new_sound_play->setEnabled(soundHasPlayableFile(id)); });
  connect(_new_group_id, QOverload<int>::of(&QSpinBox::valueChanged), this,
           [this](int group)
  {
    unsigned next_line = 0;
    for (auto const& state : _rows)
      if (state.row.group_id == static_cast<unsigned>(group))
        next_line = std::max(next_line, state.row.line_id + 1);
    _new_line_id->setValue(static_cast<int>(std::min(255u, next_line)));
    refreshDialogueSetSummary();
  });
  connect(_new_probability, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          [this](double) { refreshDialogueSetSummary(); });
  connect(_new_sound_play, &QPushButton::clicked, this,
          [this] { playSound(static_cast<unsigned>(_new_sound->value())); });
  connect(_new_sound_choose, &QPushButton::clicked, this, [this]
  {
    auto* target = new QPushButton(this);
    target->hide();
    target->setProperty("id", _new_sound->value());
    auto* picker = new SoundEntryPickerWindow(target, -1, true, this, true);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &QObject::destroyed, this, [this, target]
    {
      _new_sound->setValue(target->property("id").toInt());
      target->deleteLater();
    });
    picker->show();
  });
  connect(_add_dialogue, &QPushButton::clicked, this,
          [this] { addDialogue(false); });
  connect(_add_dialogue_with_trigger, &QPushButton::clicked, this,
          [this] { addDialogue(true); });
  connect(_edit_dialogue, &QPushButton::clicked, this,
          [this] { editSelectedDialogue(); });
  connect(_apply_dialogue_edit, &QPushButton::clicked, this,
          [this] { applyDialogueEdit(); });
  connect(_cancel_dialogue_edit, &QPushButton::clicked, this,
          [this] { cancelDialogueEdit(); });
  connect(_remove_dialogue, &QPushButton::clicked, this,
           [this] { removeSelectedDialogue(); });
  connect(_new_set, &QPushButton::clicked, this,
          [this] { startNewDialogueSet(); });
  connect(_trigger_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int)
  {
    unsigned const type = _trigger_type->currentData().toUInt();
    refreshTriggerControls();
    if (type == 10)
    {
      _trigger_primary->setValue(10);
      _trigger_cooldown->setValue(10000);
    }
    else if (type == 1)
    {
      _trigger_primary->setValue(1000);
      _trigger_cooldown->setValue(30000);
    }
    else if (type == 19 || type == 20)
    {
      _trigger_primary->setValue(1);
      _trigger_cooldown->setValue(0);
    }
    else
    {
      _trigger_primary->setValue(0);
      _trigger_cooldown->setValue(0);
    }
  });
  connect(_add_trigger, &QPushButton::clicked, this,
          [this] { addTrigger(); });
  connect(_remove_trigger, &QPushButton::clicked, this,
          [this] { removeStagedTrigger(); });
  connect(_trigger_face_player, &QCheckBox::toggled, _trigger_face_seconds,
          &QSpinBox::setEnabled);
  connect(_existing_face_player, &QCheckBox::toggled, _existing_face_seconds,
          &QSpinBox::setEnabled);
  connect(_existing_trigger, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { refreshExistingTriggerControls(); });
  connect(_stage_existing_facing, &QPushButton::clicked, this,
          [this] { stageExistingFacing(); });
  connect(_staged_trigger_list, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { _remove_trigger->setEnabled(_staged_trigger_list->currentIndex() >= 0); });

  refreshTriggerControls();
  _new_sound_play->setEnabled(false);
  _add_dialogue->setEnabled(_context.creature_text_available);
  _add_dialogue->setToolTip(_context.creature_text_available ? QString()
    : QString("The connected world database has no creature_text table."));
  setDialogueEditMode(false);
  refreshRows();
}

bool NpcDialogueSoundEditor::changed() const
{
  if (!_trigger_changes.empty() || !_facing_changes.empty()) return true;
  for (auto const& state : _rows)
    if (state.is_new || state.row.sound != state.original_sound
        || linePropertiesDiffer(state.row, state.original_row)) return true;
  return false;
}

std::vector<NpcDialogueTriggerChange> NpcDialogueSoundEditor::triggerChanges() const
{
  return _trigger_changes;
}

std::vector<NpcDialogueFacingChange> NpcDialogueSoundEditor::facingChanges() const
{
  return _facing_changes;
}

std::vector<NpcDialogueSoundChange> NpcDialogueSoundEditor::changes() const
{
  std::vector<NpcDialogueSoundChange> result;
  for (auto const& state : _rows)
    if (!state.is_new && state.row.sound != state.original_sound
        && !linePropertiesDiffer(state.row, state.original_row))
      result.push_back({state.row.group_id, state.row.line_id, state.row.sound});
  return result;
}

std::vector<NpcDialogueLineChange> NpcDialogueSoundEditor::newDialogueLines() const
{
  std::vector<NpcDialogueLineChange> result;
  for (auto const& state : _rows)
  {
    if (!state.is_new) continue;
    auto const& row = state.row;
    result.push_back({row.group_id, row.line_id, row.text, row.chat_type, row.language,
                      row.probability, row.emote, row.duration, row.sound,
                      row.broadcast_text_id, row.text_range, row.comment});
  }
  return result;
}

std::vector<NpcDialogueLineUpdate> NpcDialogueSoundEditor::dialogueLineUpdates() const
{
  std::vector<NpcDialogueLineUpdate> result;
  for (auto const& state : _rows)
  {
    if (state.is_new || !linePropertiesDiffer(state.row, state.original_row)) continue;
    result.push_back({lineChange(state.original_row), lineChange(state.row)});
  }
  return result;
}

bool NpcDialogueSoundEditor::validate(QString& error) const
{
  if (std::any_of(_rows.begin(), _rows.end(),
                  [](RowState const& state)
                  { return state.is_new || linePropertiesDiffer(state.row, state.original_row); })
      && !_context.creature_text_available)
  {
    error = "The world database has no creature_text table for the dialogue line changes.";
    return false;
  }
  if (!_trigger_changes.empty() || !_facing_changes.empty())
  {
    if (!_context.smart_scripts_available)
    {
      error = "The world database has no smart_scripts table for the dialogue trigger.";
      return false;
    }
    if (!_context.script_name.isEmpty()
        || (!_context.ai_name.isEmpty()
            && _context.ai_name.compare("SmartAI", Qt::CaseInsensitive) != 0))
    {
      error = "This NPC uses a different server AI and cannot safely receive SmartAI triggers.";
      return false;
    }
  }
  for (auto const& state : _rows)
  {
    if ((!state.is_new && state.row.sound == state.original_sound) || !state.row.sound) continue;
    if (!gSoundEntriesDB.CheckIfIdExists(state.row.sound))
    {
      error = QString("Dialogue group %1, line %2 references missing SoundEntries row %3.")
        .arg(state.row.group_id).arg(state.row.line_id).arg(state.row.sound);
      return false;
    }
  }
  return true;
}

void NpcDialogueSoundEditor::refreshRows()
{
  _tree->clear();
  for (std::size_t index = 0; index < _rows.size(); ++index)
  {
    auto const& state = _rows[index];
    auto const& row = state.row;
    auto* item = new QTreeWidgetItem(_tree);
    item->setData(0, Qt::UserRole, static_cast<unsigned>(index));
    item->setText(0, QString("%1%2 / %3")
      .arg(state.is_new ? "NEW: " : "").arg(row.group_id).arg(row.line_id));
    item->setText(1, row.text.simplified());
    item->setToolTip(1, row.text);
     item->setText(2, QString("%1%").arg(selectionChance(index), 0, 'f', 1));
     item->setToolTip(2, QString("First selection: %1% from weight %2. "
       "Previously spoken lines are excluded until all lines in this set have played.")
       .arg(selectionChance(index), 0, 'f', 1).arg(row.probability, 0, 'g', 4));
    QStringList trigger_labels;
    for (auto const& trigger : row.triggers)
      trigger_labels.push_back(QString("%1%2 · %3% · %4%5")
        .arg(trigger.staged ? "NEW: " : "").arg(trigger.event)
        .arg(trigger.chance).arg(trigger.scope)
        .arg(trigger.face_player_duration
          ? QString(" · faces player for %1 s").arg(trigger.face_player_duration / 1000)
          : QString()));
    int const staged_count = static_cast<int>(std::count_if(row.triggers.begin(),
      row.triggers.end(), [](NpcDialogueTriggerReference const& trigger)
      { return trigger.staged; }));
    item->setText(3, trigger_labels.isEmpty()
      ? "None"
      : staged_count ? QString("%1 · %2 new").arg(trigger_labels.size()).arg(staged_count)
                     : QString("%1 trigger%2").arg(trigger_labels.size())
                         .arg(trigger_labels.size() == 1 ? "" : "s"));
    item->setToolTip(3, trigger_labels.isEmpty() ? _context.trigger_scan_note
                                                 : trigger_labels.join("\n"));
    item->setText(4, soundLabel(row.sound));
    item->setText(5, row.broadcast_text_id
      ? (state.is_new ? QString("Text %1: shared reference").arg(row.broadcast_text_id)
                      : QString("Text %1: %2").arg(row.broadcast_text_id)
                          .arg(soundLabel(row.broadcast_sound)))
      : "No BroadcastText link");
  }
  if (_tree->topLevelItemCount()) _tree->setCurrentItem(_tree->topLevelItem(0));
  refreshDialogueSetSummary();
  refreshSelection();
}

void NpcDialogueSoundEditor::refreshDialogueSetSummary()
{
  unsigned const group = static_cast<unsigned>(_new_group_id->value());
  double weight_sum = 0.0;
  unsigned count = 0;
  for (auto const& state : _rows)
  {
    if (state.row.group_id != group) continue;
    ++count;
    weight_sum += std::max(0.0, state.row.probability);
  }
  _set_summary->setText(count
    ? QString("Set %1: %2 line%3, total weight %4. The next line stays in this set; "
              "its first-pick chance will be its weight divided by the new total.")
        .arg(group).arg(count).arg(count == 1 ? "" : "s")
        .arg(weight_sum, 0, 'g', 5)
    : QString("Set %1 is empty. Add its first line and a trigger, then add alternatives.")
        .arg(group));
}

void NpcDialogueSoundEditor::startNewDialogueSet()
{
  if (_editing_row >= 0) cancelDialogueEdit();
  unsigned next = 0;
  while (next <= 255 && std::any_of(_rows.begin(), _rows.end(),
    [next](RowState const& state) { return state.row.group_id == next; }))
    ++next;
  if (next > 255)
  {
    _details->setText("All 256 dialogue group IDs are already in use.");
    return;
  }
  _new_group_id->setValue(static_cast<int>(next));
  _new_line_id->setValue(0);
  _new_text->setFocus();
}

void NpcDialogueSoundEditor::refreshSelection()
{
  _existing_trigger->clear();
  auto* item = _tree->currentItem();
  bool const selected = item != nullptr;
  _sound_id->setEnabled(selected);
  _choose->setEnabled(selected);
  _clear->setEnabled(selected);
  _edit_dialogue->setEnabled(selected && _editing_row < 0);
  _remove_dialogue->setEnabled(false);
  bool const smart_ai_compatible = _context.script_name.isEmpty()
    && (_context.ai_name.isEmpty()
        || _context.ai_name.compare("SmartAI", Qt::CaseInsensitive) == 0);
  _add_trigger->setEnabled(selected && _context.smart_scripts_available
                           && smart_ai_compatible);
  if (!selected)
  {
    refreshExistingTriggerControls();
    _play->setEnabled(false);
    _play_broadcast->setEnabled(false);
    _details->setText(_rows.empty()
      ? "This NPC has no creature_text dialogue rows. Use Add dialogue line to create one."
      : "Select a dialogue line.");
    return;
  }

  unsigned const index = item->data(0, Qt::UserRole).toUInt();
  if (index >= _rows.size()) return;
  _remove_dialogue->setEnabled(_rows[index].is_new && _editing_row < 0);
  auto const& row = _rows[index].row;
  for (auto const& trigger : row.triggers)
  {
    if (trigger.staged || !trigger.smart_owner ||
        (trigger.event_type != 10 && trigger.event_type != 19 && trigger.event_type != 20))
      continue;
    int const trigger_index = _existing_trigger->count();
    _existing_trigger->addItem(QString("%1 (%2)").arg(trigger.event, trigger.scope));
    _existing_trigger->setItemData(trigger_index,
      static_cast<qlonglong>(trigger.smart_owner));
    _existing_trigger->setItemData(trigger_index, trigger.smart_id, Qt::UserRole + 1);
  }
  refreshExistingTriggerControls();
  QString const chance = QString(
    "Line selection chance after group %2 is triggered: %1% (database weight %3). "
    "A line is excluded after it plays until every line in the group has played. ")
      .arg(selectionChance(index), 0, 'f', 1).arg(row.group_id)
      .arg(row.probability, 0, 'g', 4);
  QString trigger_details;
  if (row.triggers.empty())
    trigger_details = "Trigger status: " + (_context.trigger_scan_note.isEmpty()
      ? QString("No direct database trigger was found. Stored dialogue does not run itself. ")
      : _context.trigger_scan_note + " ");
  else
  {
    QStringList lines;
    for (auto const& trigger : row.triggers)
    {
      QString line = QString("%1%2 (%3% trigger chance; %4%5)")
        .arg(trigger.staged ? "NEW: " : "").arg(trigger.event)
        .arg(trigger.chance).arg(trigger.scope)
        .arg(trigger.face_player_duration
          ? QString("; faces player for %1 s").arg(trigger.face_player_duration / 1000)
          : QString());
      if (!trigger.comment.isEmpty()) line += " — " + trigger.comment;
      lines.push_back(line);
    }
    trigger_details = "Database trigger" + QString(row.triggers.size() == 1 ? ": " : "s: ")
      + lines.join("; ") + ". ";
  }
  QSignalBlocker const blocker(_sound_id);
  _sound_id->setValue(static_cast<int>(row.sound));
  _play->setEnabled(soundHasPlayableFile(row.sound));
  _play_broadcast->setEnabled(soundHasPlayableFile(row.broadcast_sound));

  if (!row.sound)
  {
    _details->setText(chance + trigger_details + (row.broadcast_sound
      ? QString("No scripted sound is assigned. BroadcastText %1 references %2, but the "
                "creature_text playback path does not use that field.")
          .arg(row.broadcast_text_id).arg(soundLabel(row.broadcast_sound))
      : "No scripted sound is assigned to this dialogue line."));
    return;
  }
  if (!gSoundEntriesDB.CheckIfIdExists(row.sound))
  {
    _details->setText(chance + trigger_details
      + QString("SoundEntries %1 is missing from the configured client.")
                                 .arg(row.sound));
    return;
  }

  auto const entry = gSoundEntriesDB.getByID(row.sound);
  QStringList files;
  QStringList missing;
  QString const folder = QString::fromUtf8(entry.getString(SoundEntriesDB::FilePath));
  auto const client = Noggit::Application::NoggitApplication::instance()->clientData();
  for (unsigned file = 0; file < 10; ++file)
  {
    QString const name = QString::fromUtf8(entry.getString(SoundEntriesDB::Filenames + file));
    if (name.isEmpty()) continue;
    files.push_back(name);
    if (client && !client->exists((folder + "\\" + name).toStdString())) missing.push_back(name);
  }
  _details->setText(chance + trigger_details + QString("Folder: %1   Files: %2%3")
    .arg(folder, files.isEmpty() ? "None" : files.join(", "),
         missing.isEmpty() ? QString() : "   Missing: " + missing.join(", ")));
}

void NpcDialogueSoundEditor::setSelectedSound(unsigned id)
{
  auto* item = _tree->currentItem();
  if (!item) return;
  unsigned const index = item->data(0, Qt::UserRole).toUInt();
  if (index >= _rows.size()) return;
  _rows[index].row.sound = id;
  item->setText(4, soundLabel(id));
  refreshSelection();
}

void NpcDialogueSoundEditor::loadDialogueForm(NpcDialogueSoundRow const& row)
{
  QSignalBlocker const group_blocker(_new_group_id);
  QSignalBlocker const probability_blocker(_new_probability);
  _new_text->setText(row.text);
  _new_group_id->setValue(static_cast<int>(row.group_id));
  _new_line_id->setValue(static_cast<int>(row.line_id));
  int chat_index = _new_chat_type->findData(row.chat_type);
  if (chat_index < 0)
  {
    _new_chat_type->addItem(QString("Chat type %1").arg(row.chat_type), row.chat_type);
    chat_index = _new_chat_type->count() - 1;
  }
  _new_chat_type->setCurrentIndex(chat_index);
  _new_language->setValue(row.language);
  _new_probability->setValue(row.probability);
  _new_emote->setValue(static_cast<int>(row.emote));
  _new_duration->setValue(static_cast<int>(row.duration));
  _new_sound->setValue(static_cast<int>(row.sound));
  _new_broadcast->setValue(static_cast<int>(row.broadcast_text_id));
  refreshDialogueSetSummary();
}

void NpcDialogueSoundEditor::setDialogueEditMode(bool editing)
{
  _tree->setEnabled(!editing);
  _new_group_id->setEnabled(!editing);
  _new_line_id->setEnabled(!editing);
  _add_dialogue->setEnabled(!editing && _context.creature_text_available);
  bool const smart_ai_compatible = _context.script_name.isEmpty()
    && (_context.ai_name.isEmpty()
        || _context.ai_name.compare("SmartAI", Qt::CaseInsensitive) == 0);
  _add_dialogue_with_trigger->setEnabled(!editing && _context.creature_text_available
    && _context.smart_scripts_available && smart_ai_compatible);
  _edit_dialogue->setEnabled(!editing && _tree->currentItem());
  _apply_dialogue_edit->setEnabled(editing);
  _cancel_dialogue_edit->setEnabled(editing);
  _new_set->setEnabled(!editing);
  auto* item = _tree->currentItem();
  if (!editing && item)
  {
    unsigned const index = item->data(0, Qt::UserRole).toUInt();
    _remove_dialogue->setEnabled(index < _rows.size() && _rows[index].is_new);
  }
  else
    _remove_dialogue->setEnabled(false);
}

void NpcDialogueSoundEditor::editSelectedDialogue()
{
  auto* item = _tree->currentItem();
  if (!item) return;
  unsigned const index = item->data(0, Qt::UserRole).toUInt();
  if (index >= _rows.size()) return;
  _editing_row = static_cast<int>(index);
  loadDialogueForm(_rows[index].row);
  setDialogueEditMode(true);
  _new_text->setFocus();
  _details->setText(QString("Editing dialogue set %1, line %2. The set and line IDs remain fixed "
                            "because triggers and database rows use them as keys.")
    .arg(_rows[index].row.group_id).arg(_rows[index].row.line_id));
}

void NpcDialogueSoundEditor::applyDialogueEdit()
{
  if (_editing_row < 0 || static_cast<std::size_t>(_editing_row) >= _rows.size()) return;
  QString const text = _new_text->text().trimmed();
  if (text.isEmpty())
  {
    _details->setText("Dialogue text cannot be empty.");
    _new_text->setFocus();
    return;
  }
  auto& row = _rows[static_cast<std::size_t>(_editing_row)].row;
  row.text = text;
  row.chat_type = _new_chat_type->currentData().toUInt();
  row.language = _new_language->value();
  row.probability = _new_probability->value();
  row.emote = static_cast<unsigned>(_new_emote->value());
  row.duration = static_cast<unsigned>(_new_duration->value());
  row.sound = static_cast<unsigned>(_new_sound->value());
  row.broadcast_text_id = static_cast<unsigned>(_new_broadcast->value());
  unsigned const index = static_cast<unsigned>(_editing_row);
  _editing_row = -1;
  setDialogueEditMode(false);
  refreshRows();
  if (index < static_cast<unsigned>(_tree->topLevelItemCount()))
    _tree->setCurrentItem(_tree->topLevelItem(static_cast<int>(index)));
}

void NpcDialogueSoundEditor::cancelDialogueEdit()
{
  if (_editing_row < 0) return;
  unsigned group = 0;
  if (static_cast<std::size_t>(_editing_row) < _rows.size())
    group = _rows[static_cast<std::size_t>(_editing_row)].row.group_id;
  _editing_row = -1;
  setDialogueEditMode(false);
  _new_text->clear();
  _new_group_id->setValue(static_cast<int>(group));
  _new_chat_type->setCurrentIndex(std::max(0, _new_chat_type->findData(12)));
  _new_language->setValue(0);
  _new_probability->setValue(100.0);
  _new_emote->setValue(0);
  _new_duration->setValue(0);
  _new_sound->setValue(0);
  _new_broadcast->setValue(0);
}

void NpcDialogueSoundEditor::addDialogue(bool with_trigger)
{
  QString const text = _new_text->text().trimmed();
  if (text.isEmpty())
  {
    _details->setText("Enter dialogue text before adding the line.");
    _new_text->setFocus();
    return;
  }
  if (!_context.creature_text_available) return;
  if (with_trigger && !_add_dialogue_with_trigger->isEnabled()) return;

  unsigned const group = static_cast<unsigned>(_new_group_id->value());
  unsigned const line = static_cast<unsigned>(_new_line_id->value());
  bool group_exists = false;
  for (auto const& state : _rows)
  {
    if (state.row.group_id != group) continue;
    group_exists = true;
    if (state.row.line_id == line)
    {
      _details->setText(QString("Dialogue group %1, line %2 already exists.").arg(group).arg(line));
      return;
    }
  }
  if (with_trigger && group_exists)
  {
    _details->setText("Choose an unused group for this line's own trigger. "
                      "A trigger for an existing group would also select its other lines.");
    return;
  }

  NpcDialogueSoundRow row;
  row.group_id = group;
  row.line_id = line;
  row.text = text;
  row.probability = _new_probability->value();
  row.sound = static_cast<unsigned>(_new_sound->value());
  row.broadcast_text_id = static_cast<unsigned>(_new_broadcast->value());
  row.chat_type = _new_chat_type->currentData().toUInt();
  row.language = _new_language->value();
  row.emote = static_cast<unsigned>(_new_emote->value());
  row.duration = static_cast<unsigned>(_new_duration->value());
  row.comment = "Noggit custom dialogue";
  for (auto const& state : _rows)
    if (state.row.group_id == group)
    {
      row.triggers = state.row.triggers;
      break;
    }
  _rows.push_back({std::move(row), {}, 0, true});
  refreshRows();
  int const new_index = static_cast<int>(_rows.size() - 1);
  _tree->setCurrentItem(_tree->topLevelItem(new_index));
  if (with_trigger) addTrigger();
  _new_text->clear();
  _new_line_id->setValue(static_cast<int>(std::min(255u, line + 1)));
}

void NpcDialogueSoundEditor::removeSelectedDialogue()
{
  auto* item = _tree->currentItem();
  if (!item) return;
  unsigned const row_index = item->data(0, Qt::UserRole).toUInt();
  if (row_index >= _rows.size() || !_rows[row_index].is_new) return;
  unsigned const group = _rows[row_index].row.group_id;
  bool const group_remains = std::any_of(_rows.begin(), _rows.end(),
    [&](RowState const& state)
    {
      return &state != &_rows[row_index] && state.row.group_id == group;
    });
  if (!group_remains)
  {
    for (int index = _staged_trigger_list->count() - 1; index >= 0; --index)
    {
      unsigned const token = _staged_trigger_list->itemData(index).toUInt();
      auto const found = std::find_if(_trigger_changes.begin(), _trigger_changes.end(),
        [token](NpcDialogueTriggerChange const& change) { return change.token == token; });
      if (found != _trigger_changes.end() && found->group_id == group)
        _staged_trigger_list->removeItem(index);
    }
    _trigger_changes.erase(std::remove_if(_trigger_changes.begin(), _trigger_changes.end(),
      [group](NpcDialogueTriggerChange const& change) { return change.group_id == group; }),
      _trigger_changes.end());
  }
  _rows.erase(_rows.begin() + row_index);
  refreshRows();
}

void NpcDialogueSoundEditor::refreshTriggerControls()
{
  unsigned const type = _trigger_type->currentData().toUInt();
  bool const proximity = type == 10;
  bool const timer = type == 1;
  bool const quest = type == 19 || type == 20;
  bool const can_face_player = proximity || quest;
  _trigger_face_player->setEnabled(can_face_player);
  if (!can_face_player) _trigger_face_player->setChecked(false);
  _trigger_face_seconds->setEnabled(can_face_player && _trigger_face_player->isChecked());
  bool const show_primary = proximity || timer || quest;
  bool const show_cooldown = proximity || timer || quest;
  _trigger_primary_label->setVisible(show_primary);
  _trigger_primary->setVisible(show_primary);
  _trigger_cooldown_label->setVisible(show_cooldown);
  _trigger_cooldown->setVisible(show_cooldown);
  if (proximity)
  {
    _trigger_primary_label->setText("Range:");
    _trigger_primary->setRange(1, 100);
    _trigger_primary->setSuffix(" yd");
    _trigger_cooldown_label->setText("Cooldown:");
    _trigger_cooldown->setSuffix(" ms");
  }
  else if (timer)
  {
    _trigger_primary_label->setText("First delay:");
    _trigger_primary->setRange(0, std::numeric_limits<int>::max());
    _trigger_primary->setSuffix(" ms");
    _trigger_cooldown_label->setText("Repeat every:");
    _trigger_cooldown->setSuffix(" ms");
  }
  else if (quest)
  {
    _trigger_primary_label->setText("Quest ID:");
    _trigger_primary->setRange(1, std::numeric_limits<int>::max());
    _trigger_primary->setSuffix(QString());
    _trigger_cooldown_label->setText("Cooldown:");
    _trigger_cooldown->setSuffix(" ms");
  }
  bool const compatible = _context.smart_scripts_available
    && _context.script_name.isEmpty()
    && (_context.ai_name.isEmpty()
        || _context.ai_name.compare("SmartAI", Qt::CaseInsensitive) == 0);
  QString reason;
  if (!_context.smart_scripts_available)
    reason = "The connected world database has no smart_scripts table.";
  else if (!_context.script_name.isEmpty())
    reason = QString("This NPC is controlled by C++ script %1.").arg(_context.script_name);
  else if (!_context.ai_name.isEmpty()
           && _context.ai_name.compare("SmartAI", Qt::CaseInsensitive) != 0)
    reason = QString("This NPC uses AIName %1 instead of SmartAI.").arg(_context.ai_name);
  _add_trigger->setToolTip(compatible ? QString() : reason);
  _add_dialogue_with_trigger->setEnabled(_editing_row < 0
    && _context.creature_text_available && compatible);
  _add_dialogue_with_trigger->setToolTip(_context.creature_text_available
    ? (compatible ? QString() : reason)
    : QString("The connected world database has no creature_text table."));
  _remove_trigger->setEnabled(_staged_trigger_list->currentIndex() >= 0);
}

void NpcDialogueSoundEditor::addTrigger()
{
  auto* item = _tree->currentItem();
  if (!item || !_add_trigger->isEnabled()) return;
  unsigned const row_index = item->data(0, Qt::UserRole).toUInt();
  if (row_index >= _rows.size()) return;
  unsigned const group = _rows[row_index].row.group_id;
  unsigned const event_type = _trigger_type->currentData().toUInt();
  bool const spawn_only = _trigger_scope->currentData().toBool();
  if (std::any_of(_trigger_changes.begin(), _trigger_changes.end(),
      [=](NpcDialogueTriggerChange const& change)
      {
        return change.group_id == group && change.event_type == event_type
          && change.selected_spawn_only == spawn_only;
      }))
  {
    _details->setText("That trigger is already staged for this dialogue group and scope.");
    return;
  }
  std::array<unsigned, 5> params{};
  if (event_type == 10)
  {
    params[0] = 1; // Include non-hostile targets.
    params[1] = static_cast<unsigned>(_trigger_primary->value());
    params[2] = params[3] = static_cast<unsigned>(_trigger_cooldown->value());
    params[4] = 1; // Players only.
  }
  else if (event_type == 1)
  {
    params[0] = params[1] = static_cast<unsigned>(_trigger_primary->value());
    params[2] = params[3] = static_cast<unsigned>(_trigger_cooldown->value());
  }
  else if (event_type == 19 || event_type == 20)
  {
    params[0] = static_cast<unsigned>(_trigger_primary->value());
    params[1] = params[2] = static_cast<unsigned>(_trigger_cooldown->value());
  }
  NpcDialogueTriggerChange change;
  change.token = _next_trigger_token++;
  change.group_id = group;
  change.event_type = event_type;
  change.event_chance = static_cast<unsigned>(_trigger_chance->value());
  change.event_params = params;
  change.speech_delay = static_cast<unsigned>(_trigger_speech_delay->value());
  change.face_player_duration = _trigger_face_player->isChecked()
    ? static_cast<unsigned>(_trigger_face_seconds->value()) * 1000 : 0;
  change.selected_spawn_only = spawn_only;
  change.event_label = _trigger_type->currentText();
  _trigger_changes.push_back(change);

  NpcDialogueTriggerReference const reference = {
    change.event_label,
    change.event_chance,
     spawn_only ? QString("selected spawn GUID %1").arg(_context.selected_spawn_guid)
                : QString("NPC template"),
    "Staged; saved when the unique NPC is created.",
    true,
    change.token,
    0,
    0,
    change.event_type,
    change.face_player_duration};
  for (auto& state : _rows)
    if (state.row.group_id == group) state.row.triggers.push_back(reference);
  _staged_trigger_list->addItem(QString("Group %1: %2")
    .arg(group).arg(change.event_label), change.token);
  refreshRows();
  if (row_index < static_cast<unsigned>(_tree->topLevelItemCount()))
    _tree->setCurrentItem(_tree->topLevelItem(static_cast<int>(row_index)));
}

void NpcDialogueSoundEditor::removeStagedTrigger()
{
  int const staged_index = _staged_trigger_list->currentIndex();
  if (staged_index < 0) return;
  unsigned const token = _staged_trigger_list->currentData().toUInt();
  _trigger_changes.erase(std::remove_if(_trigger_changes.begin(), _trigger_changes.end(),
    [token](NpcDialogueTriggerChange const& change) { return change.token == token; }),
    _trigger_changes.end());
  for (auto& state : _rows)
    state.row.triggers.erase(std::remove_if(state.row.triggers.begin(), state.row.triggers.end(),
      [token](NpcDialogueTriggerReference const& trigger)
      { return trigger.staged && trigger.staged_token == token; }), state.row.triggers.end());
  _staged_trigger_list->removeItem(staged_index);
  refreshRows();
}

void NpcDialogueSoundEditor::refreshExistingTriggerControls()
{
  bool const selected = _existing_trigger->currentIndex() >= 0;
  _existing_face_player->setEnabled(selected);
  _stage_existing_facing->setEnabled(selected);
  if (!selected)
  {
    _existing_face_player->setChecked(false);
    _existing_face_seconds->setEnabled(false);
    return;
  }
  std::int64_t const owner = _existing_trigger->currentData().toLongLong();
  unsigned const id = _existing_trigger->currentData(Qt::UserRole + 1).toUInt();
  auto* item = _tree->currentItem();
  if (!item) return;
  unsigned const row_index = item->data(0, Qt::UserRole).toUInt();
  if (row_index >= _rows.size()) return;
  for (auto const& trigger : _rows[row_index].row.triggers)
  {
    if (trigger.smart_owner != owner || trigger.smart_id != id) continue;
    _existing_face_player->setChecked(trigger.face_player_duration != 0);
    _existing_face_seconds->setValue(trigger.face_player_duration
      ? static_cast<int>(trigger.face_player_duration / 1000) : 3);
    _existing_face_seconds->setEnabled(trigger.face_player_duration != 0);
    return;
  }
}

void NpcDialogueSoundEditor::stageExistingFacing()
{
  if (_existing_trigger->currentIndex() < 0) return;
  std::int64_t const owner = _existing_trigger->currentData().toLongLong();
  unsigned const id = _existing_trigger->currentData(Qt::UserRole + 1).toUInt();
  unsigned const duration = _existing_face_player->isChecked()
    ? static_cast<unsigned>(_existing_face_seconds->value()) * 1000 : 0;
  for (auto& state : _rows)
    for (auto& trigger : state.row.triggers)
    {
      if (trigger.smart_owner != owner || trigger.smart_id != id) continue;
      auto const found = std::find_if(_facing_changes.begin(), _facing_changes.end(),
        [owner, id](NpcDialogueFacingChange const& change)
        { return change.smart_owner == owner && change.smart_id == id; });
      if (found == _facing_changes.end())
        _facing_changes.push_back({owner, id, state.row.group_id,
                                   trigger.face_player_duration, duration});
      else
        found->duration = duration;
      trigger.face_player_duration = duration;
    }
  _facing_changes.erase(std::remove_if(_facing_changes.begin(), _facing_changes.end(),
    [](NpcDialogueFacingChange const& change)
    { return change.duration == change.original_duration; }), _facing_changes.end());
  _details->setText(QString("Facing staged for this trigger: %1. Save dialogue to apply it.")
    .arg(duration ? QString("%1 seconds").arg(duration / 1000) : "off"));
}

double NpcDialogueSoundEditor::selectionChance(std::size_t index) const
{
  if (index >= _rows.size()) return 0.0;
  unsigned const group = _rows[index].row.group_id;
  double weight_sum = 0.0;
  std::size_t group_size = 0;
  for (auto const& state : _rows)
  {
    if (state.row.group_id != group) continue;
    ++group_size;
    if (state.row.probability > 0.0) weight_sum += state.row.probability;
  }
  if (!group_size) return 0.0;
  if (weight_sum <= 0.0) return 100.0 / static_cast<double>(group_size);
  return std::max(0.0, _rows[index].row.probability) * 100.0 / weight_sum;
}

void NpcDialogueSoundEditor::playSound(unsigned id)
{
  if (!soundHasPlayableFile(id)) return;
  auto* player = new SoundEntryPlayer(this);
  player->setAttribute(Qt::WA_DeleteOnClose);
  player->setWindowFlag(Qt::Window);
  player->LoadSoundsFromSoundEntry(static_cast<int>(id));
  player->show();
}
}
