#pragma once

#include <QWidget>

#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace Noggit::Ui
{
  struct NpcDialogueTriggerReference
  {
    QString event;
    unsigned chance = 100;
    QString scope;
    QString comment;
    bool staged = false;
    unsigned staged_token = 0;
    std::int64_t smart_owner = 0;
    unsigned smart_id = 0;
    unsigned event_type = 0;
    unsigned face_player_duration = 0;
  };

  struct NpcDialogueSoundRow
  {
    unsigned group_id = 0;
    unsigned line_id = 0;
    QString text;
    double probability = 0.0;
    unsigned sound = 0;
    unsigned broadcast_text_id = 0;
    unsigned broadcast_sound = 0;
    std::vector<NpcDialogueTriggerReference> triggers;
    unsigned chat_type = 12;
    int language = 0;
    unsigned emote = 0;
    unsigned duration = 0;
    unsigned text_range = 0;
    QString comment;
  };

  struct NpcDialogueSoundContext
  {
    unsigned source_entry = 0;
    QString ai_name;
    QString script_name;
    QStringList spawn_sources;
    QString trigger_scan_note;
    std::uint64_t selected_spawn_guid = 0;
    bool creature_text_available = false;
    bool broadcast_text_available = false;
    bool smart_scripts_available = false;
  };

  struct NpcDialogueSoundChange
  {
    unsigned group_id = 0;
    unsigned line_id = 0;
    unsigned sound = 0;
  };

  struct NpcDialogueTriggerChange
  {
    unsigned token = 0;
    unsigned group_id = 0;
    unsigned event_type = 0;
    unsigned event_chance = 100;
    std::array<unsigned, 5> event_params{};
    unsigned speech_delay = 0;
    unsigned face_player_duration = 0;
    bool selected_spawn_only = false;
    QString event_label;
  };

  struct NpcDialogueFacingChange
  {
    std::int64_t smart_owner = 0;
    unsigned smart_id = 0;
    unsigned group_id = 0;
    unsigned original_duration = 0;
    unsigned duration = 0;
  };

  struct NpcDialogueLineChange
  {
    unsigned group_id = 0;
    unsigned line_id = 0;
    QString text;
    unsigned chat_type = 12;
    int language = 0;
    double probability = 100.0;
    unsigned emote = 0;
    unsigned duration = 0;
    unsigned sound = 0;
    unsigned broadcast_text_id = 0;
    unsigned text_range = 0;
    QString comment;
  };

  struct NpcDialogueLineUpdate
  {
    NpcDialogueLineChange original;
    NpcDialogueLineChange replacement;
  };

  // Stages creature_text.Sound edits. BroadcastText sounds are shown for
  // reference and preview, but remain shared and are never changed here.
  class NpcDialogueSoundEditor : public QWidget
  {
  public:
    explicit NpcDialogueSoundEditor(std::vector<NpcDialogueSoundRow> rows,
                                    NpcDialogueSoundContext context = {},
                                    QWidget* parent = nullptr);

    bool changed() const;
    std::vector<NpcDialogueSoundChange> changes() const;
    std::vector<NpcDialogueLineChange> newDialogueLines() const;
    std::vector<NpcDialogueLineUpdate> dialogueLineUpdates() const;
    std::vector<NpcDialogueTriggerChange> triggerChanges() const;
    std::vector<NpcDialogueFacingChange> facingChanges() const;
    bool validate(QString& error) const;

  private:
    struct RowState
    {
      NpcDialogueSoundRow row;
      NpcDialogueSoundRow original_row;
      unsigned original_sound = 0;
      bool is_new = false;
    };

    void refreshRows();
    void refreshSelection();
    void setSelectedSound(unsigned id);
    void playSound(unsigned id);
    void addDialogue(bool with_trigger);
    void editSelectedDialogue();
    void applyDialogueEdit();
    void cancelDialogueEdit();
    void loadDialogueForm(NpcDialogueSoundRow const& row);
    void setDialogueEditMode(bool editing);
    void startNewDialogueSet();
    void refreshDialogueSetSummary();
    void removeSelectedDialogue();
    void refreshTriggerControls();
    void addTrigger();
    void removeStagedTrigger();
    void refreshExistingTriggerControls();
    void stageExistingFacing();
    double selectionChance(std::size_t index) const;

    std::vector<RowState> _rows;
    std::vector<NpcDialogueTriggerChange> _trigger_changes;
    std::vector<NpcDialogueFacingChange> _facing_changes;
    NpcDialogueSoundContext _context;
    unsigned _next_trigger_token = 1;
    int _editing_row = -1;
    QTreeWidget* _tree = nullptr;
    QSpinBox* _sound_id = nullptr;
    QPushButton* _choose = nullptr;
    QPushButton* _clear = nullptr;
    QPushButton* _play = nullptr;
    QPushButton* _play_broadcast = nullptr;
    QLabel* _details = nullptr;
    QLineEdit* _new_text = nullptr;
    QSpinBox* _new_group_id = nullptr;
    QSpinBox* _new_line_id = nullptr;
    QComboBox* _new_chat_type = nullptr;
    QSpinBox* _new_language = nullptr;
    QDoubleSpinBox* _new_probability = nullptr;
    QLabel* _set_summary = nullptr;
    QSpinBox* _new_emote = nullptr;
    QSpinBox* _new_duration = nullptr;
    QSpinBox* _new_sound = nullptr;
    QSpinBox* _new_broadcast = nullptr;
    QPushButton* _new_sound_choose = nullptr;
    QPushButton* _new_sound_play = nullptr;
    QPushButton* _add_dialogue = nullptr;
    QPushButton* _add_dialogue_with_trigger = nullptr;
    QPushButton* _edit_dialogue = nullptr;
    QPushButton* _apply_dialogue_edit = nullptr;
    QPushButton* _cancel_dialogue_edit = nullptr;
    QPushButton* _new_set = nullptr;
    QPushButton* _remove_dialogue = nullptr;
    QComboBox* _trigger_type = nullptr;
    QSpinBox* _trigger_chance = nullptr;
    QLabel* _trigger_primary_label = nullptr;
    QSpinBox* _trigger_primary = nullptr;
    QLabel* _trigger_cooldown_label = nullptr;
    QSpinBox* _trigger_cooldown = nullptr;
    QSpinBox* _trigger_speech_delay = nullptr;
    QCheckBox* _trigger_face_player = nullptr;
    QSpinBox* _trigger_face_seconds = nullptr;
    QComboBox* _trigger_scope = nullptr;
    QPushButton* _add_trigger = nullptr;
    QComboBox* _staged_trigger_list = nullptr;
    QPushButton* _remove_trigger = nullptr;
    QComboBox* _existing_trigger = nullptr;
    QCheckBox* _existing_face_player = nullptr;
    QSpinBox* _existing_face_seconds = nullptr;
    QPushButton* _stage_existing_facing = nullptr;
  };
}
