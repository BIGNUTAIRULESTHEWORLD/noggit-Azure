// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/Toolbar.h>

#include <noggit/Tool.hpp>
#include <noggit/ui/FontNoggit.hpp>

#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <utility>

namespace Noggit::Ui
{
  namespace
  {
    std::pair<QString, QString> location_for(editing_mode mode)
    {
      switch (mode)
      {
      case editing_mode::ground:
      case editing_mode::flatten_blur:
      case editing_mode::holes:
      case editing_mode::impass:
      case editing_mode::water:
      case editing_mode::stamp:
        return {"Environment", "Terrain"};
      case editing_mode::paint:
      case editing_mode::mccv:
        return {"Environment", "Painting"};
      case editing_mode::areaid:
      case editing_mode::light:
        return {"Environment", "World"};
      case editing_mode::object:
      case editing_mode::fence:
        return {"Objects", "Placement"};
      case editing_mode::area_trigger:
        return {"Objects", "Encounters"};
      case editing_mode::chunk:
      case editing_mode::minimap:
        return {"Utilities", "Map"};
      case editing_mode::scripting:
        return {"Utilities", "Advanced"};
      }
      return {"Utilities", "Advanced"};
    }

    QString shortcut_for(editing_mode mode)
    {
      switch (mode)
      {
      case editing_mode::ground: return "1";
      case editing_mode::flatten_blur: return "2";
      case editing_mode::paint: return "3";
      case editing_mode::holes: return "4";
      case editing_mode::areaid: return "5";
      case editing_mode::impass: return "6";
      case editing_mode::water: return "7";
      case editing_mode::mccv: return "8";
      case editing_mode::object: return "9";
      default: return {};
      }
    }

    void clear_layout(QVBoxLayout* layout)
    {
      while (QLayoutItem* item = layout->takeAt(0))
      {
        delete item->widget();
        delete item;
      }
    }
  }

  toolbar::toolbar(std::vector<std::unique_ptr<Noggit::Tool>> const& tools,
                   std::function<void(editing_mode)> set_editing_mode,
                   QSettings* settings)
    : _set_editing_mode(std::move(set_editing_mode))
    , _settings(settings)
  {
    setContextMenuPolicy(Qt::PreventContextMenu);
    setAllowedAreas(Qt::LeftToolBarArea);
    setOrientation(Qt::Vertical);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);

    for (auto const& tool : tools)
    {
      auto const [category, group] = location_for(tool->editingMode());
      Entry entry;
      entry.category = category;
      entry.group = group;
      entry.name = tr(tool->name());
      entry.icon = FontNoggitIcon{tool->icon()};
      entry.mode = tool->editingMode();
      _entries.push_back(std::move(entry));
    }

    _pins = _settings->value("map_view/tool_pins",
      QStringList{"Raise / Lower", "Texture Painter"}).toStringList();
    _pins.removeDuplicates();
    while (_pins.size() > 4)
      _pins.removeLast();

    auto* rail = new QWidget(this);
    _rail = rail;
    rail->setObjectName("noggitQuickAccessRail");
    rail->setFixedWidth(148);
    rail->setStyleSheet(R"(
      QWidget#noggitQuickAccessRail { background: #14233e; color: #e9edf4; }
      QWidget#noggitQuickHolder { background: transparent; }
      QLabel#noggitRailHeading, QLabel#noggitActiveTool { color: #d6b777; }
      QToolButton#noggitQuickTool {
        background: transparent; color: #e9edf4; border-color: transparent;
      }
      QToolButton#noggitQuickTool:hover, QToolButton#noggitQuickTool:pressed {
        background: #294565; border-color: #405c7c;
      }
      QToolButton#noggitPreviousTool {
        background: transparent; color: #aabbd0; border-color: transparent;
      }
      QToolButton#noggitPreviousTool:hover {
        background: #294565; border-color: #405c7c;
      }
      QToolButton#noggitCategoryButton {
        background: transparent; border: 1px solid transparent;
        color: #e9edf4; text-align: left; padding-left: 10px;
      }
      QToolButton#noggitCategoryButton:hover {
        background: #294565; border-color: #405c7c;
      }
      QToolButton#noggitCategoryButton:checked {
        background: #294565; border-left: 2px solid #d6b777;
      }
      QFrame#noggitCategorySeparator {
        background: #405c7c; border: none;
        min-height: 1px; max-height: 1px;
      }
    )");
    auto* rail_layout = new QVBoxLayout(rail);
    rail_layout->setContentsMargins(6, 7, 6, 7);
    rail_layout->setSpacing(3);

    auto* quick_heading = new QLabel(tr("QUICK ACCESS"), rail);
    quick_heading->setObjectName("noggitRailHeading");
    rail_layout->addWidget(quick_heading);
    auto* quick_holder = new QWidget(rail);
    quick_holder->setObjectName("noggitQuickHolder");
    _quick_layout = new QVBoxLayout(quick_holder);
    _quick_layout->setContentsMargins(0, 0, 0, 0);
    _quick_layout->setSpacing(2);
    rail_layout->addWidget(quick_holder);

    _previous_button = new QToolButton(rail);
    _previous_button->setObjectName("noggitPreviousTool");
    _previous_button->setFocusPolicy(Qt::NoFocus);
    _previous_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    _previous_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    _previous_button->setText(tr("Previous tool"));
    _previous_button->setEnabled(false);
    connect(_previous_button, &QToolButton::clicked, this, [this]
    {
      if (_previous_mode)
        _set_editing_mode(*_previous_mode);
    });
    rail_layout->addWidget(_previous_button);

    auto* line = new QFrame(rail);
    line->setObjectName("noggitCategorySeparator");
    line->setFrameShape(QFrame::HLine);
    rail_layout->addWidget(line);
    auto* categories_heading = new QLabel(tr("CATEGORIES"), rail);
    categories_heading->setObjectName("noggitRailHeading");
    rail_layout->addWidget(categories_heading);

    QStringList const categories{"Environment", "Objects", "Diagnostics", "NPCs", "Utilities"};
    for (int index = 0; index < categories.size(); ++index)
    {
      QString const& category = categories[index];
      auto* button = new QToolButton(rail);
      button->setObjectName("noggitCategoryButton");
      button->setText(category);
      button->setCheckable(true);
      button->setFocusPolicy(Qt::NoFocus);
      button->setToolButtonStyle(Qt::ToolButtonTextOnly);
      button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
      button->setMinimumHeight(34);
      button->installEventFilter(this);
      _category_buttons[button] = category;
      connect(button, &QToolButton::clicked, this, [this, button, category]
      {
        open_browser(category, button);
      });
      rail_layout->addWidget(button);
      if (index + 1 < categories.size())
      {
        auto* separator = new QFrame(rail);
        separator->setObjectName("noggitCategorySeparator");
        separator->setFrameShape(QFrame::HLine);
        rail_layout->addWidget(separator);
      }
    }

    line = new QFrame(rail);
    line->setObjectName("noggitCategorySeparator");
    line->setFrameShape(QFrame::HLine);
    rail_layout->addWidget(line);
    _active_label = new QLabel(tr("Active: Raise / Lower"), rail);
    _active_label->setObjectName("noggitActiveTool");
    _active_label->setWordWrap(true);
    rail_layout->addWidget(_active_label);

    addWidget(rail);
    build_browser();
    rebuild_quick_access();
  }

  void toolbar::build_browser()
  {
    // A popup grabs keyboard focus as soon as it opens. The editor must keep
    // receiving movement keys when this menu appears from a hover.
    _browser = new QFrame(this, Qt::Tool | Qt::FramelessWindowHint);
    _browser->setObjectName("noggitToolBrowser");
    _browser->setAttribute(Qt::WA_ShowWithoutActivating);
    _browser->setFocusPolicy(Qt::NoFocus);
    _browser->setFrameShape(QFrame::StyledPanel);
    _browser->setMinimumWidth(420);
    _browser->setStyleSheet(R"(
      QFrame#noggitToolBrowser {
        background: #0c192d; border: 1px solid #405c7c;
      }
      QFrame#noggitToolBrowser QWidget { background: transparent; color: #e9edf4; }
      QFrame#noggitToolBrowser QLabel { color: #d6b777; }
      QFrame#noggitToolBrowser QLineEdit {
        background: #14233e; color: #e9edf4;
        selection-background-color: #315f96;
      }
      QPushButton#noggitToolGroup, QPushButton#noggitToolChoice {
        background: #14233e; color: #e9edf4; border: 1px solid #405c7c;
        text-align: left; padding: 5px 8px;
      }
      QPushButton#noggitToolGroup:hover, QPushButton#noggitToolChoice:hover {
        background: #294565; border-color: #6fa9df;
      }
      QPushButton#noggitToolGroup:checked, QPushButton#noggitToolChoice:checked {
        background: #294565; border-color: #d6b777;
      }
    )");
    _browser->installEventFilter(this);
    _close_timer = new QTimer(this);
    _close_timer->setSingleShot(true);
    _close_timer->setInterval(220);
    connect(_close_timer, &QTimer::timeout, this, [this]
    {
      if (!_browser->isVisible())
        return;
      QPoint const cursor = QCursor::pos();
      QRect const rail_bounds(_rail->mapToGlobal(QPoint(0, 0)), _rail->size());
      if (!_browser->geometry().contains(cursor) && !rail_bounds.contains(cursor))
        _browser->hide();
    });
    auto* outer = new QVBoxLayout(_browser);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    _search = new QLineEdit(_browser);
    _search->setPlaceholderText(tr("Find a tool or command..."));
    _search->setClearButtonEnabled(true);
    outer->addWidget(_search);
    connect(_search, &QLineEdit::textChanged, this, [this]
    {
      _group_panel->setVisible(_search->text().trimmed().isEmpty());
      _browser_heading->setText(_search->text().trimmed().isEmpty()
        ? _current_group : tr("Search results"));
      rebuild_tools();
    });

    auto* columns = new QHBoxLayout;
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(7);
    outer->addLayout(columns);

    _group_panel = new QWidget(_browser);
    _group_panel->setFixedWidth(130);
    _group_layout = new QVBoxLayout(_group_panel);
    _group_layout->setContentsMargins(0, 0, 0, 0);
    _group_layout->setSpacing(2);
    columns->addWidget(_group_panel);

    auto* right_panel = new QWidget(_browser);
    auto* right_layout = new QVBoxLayout(right_panel);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(4);
    _browser_heading = new QLabel(_current_group, right_panel);
    right_layout->addWidget(_browser_heading);

    auto* scroll = new QScrollArea(right_panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(300);
    scroll->setMaximumHeight(360);
    _tool_panel = new QWidget(scroll);
    _tool_layout = new QVBoxLayout(_tool_panel);
    _tool_layout->setContentsMargins(0, 0, 0, 0);
    _tool_layout->setSpacing(2);
    scroll->setWidget(_tool_panel);
    right_layout->addWidget(scroll);
    columns->addWidget(right_panel, 1);
  }

  bool toolbar::eventFilter(QObject* watched, QEvent* event)
  {
    if (event->type() == QEvent::Enter)
    {
      if (watched == _browser)
        _close_timer->stop();
      if (auto const group = _group_buttons.find(watched);
          group != _group_buttons.end() && _search->text().isEmpty())
      {
        _current_group = group->second;
        _browser_heading->setText(_current_group);
        for (auto const& [button, name] : _group_buttons)
          static_cast<QPushButton*>(button)->setChecked(name == _current_group);
        rebuild_tools();
      }
    }
    else if (event->type() == QEvent::Leave
             && (watched == _browser || _category_buttons.count(watched)))
    {
      schedule_browser_close();
    }
    else if (watched == _browser && event->type() == QEvent::Hide)
    {
      for (auto const& category_button : _category_buttons)
        static_cast<QToolButton*>(category_button.first)->setChecked(false);
    }
    return QToolBar::eventFilter(watched, event);
  }

  void toolbar::schedule_browser_close()
  {
    if (_browser->isVisible())
      _close_timer->start();
  }

  void toolbar::open_browser(QString const& category, QToolButton* source)
  {
    _close_timer->stop();
    _current_category = category;
    for (auto const& [button, name] : _category_buttons)
      static_cast<QToolButton*>(button)->setChecked(name == category);
    _search->clear();
    rebuild_groups();
    rebuild_tools();
    _browser->adjustSize();

    QPoint position = source->mapToGlobal(QPoint(source->width(), 0));
    if (QScreen* screen = QGuiApplication::screenAt(position))
    {
      QRect const available = screen->availableGeometry();
      position.setX(std::min(position.x(), available.right() - _browser->width()));
      position.setY(std::min(position.y(), available.bottom() - _browser->height()));
    }
    _browser->move(position);
    _browser->show();
    _browser->raise();
  }

  void toolbar::rebuild_groups()
  {
    _group_buttons.clear();
    clear_layout(_group_layout);
    QStringList groups;
    for (Entry const& entry : _entries)
    {
      if (entry.category == _current_category && !groups.contains(entry.group))
        groups.append(entry.group);
    }
    if (!groups.contains(_current_group) && !groups.isEmpty())
      _current_group = groups.first();

    _group_layout->addWidget(new QLabel(_current_category, _group_panel));
    for (QString const& group : groups)
    {
      auto* button = new QPushButton(group + QStringLiteral("  >"), _group_panel);
      button->setObjectName("noggitToolGroup");
      button->setCheckable(true);
      button->setChecked(group == _current_group);
      button->setMinimumHeight(35);
      button->installEventFilter(this);
      _group_buttons[button] = group;
      connect(button, &QPushButton::clicked, this, [this, group]
      {
        _current_group = group;
        _browser_heading->setText(group);
        for (auto const& [group_button, name] : _group_buttons)
          static_cast<QPushButton*>(group_button)->setChecked(name == _current_group);
        rebuild_tools();
      });
      _group_layout->addWidget(button);
    }
    _group_layout->addStretch();
    _browser_heading->setText(_current_group);
  }

  void toolbar::rebuild_tools()
  {
    clear_layout(_tool_layout);
    QString const query = _search->text().trimmed();
    int result_count = 0;
    for (Entry const& entry : _entries)
    {
      if (query.isEmpty())
      {
        if (entry.category != _current_category || entry.group != _current_group)
          continue;
      }
      else if (!entry.name.contains(query, Qt::CaseInsensitive)
               && !entry.category.contains(query, Qt::CaseInsensitive)
               && !entry.group.contains(query, Qt::CaseInsensitive))
      {
        continue;
      }

      ++result_count;
      auto* row = new QWidget(_tool_panel);
      auto* layout = new QHBoxLayout(row);
      layout->setContentsMargins(0, 0, 0, 0);
      layout->setSpacing(2);

      auto* select = new QPushButton(row);
      select->setObjectName("noggitToolChoice");
      select->setText(entry.name);
      select->setIcon(entry.icon);
      select->setMinimumHeight(36);
      select->setToolTip(entry.category + QStringLiteral(" -> ") + entry.group);
      if (entry.mode && _active_mode == entry.mode)
      {
        select->setCheckable(true);
        select->setChecked(true);
      }
      if (entry.checked)
      {
        select->setCheckable(true);
        select->setChecked(entry.checked());
      }
      connect(select, &QPushButton::clicked, this, [this, name = entry.name]
      {
        if (Entry const* selected = find_entry(name))
          select_entry(*selected);
      });
      layout->addWidget(select, 1);

      if (entry.mode && !shortcut_for(*entry.mode).isEmpty())
      {
        auto* shortcut = new QLabel(shortcut_for(*entry.mode), row);
        shortcut->setToolTip(tr("Keyboard shortcut"));
        layout->addWidget(shortcut);
      }
      else if (!entry.kind.isEmpty())
      {
        layout->addWidget(new QLabel(entry.kind, row));
      }

      auto* pin = new QToolButton(row);
      pin->setText(_pins.contains(entry.name)
        ? QString(QChar(0x2605)) : QString(QChar(0x2606)));
      pin->setEnabled(_pins.contains(entry.name) || _pins.size() < 4);
      pin->setToolTip(_pins.contains(entry.name) ? tr("Unpin from Quick Access")
                     : _pins.size() < 4 ? tr("Pin to Quick Access")
                                        : tr("Quick Access holds up to four items"));
      pin->setFixedWidth(26);
      connect(pin, &QToolButton::clicked, this, [this, name = entry.name]
      {
        toggle_pin(name);
      });
      layout->addWidget(pin);
      _tool_layout->addWidget(row);
    }

    if (result_count == 0)
      _tool_layout->addWidget(new QLabel(tr("No matching tools"), _tool_panel));
    _tool_layout->addStretch();
  }

  void toolbar::select_entry(Entry const& entry)
  {
    _browser->hide();
    if (entry.mode)
      _set_editing_mode(*entry.mode);
    else if (entry.callback)
      entry.callback();
  }

  void toolbar::toggle_pin(QString const& name)
  {
    if (_pins.contains(name))
      _pins.removeAll(name);
    else if (_pins.size() < 4)
      _pins.append(name);
    else
      return;

    _settings->setValue("map_view/tool_pins", _pins);
    rebuild_quick_access();
    // The star button belongs to a row rebuilt here. Defer deletion until its
    // click event has returned to Qt.
    QTimer::singleShot(0, this, [this]
    {
      if (_browser->isVisible())
        rebuild_tools();
    });
  }

  void toolbar::rebuild_quick_access()
  {
    clear_layout(_quick_layout);
    for (QString const& name : _pins)
    {
      Entry const* entry = find_entry(name);
      if (!entry)
        continue;
      auto* button = new QToolButton(this);
      button->setObjectName("noggitQuickTool");
      button->setText(fontMetrics().elidedText(
        QString(QChar(0x2605)) + QStringLiteral(" ") + name,
        Qt::ElideRight, 105));
      button->setIcon(entry->icon);
      button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
      button->setMinimumHeight(33);
      button->setToolTip(tr("Open %1").arg(name));
      connect(button, &QToolButton::clicked, this, [this, name]
      {
        if (Entry const* selected = find_entry(name))
          select_entry(*selected);
      });
      _quick_layout->addWidget(button);
    }
  }

  toolbar::Entry const* toolbar::find_entry(QString const& name) const
  {
    auto const match = std::find_if(_entries.begin(), _entries.end(),
      [&name](Entry const& entry) { return entry.name == name; });
    return match == _entries.end() ? nullptr : &*match;
  }

  toolbar::Entry const* toolbar::find_entry(editing_mode mode) const
  {
    auto const match = std::find_if(_entries.begin(), _entries.end(),
      [mode](Entry const& entry) { return entry.mode == mode; });
    return match == _entries.end() ? nullptr : &*match;
  }

  void toolbar::check_tool(editing_mode mode)
  {
    if (_active_mode && *_active_mode != mode)
    {
      _previous_mode = _active_mode;
      if (Entry const* previous = find_entry(*_previous_mode))
      {
        _previous_button->setText(fontMetrics().elidedText(
          QString(QChar(0x21B6)) + QStringLiteral(" ") + previous->name,
          Qt::ElideRight, 124));
        _previous_button->setToolTip(tr("Return to %1").arg(previous->name));
        _previous_button->setEnabled(true);
      }
    }
    _active_mode = mode;
    if (Entry const* active = find_entry(mode))
      _active_label->setText(tr("Active: %1").arg(active->name));
    if (_browser->isVisible())
      rebuild_tools();
  }

  void toolbar::add_command(QString category, QString group, QString name,
                            std::function<void()> callback, QString kind,
                            std::function<bool()> checked)
  {
    _entries.push_back(Entry{std::move(category), std::move(group), std::move(name),
                             {}, std::nullopt, std::move(callback),
                             std::move(checked), std::move(kind)});
    rebuild_quick_access();
  }
}
