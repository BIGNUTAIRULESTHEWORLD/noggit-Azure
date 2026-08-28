#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/project/ApplicationProjectReader.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
#include <noggit/ui/windows/projectSelection/components/CreateProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/LoadProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/RecentProjectsComponent.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>


#include <QFile>
#include <QFileDialog>
#include <QBitmap>
#include <QEvent>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPushButton>
#include <QSettings>
#include <QStyleOption>
#include <QStyleOptionButton>
#include <QString>
#include <QToolButton>
#include <QVector>

#include "ui_NoggitProjectSelectionWindow.h"

#include <filesystem>


using namespace Noggit::Ui::Windows;

namespace
{
  class ProjectSelectionTitleBar final : public QWidget
  {
  public:
    explicit ProjectSelectionTitleBar(QWidget* parent)
      : QWidget(parent)
    {
      setCursor(Qt::SizeAllCursor);
      setAutoFillBackground(false);
      setAttribute(Qt::WA_NoSystemBackground, true);
      setAttribute(Qt::WA_TranslucentBackground, true);
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override
    {
      if (event->button() == Qt::LeftButton)
      {
        _drag_offset = event->globalPos() - window()->frameGeometry().topLeft();
        _dragging = true;
        event->accept();
        return;
      }

      QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
      if (_dragging && (event->buttons() & Qt::LeftButton))
      {
        window()->move(event->globalPos() - _drag_offset);
        event->accept();
        return;
      }

      QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
      if (event->button() == Qt::LeftButton)
        _dragging = false;

      QWidget::mouseReleaseEvent(event);
    }

  private:
    QPoint _drag_offset;
    bool _dragging = false;
  };

  enum class FantasyTextRole
  {
    Title,
    Heading,
    Button
  };

  class WarcraftGlyphAtlas
  {
  public:
    WarcraftGlyphAtlas()
    {
      QImage const source(":/fonts/warcraft_alphabet");
      if (source.isNull())
      {
        LogError << "Unable to load the project selection alphabet image." << std::endl;
        return;
      }

      QVector<int> row_projection(source.height(), 0);
      for (int y = 0; y < source.height(); ++y)
      {
        for (int x = 0; x < source.width(); ++x)
        {
          if (isInk(source.pixel(x, y)))
            ++row_projection[y];
        }
      }

      QVector<QPair<int, int>> const rows = findRuns(row_projection, 5);
      int character_index = 0;

      for (auto const& row : rows)
      {
        QVector<int> column_projection(source.width(), 0);
        for (int x = 0; x < source.width(); ++x)
        {
          for (int y = row.first; y <= row.second; ++y)
          {
            if (isInk(source.pixel(x, y)))
              ++column_projection[x];
          }
        }

        QVector<QPair<int, int>> const columns = findRuns(column_projection, 2);
        for (auto const& column : columns)
        {
          if (character_index >= 26)
            break;

          QRect bounds(column.first, row.first,
                       column.second - column.first + 1,
                       row.second - row.first + 1);
          bounds.adjust(-1, -1, 1, 1);
          bounds = bounds.intersected(source.rect());

          QImage mask(bounds.size(), QImage::Format_ARGB32_Premultiplied);
          mask.fill(Qt::transparent);

          for (int y = 0; y < bounds.height(); ++y)
          {
            for (int x = 0; x < bounds.width(); ++x)
            {
              int const luminance = qGray(source.pixel(bounds.left() + x, bounds.top() + y));
              int const alpha = qBound(0, (205 - luminance) * 255 / 150, 255);
              mask.setPixelColor(x, y, QColor(255, 255, 255, alpha));
            }
          }

          _glyphs.insert(QChar('A' + character_index), mask);
          ++character_index;
        }
      }

      if (_glyphs.size() != 26)
      {
        LogError << "Project selection alphabet segmentation produced "
                 << _glyphs.size() << " glyphs instead of 26." << std::endl;
        _glyphs.clear();
      }
    }

    QImage renderTextMask(QString const& text, int requested_height, int maximum_width) const
    {
      if (_glyphs.isEmpty() || requested_height <= 0 || maximum_width <= 0)
        return {};

      QString const uppercase_text = text.toUpper();
      int glyph_height = requested_height;
      int text_width = measureText(uppercase_text, glyph_height);

      if (text_width > maximum_width)
      {
        glyph_height = qMax(8, glyph_height * maximum_width / text_width);
        text_width = measureText(uppercase_text, glyph_height);
      }

      QImage mask(QSize(qMax(1, text_width), glyph_height), QImage::Format_ARGB32_Premultiplied);
      mask.fill(Qt::transparent);

      QPainter painter(&mask);
      painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

      int x = 0;
      int const tracking = qMax(1, glyph_height / 12);
      int const space_width = qMax(3, glyph_height * 2 / 5);

      for (QChar const character : uppercase_text)
      {
        if (character.isSpace())
        {
          x += space_width;
          continue;
        }

        auto const glyph_it = _glyphs.constFind(character);
        if (glyph_it == _glyphs.constEnd())
        {
          x += space_width;
          continue;
        }

        QImage const& glyph = glyph_it.value();
        int const glyph_width = qMax(1, glyph.width() * glyph_height / glyph.height());
        painter.drawImage(QRect(x, 0, glyph_width, glyph_height), glyph);
        x += glyph_width + tracking;
      }

      return mask;
    }

  private:
    static bool isInk(QRgb pixel)
    {
      return qGray(pixel) < 180;
    }

    static QVector<QPair<int, int>> findRuns(QVector<int> const& projection, int minimum_count)
    {
      QVector<QPair<int, int>> runs;
      int run_start = -1;

      for (int i = 0; i < projection.size(); ++i)
      {
        bool const active = projection[i] >= minimum_count;
        if (active && run_start < 0)
          run_start = i;
        else if (!active && run_start >= 0)
        {
          runs.append(qMakePair(run_start, i - 1));
          run_start = -1;
        }
      }

      if (run_start >= 0)
        runs.append(qMakePair(run_start, projection.size() - 1));

      return runs;
    }

    int measureText(QString const& text, int glyph_height) const
    {
      int width = 0;
      int const tracking = qMax(1, glyph_height / 12);
      int const space_width = qMax(3, glyph_height * 2 / 5);
      bool has_glyph = false;

      for (QChar const character : text)
      {
        if (character.isSpace())
        {
          width += space_width;
          continue;
        }

        auto const glyph_it = _glyphs.constFind(character);
        if (glyph_it == _glyphs.constEnd())
        {
          width += space_width;
          continue;
        }

        QImage const& glyph = glyph_it.value();
        width += qMax(1, glyph.width() * glyph_height / glyph.height()) + tracking;
        has_glyph = true;
      }

      if (has_glyph)
        width -= tracking;

      return qMax(1, width);
    }

    QHash<QChar, QImage> _glyphs;
  };

  class FantasyTextEffect final : public QObject
  {
  public:
    explicit FantasyTextEffect(QObject* parent) : QObject(parent) {}

    void apply(QWidget* widget, FantasyTextRole role)
    {
      _roles.insert(widget, role);
      widget->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
      if (event->type() != QEvent::Paint)
        return QObject::eventFilter(watched, event);

      auto widget = qobject_cast<QWidget*>(watched);
      if (!widget || !_roles.contains(widget))
        return QObject::eventFilter(watched, event);

      QPainter painter(widget);
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setRenderHint(QPainter::TextAntialiasing, true);

      QString text;
      bool pressed = false;

      if (auto button = qobject_cast<QPushButton*>(widget))
      {
        QStyleOptionButton option;
        option.initFrom(button);
        option.rect = button->rect();
        option.features = QStyleOptionButton::None;

        if (button->isFlat())
          option.features |= QStyleOptionButton::Flat;
        if (button->isDefault())
          option.features |= QStyleOptionButton::DefaultButton;
        if (button->autoDefault())
          option.features |= QStyleOptionButton::AutoDefaultButton;

        if (button->isDown())
          option.state |= QStyle::State_Sunken;
        else
          option.state |= QStyle::State_Raised;

        if (button->isChecked())
          option.state |= QStyle::State_On;
        else
          option.state |= QStyle::State_Off;

        button->style()->drawControl(QStyle::CE_PushButtonBevel, &option, &painter, button);
        text = button->text();
        pressed = button->isDown();
      }
      else if (auto label = qobject_cast<QLabel*>(widget))
      {
        QStyleOption option;
        option.initFrom(label);
        label->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, label);
        text = label->text();
      }

      if (text.isEmpty())
        return true;

      bool const highlighted =
          _roles.value(widget) == FantasyTextRole::Button && widget->underMouse();
      drawFantasyText(painter, widget, text, pressed, highlighted, _roles.value(widget));
      return true;
    }

  private:
    static QImage tintMask(QImage const& mask, QBrush const& brush)
    {
      QImage tinted(mask.size(), QImage::Format_ARGB32_Premultiplied);
      tinted.fill(Qt::transparent);

      QPainter painter(&tinted);
      painter.fillRect(tinted.rect(), brush);
      painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
      painter.drawImage(0, 0, mask);
      return tinted;
    }

    static void drawFantasyText(QPainter& painter,
                                QWidget* widget,
                                QString const& text,
                                bool pressed,
                                bool highlighted,
                                FantasyTextRole role)
    {
      QRect const content_rect = widget->rect().adjusted(9, 5, -9, -5);
      int const requested_height = role == FantasyTextRole::Heading
          ? qMax(18, widget->font().pixelSize() + 3)
          : qMax(16, widget->font().pixelSize() + 2);

      static WarcraftGlyphAtlas const atlas;
      QImage const mask = atlas.renderTextMask(text, requested_height, content_rect.width() - 8);
      if (mask.isNull())
        return;

      QPoint origin(content_rect.center().x() - mask.width() / 2,
                    content_rect.center().y() - mask.height() / 2 + (pressed ? 2 : 0));

      bool const azure_title = role == FantasyTextRole::Title;

      QImage const shadow = tintMask(mask, azure_title
                                              ? QColor(0, 7, 29, 235)
                                              : QColor(14, 5, 0, 225));
      painter.drawImage(origin + QPoint(3, 4), shadow);

      if (azure_title)
      {
        QImage const glow = tintMask(mask, QColor(66, 123, 255, 115));
        for (int y = -3; y <= 3; ++y)
        {
          for (int x = -3; x <= 3; ++x)
          {
            if (qAbs(x) + qAbs(y) >= 3)
              painter.drawImage(origin + QPoint(x, y), glow);
          }
        }
      }

      QImage const outline = tintMask(mask, azure_title
                                               ? QColor(4, 25, 67)
                                               : QColor(55, 18, 0));
      int const outline_radius = role == FantasyTextRole::Button ? 1 : 2;
      for (int y = -outline_radius; y <= outline_radius; ++y)
      {
        for (int x = -outline_radius; x <= outline_radius; ++x)
        {
          if (x != 0 || y != 0)
            painter.drawImage(origin + QPoint(x, y), outline);
        }
      }

      QLinearGradient fill(0.0, 0.0, 0.0, mask.height());

      if (azure_title)
      {
        fill.setColorAt(0.00, QColor(230, 250, 255));
        fill.setColorAt(0.20, QColor(112, 222, 255));
        fill.setColorAt(0.55, QColor(30, 133, 242));
        fill.setColorAt(0.82, QColor(31, 74, 190));
        fill.setColorAt(1.00, QColor(122, 92, 246));
      }
      else if (widget->isEnabled())
      {
        fill.setColorAt(0.00, highlighted ? QColor(255, 249, 152) : QColor(255, 235, 103));
        fill.setColorAt(0.22, highlighted ? QColor(255, 204, 45) : QColor(255, 180, 20));
        fill.setColorAt(0.58, highlighted ? QColor(245, 133, 5) : QColor(220, 103, 0));
        fill.setColorAt(0.84, highlighted ? QColor(185, 70, 0) : QColor(137, 48, 0));
        fill.setColorAt(1.00, highlighted ? QColor(255, 184, 25) : QColor(223, 126, 5));
      }
      else
      {
        fill.setColorAt(0.0, QColor(126, 151, 179));
        fill.setColorAt(0.5, QColor(75, 96, 121));
        fill.setColorAt(1.0, QColor(39, 53, 71));
      }

      QImage const fantasy_fill = tintMask(mask, fill);
      painter.drawImage(origin, fantasy_fill);

      QImage const top_highlight = tintMask(mask, widget->isEnabled()
                                                  ? (azure_title
                                                       ? QColor(225, 253, 255, 125)
                                                       : QColor(255, 239, 135, 95))
                                                  : QColor(170, 190, 210, 50));
      painter.setClipRect(QRect(origin, QSize(mask.width(), qMax(1, mask.height() / 4))));
      painter.drawImage(origin, top_highlight);
      painter.setClipping(false);
    }

    QHash<QWidget*, FantasyTextRole> _roles;
  };
}

NoggitProjectSelectionWindow::NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app,
                                                           QWidget* parent)
  : QMainWindow(parent)
  , _ui(new ::Ui::NoggitProjectSelectionWindow)
  , _noggit_application(noggit_app)
{
  // The artwork supplies the complete window frame, so do not wrap it in a
  // second rectangular Windows title bar and border.
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground, true);

  ////////////////////////////
  // auto load favorite project
  QSettings settings;
  int favorite_proj_idx = settings.value("favorite_project", -1).toInt();

  bool load_favorite = settings.value("auto_load_fav_project", true).toBool();

  // if it has client data, it means it already loaded before and we exited through the menu, skip autoloading favorite
  if (noggit_app->hasClientData())
      load_favorite = false;

  if (load_favorite && favorite_proj_idx != -1)
  {
    Log << "Auto loading favorite project index : " << favorite_proj_idx << std::endl;

    int size = settings.beginReadArray("recent_projects");

    QString project_final_path;

    // for (int i = 0; i < size; ++i)
    if (size > favorite_proj_idx)
    {
      settings.setArrayIndex(favorite_proj_idx);
      std::filesystem::path project_path = settings.value("project_path").toString().toStdString().c_str();

      if (std::filesystem::exists(project_path) && std::filesystem::is_directory(project_path))
      {
        auto project_reader = Noggit::Project::ApplicationProjectReader();
        
        auto project = project_reader.readProject(project_path);
        
        if (project.has_value())
        {
          // project->projectVersion;
          // project_directory = QString::fromStdString(project_path.generic_string());
          // auto project_name = QString::fromStdString(project->ProjectName);

          project_final_path = QString(project_path.string().c_str());
        }
      }
    }
    settings.endArray();

    if (!project_final_path.isEmpty())
    {
      auto selected_project = _load_project_component->loadProject(this, project_final_path);

      if (!selected_project)
      {
        LogError << "Selected Project is null, favorite loading failed." << std::endl;
      }
      else
      {
        Noggit::Project::CurrentProject::initialize(selected_project.get());

        _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
            _noggit_application->getConfiguration(),
            selected_project);
        _project_selection_page->showMaximized();

        close();
        return;
      }
    }
  }
  ///////////////////////////

  _ui->setupUi(this);

  // Preserve the design coordinate system used by the full-screen frame art.
  // The former native title bar made the outer rectangular window conspicuous;
  // this keeps the decorative frame itself as the visible boundary.
  setFixedSize(size());

  QImage const frame_mask_source(":/project-selection-azure-frame");
  if (!frame_mask_source.isNull())
  {
    QImage scaled_mask = frame_mask_source.scaled(
        size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32);

    // The shaped artwork contains intentional translucent lighting inside the
    // frame. A binary window mask must treat every non-transparent artwork
    // pixel as solid or those highlights become holes in the window.
    for (int y = 0; y < scaled_mask.height(); ++y)
    {
      QRgb* scan_line = reinterpret_cast<QRgb*>(scaled_mask.scanLine(y));
      for (int x = 0; x < scaled_mask.width(); ++x)
      {
        if (qAlpha(scan_line[x]) != 0)
          scan_line[x] = qRgba(qRed(scan_line[x]), qGreen(scan_line[x]), qBlue(scan_line[x]), 255);
      }
    }

    setMask(QBitmap::fromImage(scaled_mask.createAlphaMask()));
  }

  _ui->label->setObjectName("title");
  _ui->label_2->setObjectName("title");

  // The generated chrome is the visual source of truth for this screen. The
  // widgets below remain native and interactive, but sit over its empty slots.
  _ui->titlePlaque->hide();
  _ui->label->hide();
  _ui->label_2->hide();

  _ui->rootLayout->setContentsMargins(0, 0, 0, 0);
  _ui->rootLayout->setSpacing(0);
  _ui->contentLayout->setContentsMargins(15, 0, 15, 0);
  _ui->contentLayout->setSpacing(0);
  _ui->contentLayout->setStretch(0, 29);
  _ui->contentLayout->setStretch(1, 43);
  _ui->contentLayout->setStretch(2, 28);

  for (QFrame* panel : {_ui->recentPanel, _ui->actionsPanel})
  {
    panel->setMinimumWidth(0);
    panel->setMaximumWidth(QWIDGETSIZE_MAX);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  _ui->heroWindow->setMinimumWidth(0);
  _ui->recentLayout->setContentsMargins(54, 174, 20, 112);
  _ui->recentLayout->setSpacing(0);
  _ui->listView->setSpacing(11);

  _ui->actionsLayout->setContentsMargins(0, 0, 0, 0);
  _ui->actionsLayout->setSpacing(0);

  auto configure_embedded_button = [](QPushButton* button,
                                      QString const& accessible_name)
  {
    button->setAccessibleName(accessible_name);
    button->setToolTip(accessible_name);
    button->setText(QString());
  };

  configure_embedded_button(_ui->button_create_new_project, "Create a new project");
  configure_embedded_button(_ui->button_open_existing_project, "Open an existing project");
  configure_embedded_button(_ui->button_convert_project, "Convert project");

  // These controls overlay frames that are part of the background artwork.
  // Keep their hit/highlight rectangles locked to those painted coordinates
  // rather than allowing the generic panel layout to stretch them.
  for (QPushButton* button : {_ui->button_create_new_project,
                              _ui->button_open_existing_project,
                              _ui->button_convert_project})
  {
    _ui->actionsLayout->removeWidget(button);
    button->setParent(_ui->centralwidget);
    button->show();
  }

  _ui->button_create_new_project->setGeometry(933, 190, 278, 105);
  _ui->button_open_existing_project->setGeometry(933, 320, 278, 105);
  _ui->button_convert_project->setGeometry(933, 450, 278, 105);

  _ui->footerLayout->removeWidget(_ui->settings_button);
  _ui->settings_button->setParent(_ui->centralwidget);
  _ui->settings_button->setGeometry(1136, 584, 96, 96);
  _ui->settings_button->show();

  auto title_bar = new ProjectSelectionTitleBar(_ui->centralwidget);
  title_bar->setObjectName("projectSelectionTitleBar");
  title_bar->setGeometry(0, 0, width(), 145);
  title_bar->raise();

  auto exit_button = new QPushButton("Exit Project", title_bar);
  exit_button->setObjectName("projectSelectionExitButton");
  exit_button->setAccessibleName("Exit project");
  exit_button->setToolTip("Exit Noggit Azure");
  exit_button->setCursor(Qt::ArrowCursor);
  exit_button->setGeometry(title_bar->width() - 250, 34, 140, 36);
  QObject::connect(exit_button, &QPushButton::clicked, this, &QWidget::close);

  _ui->centralwidget->setStyleSheet(R"(
    QWidget#centralwidget {
      color: #dbeeff;
      background: transparent;
      border-image: url(:/project-selection-azure-artwork) 0 0 0 0 stretch stretch;
      border: none;
    }

    QFrame#mainFrame {
      background: transparent;
      border: none;
    }

    QFrame#recentPanel,
    QFrame#actionsPanel {
      background: transparent;
      border: none;
    }

    QFrame#heroWindow {
      background: transparent;
      border: none;
    }

    QLabel#titlePlaque {
      color: #78dcff;
      background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                  stop:0 #263d73, stop:0.18 #0d1735,
                                  stop:0.76 #080d26, stop:1 #281b58);
      border: 3px solid #527bd2;
      border-top-color: #8bdcff;
      border-bottom-color: #21184d;
      border-radius: 7px;
      padding: 5px 24px;
      font-size: 26px;
    }

    QLabel#title {
      color: #f1c94f;
      background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                  stop:0 rgba(7, 20, 59, 165),
                                  stop:0.5 rgba(22, 13, 60, 190),
                                  stop:1 rgba(7, 20, 59, 165));
      border: 1px solid #526fc2;
      border-bottom-color: #6c47ba;
      border-radius: 4px;
      padding: 7px 8px;
      font-size: 19px;
    }

    QListWidget#listView {
      background: transparent;
      border: none;
      outline: none;
      padding: 1px;
    }

    QListWidget#listView::item {
      color: #dbeeff;
      background: transparent;
      border: 1px solid transparent;
      border-radius: 6px;
    }

    QListWidget#listView::item:hover {
      background-color: rgba(34, 115, 206, 48);
      border-color: rgba(121, 216, 255, 155);
    }

    QListWidget#listView::item:selected {
      background-color: rgba(37, 104, 209, 72);
      border-color: #9ce9ff;
    }

    QLabel#project-title-label {
      color: #e7cb75;
      background: transparent;
      font-family: Georgia, serif;
      font-size: 15px;
      font-weight: bold;
    }

    QLabel#project-information,
    QLabel#project-last-edited {
      color: #a9c8e7;
      background: transparent;
      font-size: 10px;
    }

    QPushButton {
      color: transparent;
      background: transparent;
      border: 1px solid transparent;
      border-radius: 9px;
    }

    QPushButton:hover {
      background-color: rgba(40, 133, 241, 42);
      border-color: rgba(155, 232, 255, 190);
    }

    QPushButton:pressed {
      background-color: rgba(43, 48, 151, 78);
      border-color: rgba(125, 113, 235, 210);
    }

    QPushButton:focus {
      border-color: #a7ecff;
    }

    QPushButton:disabled {
      background: transparent;
      border-color: transparent;
    }

    QToolButton#settings_button {
      color: transparent;
      background: transparent;
      border: 1px solid transparent;
      border-radius: 47px;
      padding: 0px;
    }

    QToolButton#settings_button:hover {
      background-color: rgba(35, 95, 162, 55);
      border-color: rgba(150, 230, 255, 195);
    }

    QPushButton#projectSelectionExitButton {
      color: #e7cb75;
      background: rgba(4, 10, 29, 125);
      border: 1px solid rgba(104, 151, 220, 110);
      border-radius: 8px;
      font-family: Georgia, serif;
      font-size: 13px;
      font-weight: bold;
      padding: 0px;
    }

    QPushButton#projectSelectionExitButton:hover {
      color: #fff2a6;
      background: rgba(40, 103, 187, 150);
      border-color: rgba(155, 232, 255, 220);
    }

    QPushButton#projectSelectionExitButton:pressed {
      background: rgba(43, 48, 151, 190);
    }

    QScrollBar:vertical {
      background: #080d25;
      width: 12px;
      margin: 1px;
      border: 1px solid #344f8c;
    }

    QScrollBar::handle:vertical {
      background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                  stop:0 #234da0, stop:1 #57339a);
      border: 1px solid #6bbfe9;
      border-radius: 3px;
      min-height: 26px;
    }

    QScrollBar::add-line:vertical,
    QScrollBar::sub-line:vertical {
      height: 0px;
    }
  )");

  _settings = new Noggit::Ui::settings(this);
  //_changelog = new Noggit::Ui::CChangelog(this);

  _load_project_component = std::make_unique<Component::LoadProjectComponent>();

  _ui->settings_button->setIcon(QIcon());

  _ui->changelog_button->hide();
  //_ui->changelog_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::file));
  //_ui->changelog_button->setIconSize(QSize(20, 20));
  //_ui->changelog_button->setText(tr(" Changelog"));
  //_ui->changelog_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  Component::RecentProjectsComponent::buildRecentProjectsList(this);

  QObject::connect(_ui->settings_button, &QToolButton::clicked, [&]
      {
          _settings->show();
      }
  );

  /*QObject::connect(_ui->changelog_button, &QToolButton::clicked, [&]()
      {
          _changelog->SelectFirst();
          _changelog->show();
      });*/

  QObject::connect(_ui->button_create_new_project, &QPushButton::clicked, [=, this]
                   {
                     ProjectInformation project_reference;
                     NoggitProjectCreationDialog project_creation_dialog(project_reference, this);

                     QObject::connect(&project_creation_dialog,  &QDialog::finished, [&project_reference, this](int result)
                     {
                       if (result != QDialog::Accepted)
                         return;

                       Component::CreateProjectComponent::createProject(this, project_reference);
                       resetFavoriteProject();
                       Component::RecentProjectsComponent::buildRecentProjectsList(this);
                     });

                     project_creation_dialog.exec();
                     project_creation_dialog.setFixedSize(project_creation_dialog.size());

                   }
  );

  QObject::connect(_ui->button_open_existing_project, &QPushButton::clicked, [=]
                   {
                     auto project_reader = Noggit::Project::ApplicationProjectReader();

                     QString proj_file = QFileDialog::getOpenFileName(this, "Open File",
                                                                     "/",
                                                                     "*.noggitproj");

                     if (proj_file.isEmpty())
                     {
                       QMessageBox::critical(this, "Error", "Failed to read project: project file is empty");
                       return;
                     }


                     std::filesystem::path filepath(proj_file.toStdString());

                     auto project = project_reader.readProject(filepath.parent_path());

                     if (!project.has_value())
                     {
                       QMessageBox::critical(this, "Error", "Failed to read project");
                       return;
                     }

                     Component::RecentProjectsComponent::registerProjectChange(filepath.parent_path().string());

                     auto application_configuration = _noggit_application->getConfiguration();
                     auto application_projects_folder_path = std::filesystem::path(application_configuration->ApplicationProjectPath);
                     auto application_project_service = Noggit::Project::ApplicationProject(application_configuration);

                     auto project_to_launch = application_project_service.loadProject(filepath.parent_path());

                     if (!project_to_launch)
                     {
                       return;
                     }

                     Noggit::Application::NoggitApplication::instance()->setClientData(project_to_launch->ClientData);

                     Noggit::Project::CurrentProject::initialize(project_to_launch.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         project_to_launch);
                     _project_selection_page->showMaximized();

                     close();
                   }
  );

  QObject::connect(_ui->listView, &QListView::doubleClicked, [=]
                   {
                     auto selected_project = _load_project_component->loadProject(this);

                     if (!selected_project)
                     {
                       LogError << "Selected Project is null, loading failed." << std::endl;
                       return;
                     }

                     Noggit::Project::CurrentProject::initialize(selected_project.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         selected_project);
                         _project_selection_page->showMaximized();

                     close();
                   }
  );

  // !disable-update && !force-changelog
  /*if (!_noggit_application->GetCommand(0) && !_noggit_application->GetCommand(1))
  {
      _updater = new Noggit::Ui::CUpdater(this);

      QObject::connect(_updater, &CUpdater::OpenUpdater, [=]()
          {
              _updater->setModal(true);
              _updater->show();
          });
  }*/

  // auto _set = new QSettings(this);
  //auto first_changelog = _set->value("first_changelog", false);

  // force-changelog
  /*if (_noggit_application->GetCommand(1) || !first_changelog.toBool())
  {
      _changelog->setModal(true);
      _changelog->show();

      if (!first_changelog.toBool())
      {
          _set->setValue("first_changelog", true);
          _set->sync();
      }
  }*/
  show();
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemDelete(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Delete Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Deleting a project will remove all saved data. Do you want to continue?");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
    {
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      QFile folder(project_path.c_str());
      folder.moveToTrash();
      break;
    }
    case QMessageBox::DestructiveRole:
    default:
      break;
  }
  resetFavoriteProject();

  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemForget(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Forget Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Data on the disk will not be removed, this action will only hide the project. Continue?.");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      break;
    case QMessageBox::DestructiveRole:
    default:
      break;
  }

  resetFavoriteProject();
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::handleContextMenuProjectListItemFavorite(int index)
{
  QSettings settings;
  settings.sync();
  settings.setValue("favorite_project", index);
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::resetFavoriteProject()
{
    QSettings settings;
    settings.sync();
    settings.setValue("favorite_project", -1);
}

NoggitProjectSelectionWindow::~NoggitProjectSelectionWindow()
{
  delete _ui;
}

