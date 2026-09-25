#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/project/ApplicationProjectReader.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/UiColorMode.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
#include <noggit/ui/windows/projectSelection/components/CreateProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/LoadProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/RecentProjectsComponent.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>


#include <QFile>
#include <QCheckBox>
#include <QFileDialog>
#include <QDir>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPushButton>
#include <QPixmap>
#include <QSettings>
#include <QStyleOption>
#include <QStyleOptionButton>
#include <QString>
#include <QToolButton>
#include <QVector>
#include <QVBoxLayout>

#include "ui_NoggitProjectSelectionWindow.h"

#include <filesystem>


using namespace Noggit::Ui::Windows;

namespace
{
  class ProjectWorldPreview final : public QWidget
  {
  public:
    explicit ProjectWorldPreview(QWidget* parent) : QWidget(parent)
    {
      setMinimumHeight(270);
      setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
      QPainter painter(this);
      painter.setRenderHint(QPainter::SmoothPixmapTransform);
      QRectF const bounds = rect().adjusted(3, 3, -3, -3);
      // Cropped from Blizzard's 2001 WoW wallpaper, archived at
      // https://archivecraft.com/archives/wallpapers/vanilla/.
      static QPixmap const artwork(QStringLiteral(":/project-selection-canyon"));
      painter.fillRect(bounds, QColor(12, 18, 32));
      if (!artwork.isNull() && bounds.width() > 0 && bounds.height() > 0)
      {
        qreal const target_aspect = bounds.width() / bounds.height();
        qreal const artwork_aspect = qreal(artwork.width()) / artwork.height();
        QRectF source(0, 0, artwork.width(), artwork.height());
        if (artwork_aspect > target_aspect)
        {
          qreal const source_width = artwork.height() * target_aspect;
          source.setLeft((artwork.width() - source_width) / 2.0);
          source.setWidth(source_width);
        }
        else
        {
          qreal const source_height = artwork.width() / target_aspect;
          source.setTop((artwork.height() - source_height) / 2.0);
          source.setHeight(source_height);
        }
        painter.drawPixmap(bounds, artwork, source);
      }

      painter.setPen(QPen(QColor(210, 183, 117), 1));
      painter.drawRect(bounds);
    }
  };

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
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  _load_project_component = std::make_unique<Component::LoadProjectComponent>();

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
  setFixedSize(size());

  _ui->rootLayout->setContentsMargins(10, 10, 10, 10);
  _ui->rootLayout->setSpacing(0);
  _ui->titlePlaque->setMinimumSize(500, 86);
  _ui->titlePlaque->setMaximumSize(530, 86);
  _ui->titlePlaque->setText(QStringLiteral("NOGGIT AZURE"));
  _ui->contentLayout->setContentsMargins(0, 0, 0, 0);
  _ui->contentLayout->setSpacing(0);
  _ui->contentLayout->setStretch(0, 27);
  _ui->contentLayout->setStretch(1, 45);
  _ui->contentLayout->setStretch(2, 27);

  for (QFrame* panel : {_ui->recentPanel, _ui->actionsPanel})
  {
    panel->setMinimumWidth(0);
    panel->setMaximumWidth(QWIDGETSIZE_MAX);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  _ui->recentLayout->setContentsMargins(18, 23, 18, 18);
  _ui->recentLayout->setSpacing(8);
  _ui->label->setText(QStringLiteral("Recent Projects"));
  _ui->label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  auto recent_note = new QLabel(QStringLiteral("Return to a world in progress"), _ui->recentPanel);
  recent_note->setObjectName("sideNote");
  _ui->recentLayout->insertWidget(1, recent_note);
  _ui->listView->setSpacing(8);

  auto hero_layout = new QVBoxLayout(_ui->heroWindow);
  hero_layout->setContentsMargins(18, 18, 18, 18);
  hero_layout->setSpacing(12);
  hero_layout->addWidget(new ProjectWorldPreview(_ui->heroWindow), 1);

  auto project_details = new QFrame(_ui->heroWindow);
  project_details->setObjectName("projectDetails");
  auto details_layout = new QVBoxLayout(project_details);
  details_layout->setContentsMargins(18, 12, 18, 12);
  details_layout->setSpacing(3);
  auto project_caption = new QLabel(QStringLiteral("SELECTED PROJECT"), project_details);
  project_caption->setObjectName("projectCaption");
  auto project_name = new QLabel(QStringLiteral("Select a project"), project_details);
  project_name->setObjectName("selectedProjectName");
  project_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto project_meta = new QLabel(QStringLiteral("Choose a project from Recent Projects"), project_details);
  project_meta->setObjectName("selectedProjectMeta");
  project_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  project_meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details_layout->addWidget(project_caption);
  details_layout->addWidget(project_name);
  details_layout->addWidget(project_meta);
  hero_layout->addWidget(project_details);

  _ui->actionsLayout->setContentsMargins(18, 23, 18, 18);
  _ui->actionsLayout->setSpacing(12);
  _ui->label_2->setText(QStringLiteral("Begin"));
  _ui->label_2->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  auto actions_note = new QLabel(QStringLiteral("Choose how to enter Noggit"), _ui->actionsPanel);
  actions_note->setObjectName("sideNote");
  _ui->actionsLayout->insertWidget(1, actions_note);
  auto launch_button = new QPushButton(QStringLiteral("Enter Selected Project"), _ui->actionsPanel);
  launch_button->setObjectName("launchSelectedProject");
  launch_button->setMinimumHeight(56);
  launch_button->setEnabled(false);
  _ui->actionsLayout->insertWidget(2, launch_button);
  _ui->button_create_new_project->setText(QStringLiteral("Create New Project"));
  _ui->button_open_existing_project->setText(QStringLiteral("Open Project File"));
  _ui->button_create_new_project->setMinimumHeight(56);
  _ui->button_open_existing_project->setMinimumHeight(56);
  _ui->button_convert_project->hide();
  _ui->changelog_button->hide();
  _ui->settings_button->setIcon(QIcon());
  _ui->settings_button->setText(QStringLiteral("Settings"));
  _ui->settings_button->setMinimumSize(88, 36);
  auto dark_mode_toggle = new QCheckBox(tr("Dark mode"), _ui->actionsPanel);
  dark_mode_toggle->setObjectName(QStringLiteral("projectColorModeToggle"));
  dark_mode_toggle->setChecked(!Noggit::Ui::azureColorMode());
  dark_mode_toggle->setToolTip(tr("Checked: Dark colors. Unchecked: Azure colors."));
  _ui->footerLayout->insertWidget(2, dark_mode_toggle);

  auto title_bar = new ProjectSelectionTitleBar(_ui->centralwidget);
  title_bar->setObjectName("projectSelectionTitleBar");
  title_bar->setGeometry(10, 10, width() - 20, 86);
  title_bar->raise();
  auto exit_button = new QPushButton(QStringLiteral("Exit"), title_bar);
  exit_button->setObjectName("projectSelectionExitButton");
  exit_button->setToolTip(QStringLiteral("Exit Noggit Azure"));
  exit_button->setCursor(Qt::ArrowCursor);
  exit_button->setGeometry(title_bar->width() - 92, 22, 76, 36);
  QObject::connect(exit_button, &QPushButton::clicked, this, &QWidget::close);

  _ui->centralwidget->setStyleSheet(R"(
    QWidget#centralwidget {
      color: #eee8da;
      background: qradialgradient(cx:0.5, cy:0.35, radius:0.9,
                                  stop:0 #1c2b4c, stop:0.6 #101b31, stop:1 #080e1b);
      border: 3px solid #64738c;
    }
    QLabel#titlePlaque {
      color: #b0d9ef;
      background: transparent;
      border: none;
      font-family: Georgia;
      font-size: 31px;
      font-weight: bold;
    }
    QFrame#mainFrame {
      background: #0b1426;
      border: 1px solid #d6b777;
    }
    QFrame#recentPanel, QFrame#actionsPanel {
      background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                  stop:0 #14233e, stop:1 #0b1426);
      border: none;
    }
    QFrame#recentPanel { border-right: 1px solid #526783; }
    QFrame#actionsPanel { border-left: 1px solid #526783; }
    QFrame#heroWindow { background: #0a1326; border: none; }
    QLabel#label, QLabel#label_2 {
      color: #d6b777;
      font-family: Georgia;
      font-size: 21px;
      font-weight: bold;
      border-bottom: 1px solid #526783;
      padding-bottom: 7px;
    }
    QLabel#sideNote {
      color: #aabbd0;
      font-family: Segoe UI;
      font-size: 11px;
      margin-bottom: 10px;
    }
    QListWidget#listView {
      background: transparent;
      border: none;
      outline: none;
    }
    QListWidget#listView::item {
      background: #172944;
      border: 1px solid #526783;
      margin-bottom: 3px;
    }
    QListWidget#listView::item:hover,
    QListWidget#listView::item:selected {
      background: #263c61;
      border: 1px solid #d6b777;
    }
    QLabel#project-title-label {
      color: #eee8da;
      background: transparent;
      font-family: Georgia;
      font-size: 14px;
    }
    QLabel#project-information, QLabel#project-last-edited {
      color: #aabbd0;
      background: transparent;
      font-family: Segoe UI;
      font-size: 10px;
    }
    QFrame#projectDetails {
      background: #14233e;
      border: 1px solid #526783;
    }
    QLabel#projectCaption {
      color: #d6b777;
      font-family: Segoe UI;
      font-size: 10px;
    }
    QLabel#selectedProjectName {
      color: #eee8da;
      font-family: Georgia;
      font-size: 22px;
    }
    QLabel#selectedProjectMeta {
      color: #aabbd0;
      font-family: Segoe UI;
      font-size: 11px;
    }
    QPushButton#launchSelectedProject,
    QPushButton#button_create_new_project,
    QPushButton#button_open_existing_project {
      color: #eee8da;
      background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                  stop:0 #253a60, stop:1 #101d35);
      border: 1px solid #526783;
      font-family: Georgia;
      font-size: 15px;
      padding: 8px;
      text-align: left;
    }
    QPushButton#launchSelectedProject {
      color: #f6e3b5;
      background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                  stop:0 #315f96, stop:1 #173358);
      border-color: #d6b777;
    }
    QPushButton#launchSelectedProject:disabled {
      color: #7e8b9c;
      background: #1c293c;
      border-color: #526783;
    }
    QPushButton#launchSelectedProject:hover,
    QPushButton#button_create_new_project:hover,
    QPushButton#button_open_existing_project:hover {
      background: #365581;
      border-color: #d6b777;
    }
    QToolButton#settings_button, QPushButton#projectSelectionExitButton {
      color: #aabbd0;
      background: transparent;
      border: none;
      font-family: Segoe UI;
      font-size: 12px;
    }
    QToolButton#settings_button:hover,
    QPushButton#projectSelectionExitButton:hover { color: #d6b777; }
    QCheckBox#projectColorModeToggle {
      color: #eee8da;
      background: transparent;
      font-family: Segoe UI;
      font-size: 12px;
    }
    QScrollBar:vertical { background: #0b1426; width: 10px; }
    QScrollBar::handle:vertical { background: #526783; min-height: 26px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
  )");

  _settings = new Noggit::Ui::settings(this);
  QString const azure_project_style = _ui->centralwidget->styleSheet();
  _ui->centralwidget->setStyleSheet(Noggit::Ui::projectColorStyle(azure_project_style));
  connect(dark_mode_toggle, &QCheckBox::toggled, this,
          [this, azure_project_style](bool dark) {
            Noggit::Ui::setAzureColorMode(!dark);
            _ui->centralwidget->setStyleSheet(Noggit::Ui::projectColorStyle(azure_project_style));
          });
  connect(_settings, &Noggit::Ui::settings::saved, this,
          [this, azure_project_style]() {
            _ui->centralwidget->setStyleSheet(Noggit::Ui::projectColorStyle(azure_project_style));
          });
  _ui->changelog_button->hide();
  Component::RecentProjectsComponent::buildRecentProjectsList(this);

  auto update_selected_project = [this, project_name, project_meta, launch_button]()
  {
    QListWidgetItem* const item = _ui->listView->currentItem();
    if (!item)
    {
      project_name->setText(QStringLiteral("Select a project"));
      project_meta->setText(QStringLiteral("Choose a project from Recent Projects"));
      launch_button->setText(QStringLiteral("Enter Selected Project"));
      launch_button->setEnabled(false);
      return;
    }

    QString const path = item->data(Qt::UserRole).toString();
    auto const project = Noggit::Project::ApplicationProjectReader().readProject(
        std::filesystem::path(path.toStdString()));
    if (!project)
    {
      launch_button->setEnabled(false);
      return;
    }

    QString const name = QString::fromStdString(project->ProjectName);
    QString const version = project->projectVersion == Noggit::Project::ProjectVersion::SL
        ? QStringLiteral("Shadowlands") : QStringLiteral("Wrath of the Lich King");
    project_name->setText(name);
    project_meta->setText(version + QStringLiteral("  •  ") + QDir::toNativeSeparators(path));
    project_meta->setToolTip(path);
    launch_button->setText(launch_button->fontMetrics().elidedText(
        QStringLiteral("Enter ") + name, Qt::ElideRight, 220));
    launch_button->setEnabled(true);
  };
  QObject::connect(_ui->listView, &QListWidget::currentItemChanged, this,
                   [update_selected_project]() { update_selected_project(); });
  if (_ui->listView->count() > 0)
    _ui->listView->setCurrentRow(0);
  else
    update_selected_project();
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
                       if (_ui->listView->count() > 0)
                         _ui->listView->setCurrentRow(0);
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

  auto launch_selected_project = [this]()
  {
    if (!_ui->listView->currentItem())
      return;

    auto selected_project = _load_project_component->loadProject(this);
    if (!selected_project)
    {
      LogError << "Selected Project is null, loading failed." << std::endl;
      return;
    }

    Noggit::Project::CurrentProject::initialize(selected_project.get());
    _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
        _noggit_application->getConfiguration(), selected_project);
    _project_selection_page->showMaximized();
    close();
  };
  QObject::connect(launch_button, &QPushButton::clicked, this, launch_selected_project);
  QObject::connect(_ui->listView, &QListView::doubleClicked, this,
                   [launch_selected_project]() { launch_selected_project(); });

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
  if (_ui->listView->count() > 0)
    _ui->listView->setCurrentRow(0);
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
  if (_ui->listView->count() > 0)
    _ui->listView->setCurrentRow(0);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::handleContextMenuProjectListItemFavorite(int index)
{
  QSettings settings;
  settings.sync();
  settings.setValue("favorite_project", index);
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
  if (_ui->listView->count() > 0)
    _ui->listView->setCurrentRow(0);
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

