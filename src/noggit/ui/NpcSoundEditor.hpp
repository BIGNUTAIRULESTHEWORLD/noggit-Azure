#pragma once

#include <QWidget>
#include <QString>

#include <array>
#include <vector>

class DBCFile;
class QLabel;
class QComboBox;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace Noggit::Ui
{
  // Stages sound assignments for a new, private NPC display. No DBC is changed
  // until the owning NPC editor commits its other client and server assets.
  class NpcSoundEditor : public QWidget
  {
  public:
    NpcSoundEditor(DBCFile* displays, DBCFile* models, DBCFile* voices,
                   DBCFile* actions, DBCFile* footstep_terrain,
                   QWidget* parent = nullptr);

    void setDisplay(unsigned display_id);
    unsigned displayId() const { return _display_id; }
    bool voiceChanged() const;
    bool actionChanged() const;
    bool voiceSlotChanged(unsigned slot) const;
    bool actionSlotChanged(unsigned field) const;
    bool voiceSlotOverridden(unsigned slot) const;
    bool actionSlotOverridden(unsigned field) const;
    bool voiceOverridesChanged() const;
    bool actionOverridesChanged() const;
    unsigned voiceBaseId() const { return _voice_base_id; }
    unsigned actionBaseId() const { return _action_base_id; }
    unsigned selectedVoiceBaseId() const { return _selected_voice_base_id; }
    unsigned selectedActionBaseId() const { return _selected_action_base_id; }
    unsigned voiceSound(unsigned slot) const { return _voice[slot]; }
    unsigned actionSound(unsigned field) const { return _action[field]; }

  private:
    struct SoundPreset
    {
      QString name;
      unsigned action_id = 0;
      unsigned voice_id = 0;
    };
    void buildPresets();
    void selectPreset(int index);
    void refreshPresetStatus();
    void refreshRows();
    void refreshSelection();
    void setSelectedSound(unsigned id);

    DBCFile* _displays;
    DBCFile* _models;
    DBCFile* _voices;
    DBCFile* _actions;
    DBCFile* _footstep_terrain;
    QTreeWidget* _tree = nullptr;
    QComboBox* _presets = nullptr;
    QLabel* _preset_status = nullptr;
    QSpinBox* _sound_id = nullptr;
    QLabel* _selected_label = nullptr;
    QLabel* _details = nullptr;
    QPushButton* _choose = nullptr;
    QPushButton* _clear = nullptr;
    QPushButton* _revert = nullptr;
    QPushButton* _play = nullptr;
    unsigned _display_id = 0;
    unsigned _voice_base_id = 0;
    unsigned _action_base_id = 0;
    unsigned _selected_voice_base_id = 0;
    unsigned _selected_action_base_id = 0;
    QString _voice_source;
    QString _action_source;
    QString _original_voice_source;
    QString _original_action_source;
    std::array<unsigned, 4> _voice{};
    std::array<unsigned, 4> _original_voice{};
    std::array<unsigned, 4> _preset_voice{};
    std::array<unsigned, 38> _action{};
    std::array<unsigned, 38> _original_action{};
    std::array<unsigned, 38> _preset_action{};
    std::vector<SoundPreset> _preset_catalog;
    int _active_preset_index = 0;
  };
}
