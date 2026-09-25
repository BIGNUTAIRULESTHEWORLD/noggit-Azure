// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Tool.hpp>

#include "ToolPanel.hpp"

#include <QBoxLayout>
#include <QSignalBlocker>
#include <QTabBar>

using namespace Noggit::Ui::Tools;

ToolPanel::ToolPanel(QWidget* parent)
  : QDockWidget(parent)
{
  auto body = new QWidget(this);
  _ui.setupUi(body);
  setWidget(body);
  setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  layout()->setAlignment(Qt::AlignTop);
  setMinimumWidth(250 + 15);
  setStyleSheet(R"(
    QDockWidget { background: #0c192d; color: #e9edf4; }
    QDockWidget::title { background: #14233e; color: #e9edf4; }
    QDockWidget::close-button, QDockWidget::float-button { background: #14233e; }
    QDockWidget::close-button:hover, QDockWidget::float-button:hover {
      background: #294565;
    }
    QWidget { background: #14233e; color: #e9edf4; }
    QWidget#toolPanel, QWidget#scrollHolder, QWidget#scrollAreaWidgetContents,
    QScrollArea, QAbstractScrollArea::viewport { background: #14233e; }
    QTabWidget::pane { background: #14233e; border: 1px solid #405c7c; }
    QGroupBox { background: #182943; border-color: #405c7c; color: #d6b777; }
    QGroupBox::title {
      color: #d6b777; border-top-color: #405c7c;
      border-bottom-color: #d6b777;
    }
    QTabBar::tab { background: #182943; border-color: #405c7c; }
    QTabBar::tab:hover { background: #294565; border-top-color: #6fa9df; }
    QTabBar::tab:selected { background: #294565; border-top-color: #d6b777; }
    QLineEdit, QAbstractSpinBox, QComboBox:editable {
      background: #0c192d; color: #e9edf4;
      selection-background-color: #315f96;
    }
    QComboBox:!editable { background: #223958; color: #e9edf4; }
    QComboBox:!editable:on { background: #294565; }
    QComboBox QAbstractItemView {
      background: #14233e; color: #e9edf4;
      selection-background-color: #294565;
    }
    QPushButton, QToolButton {
      background: #223958; color: #e9edf4; border-color: #405c7c;
    }
    QPushButton:hover, QToolButton:hover {
      background: #294565; border-color: #6fa9df;
    }
    QPushButton:pressed, QToolButton:pressed, QToolButton:checked {
      background: #294565; border-color: #d6b777;
    }
    QSlider::groove:horizontal, QSlider::groove:vertical,
    QSlider::add-page:horizontal, QSlider::add-page:vertical {
      background: #253a55; border-color: #405c7c;
    }
    QSlider::handle:horizontal, QSlider::handle:vertical {
      background: #6fa9df; border-color: #14233e;
    }
    QSlider::sub-page:horizontal, QSlider::sub-page:vertical {
      background: #315f96; border-color: #405c7c;
    }
    QScrollBar:horizontal, QScrollBar:vertical { background: #0c192d; }
    QScrollBar::handle:horizontal, QScrollBar::handle:vertical {
      background: #405c7c;
    }
  )");

  _object_modes = new QTabBar(_ui.scrollHolder);
  _object_modes->addTab(tr("Objects"));
  _object_modes->addTab(tr("Fence Builder"));
  _object_modes->setExpanding(false);
  static_cast<QBoxLayout*>(_ui.scrollHolder->layout())->insertWidget(0, _object_modes);
  _object_modes->hide();
  connect(_object_modes, &QTabBar::currentChanged, this, [this](int index)
  {
    emit objectModeRequested(index == 0 ? editing_mode::object : editing_mode::fence);
  });
}

void ToolPanel::setCurrentTool(editing_mode mode)
{
  bool const object_mode = mode == editing_mode::object || mode == editing_mode::fence;
  if (object_mode)
  {
    QSignalBlocker const blocker(_object_modes);
    _object_modes->setCurrentIndex(mode == editing_mode::object ? 0 : 1);
  }
  _object_modes->setVisible(object_mode);

  for (auto&& [tool, widget] : _tools)
  {
    if (tool->editingMode() == mode)
    {
      widget->setVisible(true);
    }
    else
    {
      widget->setVisible(false);
    }
  }

  _ui.scrollAreaWidgetContents->adjustSize();
}

void ToolPanel::registerTool(Tool* tool, QWidget* widget)
{
  _ui.scrollAreaWidgetContents->layout()->addWidget(widget);
  _tools.emplace_back(std::make_pair(tool, widget));
}
