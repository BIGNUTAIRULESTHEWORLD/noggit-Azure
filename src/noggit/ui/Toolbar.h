// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/tool_enums.hpp>
#include <QtGui/QIcon>
#include <QtWidgets/QToolBar>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSettings;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace Noggit
{
  class Tool;

  namespace Ui
  {
    class toolbar : public QToolBar
    {
    public:
      toolbar(std::vector<std::unique_ptr<Noggit::Tool>> const& tools,
              std::function<void(editing_mode)> set_editing_mode,
              QSettings* settings);

      void check_tool(editing_mode);
      void add_command(QString category, QString group, QString name,
                       std::function<void()> callback, QString kind = {},
                       std::function<bool()> checked = {});

    protected:
      bool eventFilter(QObject* watched, QEvent* event) override;

    private:
      struct Entry
      {
        QString category;
        QString group;
        QString name;
        QIcon icon;
        std::optional<editing_mode> mode;
        std::function<void()> callback;
        std::function<bool()> checked;
        QString kind;
      };

      std::function<void(editing_mode)> _set_editing_mode;
      QSettings* _settings = nullptr;
      std::vector<Entry> _entries;
      QStringList _pins;
      std::optional<editing_mode> _active_mode;
      std::optional<editing_mode> _previous_mode;
      QString _current_category = QStringLiteral("Environment");
      QString _current_group = QStringLiteral("Terrain");

      QVBoxLayout* _quick_layout = nullptr;
      QWidget* _rail = nullptr;
      QLabel* _active_label = nullptr;
      QToolButton* _previous_button = nullptr;
      QFrame* _browser = nullptr;
      QTimer* _close_timer = nullptr;
      QLineEdit* _search = nullptr;
      QLabel* _browser_heading = nullptr;
      QWidget* _group_panel = nullptr;
      QVBoxLayout* _group_layout = nullptr;
      QWidget* _tool_panel = nullptr;
      QVBoxLayout* _tool_layout = nullptr;
      std::unordered_map<QObject*, QString> _category_buttons;
      std::unordered_map<QObject*, QString> _group_buttons;

      void build_browser();
      void open_browser(QString const& category, QToolButton* source);
      void schedule_browser_close();
      void rebuild_groups();
      void rebuild_tools();
      void rebuild_quick_access();
      void select_entry(Entry const& entry);
      void toggle_pin(QString const& name);
      Entry const* find_entry(QString const& name) const;
      Entry const* find_entry(editing_mode mode) const;
    };
  }
}
