#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/DBC.h>
#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/MapView.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/FramelessWindow.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/tools/MapCreationWizard/Ui/MapCreationWizard.hpp>
#include <noggit/ui/tools/UiCommon/StackedWidget.hpp>
#include <noggit/ui/UidFixWindow.hpp>
#include <noggit/ui/windows/about/About.h>
#include <noggit/ui/windows/adtPorter/AdtPorterDialog.hpp>
#include <noggit/ui/windows/noggitWindow/components/BuildMapListComponent.hpp>
#include <noggit/ui/windows/noggitWindow/MapSelectionStyle.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapBookmarkListItem.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/uid_storage.hpp>
#include <noggit/World.h>
#include <noggit/database/SqlUIDStorage.h>
#include <noggit/database/ClientDatabase.h>

#include <string>
#include <blizzard-archive-library/include/Exception.hpp>

#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProcess>
#include <QPixmap>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtCore/QSettings>

#include <chrono>
#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <vector>

#include "revision.h"

#include "ui_TitleBar.h"
#include <noggit/ui/tools/ViewportManager/ViewportManager.hpp>

namespace Noggit::Ui::Windows
{
  class MapEmptyArtworkFrame final : public QFrame
  {
  public:
    using QFrame::QFrame;

  protected:
    void paintEvent(QPaintEvent* event) override
    {
      QFrame::paintEvent(event);

      static const QPixmap artwork(":/map-selection-coast");
      if (artwork.isNull())
        return;

      QRectF target(rect());
      target.adjust(1.0, 1.0, -1.0, -1.0);
      if (target.isEmpty())
        return;

      // The 1600x1200 source has black bars and branding outside this clean landscape band.
      QRectF const clean_band(0.0, 246.0, 1600.0, 667.0);
      qreal const scale = std::max(target.width() / clean_band.width(),
                                   target.height() / clean_band.height());
      qreal const visible_width = target.width() / scale;
      qreal const visible_height = target.height() / scale;
      QRectF const source(clean_band.left() + (clean_band.width() - visible_width) / 2.0,
                          clean_band.top() + (clean_band.height() - visible_height) * 0.46,
                          visible_width, visible_height);

      QPainter painter(this);
      painter.setRenderHint(QPainter::SmoothPixmapTransform);
      painter.drawPixmap(target, artwork, source);

      QLinearGradient left_shade(target.topLeft(),
                                  QPointF(target.left() + target.width() * 0.65, target.top()));
      left_shade.setColorAt(0.0, QColor(5, 12, 25, 130));
      left_shade.setColorAt(1.0, QColor(5, 12, 25, 0));
      painter.fillRect(target, left_shade);

      QLinearGradient bottom_shade(target.bottomLeft(),
                                    QPointF(target.left(), target.top() + target.height() * 0.56));
      bottom_shade.setColorAt(0.0, QColor(5, 12, 25, 225));
      bottom_shade.setColorAt(1.0, QColor(5, 12, 25, 0));
      painter.fillRect(target, bottom_shade);
    }
  };

  NoggitWindow::NoggitWindow(std::shared_ptr<Noggit::Application::NoggitApplicationConfiguration> application,
                             std::shared_ptr<Noggit::Project::NoggitProject> project)
      : QMainWindow(nullptr)
      , _null_widget(new QWidget(this))
      , _applicationConfiguration(application)
      , _project(project)
  {

    std::stringstream title;
    title << "Noggit Azure - " << STRPRODUCTVER;
    setWindowTitle(QString::fromStdString(title.str()));
    setWindowIcon(QIcon(":/icon"));

    Log << "Project version : " << Noggit::Project::ClientVersionFactory::MapToStringVersion(project->projectVersion).c_str() << std::endl;

    if (project->projectVersion == Project::ProjectVersion::WOTLK)
    {
      OpenDBs(project->ClientData);
    }
    else
    {
      assert(false); // TODO
      LogError << "NoggitWindow() : Unsupported project version, skipping loading DBCs." << std::endl;
    }

    _settings = new settings(this);

    QSettings settings;
    // connect to databases
    bool use_mysql_uid = settings.value("project/mysql/enabled", false).toBool();
    bool use_sql_client_database = settings.value("project/mysql/client_db_enabled", false).toBool();

    ClientDatabase::setDatabaseMode(use_sql_client_database ? DatabaseMode::Sql : DatabaseMode::ClientStorage);

    if (use_mysql_uid || use_sql_client_database)
    {
      auto host = settings.value("project/mysql/server").toString();
      auto port = settings.value("project/mysql/port", "3306").toInt();
      auto db_name = settings.value("project/mysql/db").toString();  // schema name
      auto user = settings.value("project/mysql/user").toString();
      auto pass = settings.value("project/mysql/pwd").toString();

      auto& db_manager = Sql::SqlDatabaseManager::instance();
      db_manager.initializeDbConnection(Sql::SQLDbType::Noggit, host, port, db_name, user, pass);
    }

    setCentralWidget(_null_widget);

    // The default value is AnimatedDocks | AllowTabbedDocks.
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);

    _about = new about(this);

    _menuBar = menuBar();

    if (!settings.value("systemWindowFrame", true).toBool())
    {
      QWidget* widget = new QWidget(this);
      ::Ui::TitleBar* titleBarWidget = setupFramelessWindow(widget, this, minimumSize(), maximumSize(), true);
      titleBarWidget->horizontalLayout->insertWidget(2, _menuBar);
      setMenuWidget(widget);
    }

    _menuBar->setNativeMenuBar(settings.value("nativeMenubar", true).toBool());

    auto file_menu(_menuBar->addMenu("&Noggit"));

    auto settings_action(file_menu->addAction("Settings"));
    QObject::connect(settings_action, &QAction::triggered, [&]
                     {
                       _settings->show();
                     }
    );

    auto adt_porter_action(file_menu->addAction("ADT Porting Tool..."));
    QObject::connect(adt_porter_action, &QAction::triggered, this, [this]
    {
      if (map_loaded)
      {
        QMessageBox::information(this, "ADT Porting Tool",
          "Return to the map-selection screen before porting an ADT. This prevents the tool from "
          "changing files that are currently open in the world editor.");
        return;
      }
      auto* dialog = new AdtPorterDialog(_project, this);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->show();
      dialog->raise();
      dialog->activateWindow();
    });

    auto about_action(file_menu->addAction("About"));
    QObject::connect(about_action, &QAction::triggered, [&]
                     {
                       _about->show();
                     }
    );

    auto proj_selec_action(file_menu->addAction("Exit to Project Selection"));
    QObject::connect(proj_selec_action, &QAction::triggered, [this]
        {
            // auto noggit = Noggit::Application::NoggitApplication::instance();
            // auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
            // project_selection->show();

            exit_to_project_selection = true;
            close();
        }
    );

    auto mapmenu_action(file_menu->addAction("Exit"));
    QObject::connect(mapmenu_action, &QAction::triggered, [this]
                     {
                       close();
                     }
    );

    _menuBar->adjustSize();

    _buildMapListComponent = std::make_unique<Component::BuildMapListComponent>();

    _map_creation_wizard = new Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard(_project, this);

    buildMenu();
  }

  void NoggitWindow::check_uid_then_enter_map
      (glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, bool from_bookmark
      )
  {
    QSettings settings;
    assert(getWorld());

    unsigned int world_map_id = getWorld()->getMapID();

    bool use_mysql = settings.value("project/mysql/enabled", false).toBool();

    if ((Noggit::Sql::SqlUIDStorage::hasMaxUIDStoredDB(world_map_id))
      || uid_storage::hasMaxUIDStored(world_map_id)
       )
    {

      getWorld()->mapIndex.loadMaxUID();
      enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::none, from_bookmark);
    }
    // old if no mysql block
    /*
    if (uid_storage::hasMaxUIDStored(world_map_id))
    {
      if (settings.value("uid_startup_check", true).toBool())
      {
        enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::max_uid, from_bookmark);
      } else
      {
        getWorld()->mapIndex.loadMaxUID();
        enterMapAt(pos, camera_pitch, camera_yaw, uid_fix_mode::none, from_bookmark);
      }
    }*/
    else
    {
      auto uid_fix_window(new UidFixWindow(pos, camera_pitch, camera_yaw));
      uid_fix_window->show();

      connect(uid_fix_window, &Noggit::Ui::UidFixWindow::fix_uid, [this, from_bookmark]
                  (glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, uid_fix_mode uid_fix
                  )
              {
                enterMapAt(pos, camera_pitch, camera_yaw, uid_fix, from_bookmark);
              }
      );
    }
  }

  void
  NoggitWindow::enterMapAt(glm::vec3 pos, math::degrees camera_pitch, math::degrees camera_yaw, uid_fix_mode uid_fix,
                           bool from_bookmark
  )
  {
    World* world = getWorld();

    if (world->mapIndex.hasAGlobalWMO())
    {
        // enter at mdoel's position
        // pos = glm::vec3(_world->mWmoEntry[0], _world->mWmoEntry.pos[1], _world->mWmoEntry.pos[2]);

        // better, enter at model's max extent, facing toward min extent
        auto min_extent = glm::vec3(world->mWmoEntry.extents[0][0], world->mWmoEntry.extents[0][1], world->mWmoEntry.extents[0][2]);
        auto max_extent = glm::vec3(world->mWmoEntry.extents[1][0], world->mWmoEntry.extents[1][1] * 2, world->mWmoEntry.extents[1][2]);
        float dx = min_extent.x - max_extent.x;
        float dy = min_extent.z - max_extent.z; // flipping z and y works better for some reason
        float dz = min_extent.y - max_extent.y;

        pos = { world->mWmoEntry.pos[0], world->mWmoEntry.pos[1], world->mWmoEntry.pos[2] };

        camera_yaw = math::degrees(math::radians(std::atan2(dx, dy)));

        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        camera_pitch = -math::degrees(math::radians(std::asin(dz / distance)));

    }


    // _map_creation_wizard->destroyFakeWorld();
    _map_view = (new MapView(camera_yaw, camera_pitch, pos, this, _project, std::move(_map_creation_wizard->_world), uid_fix, from_bookmark));
    connect(_map_view, &MapView::uid_fix_failed, [this]()
    { promptUidFixFailure(); });
    connect(_settings, &settings::saved, [this]()
    { if (_map_view) _map_view->onSettingsSave(); });

    // _app_toolbar = new QToolBar("Application", this);
    // _app_toolbar->setOrientation(Qt::Horizontal);
    // addToolBar(_app_toolbar);

    _stack_widget->addWidget(_map_view);
    _stack_widget->setCurrentIndex(1);

    map_loaded = true;

  }

  void NoggitWindow::applyFilterSearch(const QString &name, int type, int expansion, bool wmo_maps)
  {
      int visible_count = 0;
      for (int i = 0; i < _continents_table->count(); ++i)
      {
          auto item_widget = _continents_table->item(i);
          auto widget = qobject_cast<Noggit::Ui::Widget::MapListItem*>(_continents_table->itemWidget(item_widget));

          if (!widget)
              continue;

          item_widget->setHidden(false);

          if (!widget->name().contains(name, Qt::CaseInsensitive)
              && !QString::number(widget->id()).contains(name))
          {
              item_widget->setHidden(true);
              continue;
          }

          if (!(widget->type() == (type - 2)) && type != 0)
          {
              item_widget->setHidden(true);
              continue;
          }

          if (!(widget->expansion() == (expansion - 1)) && expansion != 0)
          {
              item_widget->setHidden(true);
          }

          if (!(widget->wmo_map() == wmo_maps))
          {
              item_widget->setHidden(true);
          }
          if (!item_widget->isHidden())
            ++visible_count;
      }
      if (auto count_label = findChild<QLabel*>("mapListCount"))
        count_label->setText(tr("%1 shown").arg(visible_count));
  }

  void NoggitWindow::loadMap(int map_id)
  {
    // _minimap->world(nullptr);

    // World is now created only here in
    // void MapCreationWizard::selectMap(int map_id)
    emit mapSelected(map_id);

    /*
    _world.reset();

    auto table = _project->ClientDatabase->LoadTable("Map", readFileAsIMemStream);
    auto record = table.Record(map_id);

    _world = std::make_unique<World>(record.Columns["Directory"].Value, map_id, Noggit::NoggitRenderContext::MAP_VIEW);
    */

    _minimap->world(getWorld());
    if (_map_preview_stack && getWorld())
      _map_preview_stack->setCurrentIndex(1);

    //_project->ClientDatabase->UnloadTable("Map");
  }

  void NoggitWindow::buildMenu()
  {
    _stack_widget = new StackedWidget(this);
    _stack_widget->setAutoResize(true);

    setCentralWidget(_stack_widget);

    auto widget(new QWidget(_stack_widget));
    widget->setObjectName("mapSelectionPage");
    _stack_widget->addWidget(widget);

    auto root_layout = new QVBoxLayout(widget);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto header = new QFrame(widget);
    header->setObjectName("mapSelectionHeader");
    header->setFixedHeight(64);
    auto header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(22, 0, 22, 0);
    header_layout->setSpacing(11);
    auto brand_mark = new QLabel(QStringLiteral("N"), header);
    brand_mark->setObjectName("mapBrandMark");
    brand_mark->setAlignment(Qt::AlignCenter);
    brand_mark->setFixedSize(34, 34);
    auto brand_name = new QLabel(QStringLiteral("Noggit Azure"), header);
    brand_name->setObjectName("mapBrandName");
    auto brand_section = new QLabel(QStringLiteral("MAP SELECTION"), header);
    brand_section->setObjectName("mapBrandSection");
    auto brand_layout = new QVBoxLayout();
    brand_layout->setContentsMargins(0, 0, 0, 0);
    brand_layout->setSpacing(1);
    brand_layout->addWidget(brand_name);
    brand_layout->addWidget(brand_section);
    header_layout->addWidget(brand_mark);
    header_layout->addLayout(brand_layout);
    header_layout->addStretch();
    auto project_label = new QLabel(QString::fromStdString(_project->ProjectName), header);
    project_label->setObjectName("mapProjectName");
    header_layout->addWidget(project_label);
    auto color_mode_toggle = new QCheckBox(tr("Dark mode"), header);
    color_mode_toggle->setObjectName("mapColorModeToggle");
    color_mode_toggle->setChecked(!Noggit::Ui::azureColorMode());
    color_mode_toggle->setToolTip(tr("Checked: Dark colors. Unchecked: Azure colors."));
    header_layout->addWidget(color_mode_toggle);
    connect(color_mode_toggle, &QCheckBox::toggled, widget,
            [widget](bool dark) {
              Noggit::Ui::setAzureColorMode(!dark);
              widget->setStyleSheet(mapSelectionStyle());
            });
    auto header_settings = new QPushButton(QStringLiteral("Settings"), header);
    header_settings->setObjectName("mapHeaderSettings");
    header_layout->addWidget(header_settings);
    QObject::connect(header_settings, &QPushButton::clicked, this,
                     [this]() { _settings->show(); });
    root_layout->addWidget(header);

    auto content = new QWidget(widget);
    content->setObjectName("mapSelectionContent");
    auto layout = new QHBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    root_layout->addWidget(content, 1);
    QListWidget* bookmarks_table(new QListWidget(widget));
    bookmarks_table->setObjectName("bookmarksList");
    _continents_table = new QListWidget(widget);
    _continents_table->setObjectName("mapList");
    _continents_table->setSpacing(3);
    _continents_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _continents_table->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
    _continents_table->setSelectionBehavior(QAbstractItemView::SelectItems);

    // in some situations like when returning to menu an item is selected and itemSelectionChanged won't fire when reclicking it
    // so might need to also connect itemClicked but then they both trigger at the same time.
    QObject::connect(_continents_table, &QListWidget::itemSelectionChanged, [this]()
      {
        QSignalBlocker const blocker(_continents_table);
        QListWidgetItem* const item = _continents_table->currentItem();
        if (item)
        {
          loadMap(item->data(Qt::UserRole).toInt());
        }
      }
    );


    QTabWidget* entry_points_tabs(new QTabWidget(widget));
    entry_points_tabs->setObjectName("mapBrowserTabs");
    //entry_points_tabs->addTab(_continents_table, "Maps");

     auto add_btn = new QPushButton("Add New Map", this);
     add_btn->setObjectName("addMapButton");
     add_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::plus));
     add_btn->setAccessibleName("map_wizard_add_button");

    /* set-up widget for seaching etc... through _continents_table */
    {
        QWidget* _first_tab = new QWidget(entry_points_tabs);
        _first_tab->setObjectName("mapsBrowserPage");
        QVBoxLayout* _first_tab_layout = new QVBoxLayout(_first_tab);
        _first_tab_layout->setContentsMargins(14, 14, 14, 14);
        _first_tab_layout->setSpacing(8);

        auto search_panel = new QWidget(_first_tab);
        search_panel->setObjectName("mapSearchPanel");
        auto search_layout = new QVBoxLayout(search_panel);
        search_layout->setContentsMargins(0, 0, 0, 0);
        search_layout->setSpacing(9);

        QLineEdit* _line_edit_search = new QLineEdit(search_panel);
        _line_edit_search->setObjectName("mapSearchInput");
        _line_edit_search->setPlaceholderText(tr("Search maps or map ID"));
        _line_edit_search->setClearButtonEnabled(true);
        QComboBox* _combo_search = new QComboBox(search_panel);
        _combo_search->setObjectName("mapTypeFilter");
        _combo_search->addItems(QStringList() <<
                                tr("All") <<
                                tr("Unknown") <<
                                tr("Continent") <<
                                tr("Dungeon") <<
                                tr("Raid") <<
                                tr("Battleground") <<
                                tr("Arena") <<
                                tr("Scenario"));
        QSettings settings;
        _combo_search->setCurrentIndex(settings.value("mapComboSearch", 2).toInt());

        QComboBox* _combo_exp_search = new QComboBox(search_panel);
        _combo_exp_search->setObjectName("mapExpansionFilter");
        _combo_exp_search->addItem(tr("All"));
        _combo_exp_search->addItem(QIcon(":/icon-classic"), tr("Classic"));
        _combo_exp_search->addItem(QIcon(":/icon-burning"), tr("Burning Cursade"));
        _combo_exp_search->addItem(QIcon(":/icon-wrath"), tr("Wrath of the Lich King"));
        _combo_exp_search->addItem(QIcon(":/icon-cata"), tr("Cataclism"));
        _combo_exp_search->addItem(QIcon(":/icon-panda"), tr("Mist of Pandaria"));
        _combo_exp_search->addItem(QIcon(":/icon-warlords"), tr("Warlords of Draenor"));
        _combo_exp_search->addItem(QIcon(":/icon-legion"), tr("Legion"));
        _combo_exp_search->addItem(QIcon(":/icon-battle"), tr("Battle for Azeroth"));
        _combo_exp_search->addItem(QIcon(":/icon-shadow"), tr("Shadowlands"));
        _combo_exp_search->setCurrentIndex(0);

        QCheckBox* _wmo_maps_search = new QCheckBox(tr("WMO maps only (no terrain)"), search_panel);
        _wmo_maps_search->setObjectName("mapWmoFilter");

        QObject::connect(_line_edit_search, QOverload<const QString&>::of(&QLineEdit::textChanged), [this, _combo_search, _combo_exp_search, _wmo_maps_search](const QString &name)
                         {
                             applyFilterSearch(name, _combo_search->currentIndex(), _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
                         });

        QObject::connect(_combo_search, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, _line_edit_search, _combo_exp_search, _wmo_maps_search](int index)
                         {
                             applyFilterSearch(_line_edit_search->text(), index, _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
                             QSettings settings;
                             settings.setValue("mapComboSearch", index);
                             settings.sync();
                         });

        QObject::connect(_combo_exp_search, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, _line_edit_search, _combo_search, _wmo_maps_search](int index)
                         {
                             applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), index, _wmo_maps_search->isChecked());
                         });

        QObject::connect(_wmo_maps_search, &QCheckBox::stateChanged, [this, _line_edit_search, _combo_search, _combo_exp_search](bool b)
                         {
                             applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), _combo_exp_search->currentIndex(), b);
                         });

        auto filter_row = new QHBoxLayout();
        filter_row->setContentsMargins(0, 0, 0, 0);
        filter_row->setSpacing(8);
        auto type_column = new QVBoxLayout();
        type_column->setSpacing(4);
        auto type_label = new QLabel(tr("TYPE"), search_panel);
        type_label->setObjectName("mapFilterLabel");
        type_column->addWidget(type_label);
        type_column->addWidget(_combo_search);
        auto expansion_column = new QVBoxLayout();
        expansion_column->setSpacing(4);
        auto expansion_label = new QLabel(tr("EXPANSION"), search_panel);
        expansion_label->setObjectName("mapFilterLabel");
        expansion_column->addWidget(expansion_label);
        expansion_column->addWidget(_combo_exp_search);
        filter_row->addLayout(type_column, 1);
        filter_row->addLayout(expansion_column, 1);
        search_layout->addWidget(_line_edit_search);
        search_layout->addLayout(filter_row);
        search_layout->addWidget(_wmo_maps_search);

        auto list_heading = new QLabel(tr("MAPS"), _first_tab);
        list_heading->setObjectName("mapListHeading");
        auto list_count = new QLabel(_first_tab);
        list_count->setObjectName("mapListCount");
        auto list_heading_row = new QHBoxLayout();
        list_heading_row->setContentsMargins(0, 0, 0, 0);
        list_heading_row->addWidget(list_heading);
        list_heading_row->addStretch();
        list_heading_row->addWidget(list_count);
        _first_tab_layout->addWidget(search_panel);
        _first_tab_layout->addLayout(list_heading_row);
        _first_tab_layout->addWidget(_continents_table, 1);
        _first_tab_layout->addWidget(add_btn);

        entry_points_tabs->addTab(_first_tab, tr("Maps"));

        entry_points_tabs->addTab(bookmarks_table, "Bookmarks");
        entry_points_tabs->setFixedWidth(330);
        layout->addWidget(entry_points_tabs);

        _buildMapListComponent->buildMapList(this);
        applyFilterSearch(_line_edit_search->text(), _combo_search->currentIndex(), _combo_exp_search->currentIndex(), _wmo_maps_search->isChecked());
    }

    qulonglong bookmark_index(0);
    for (auto entry: _project->Bookmarks)
    {
      auto item = new QListWidgetItem(bookmarks_table);

      auto bookmark_data = Widget::MapListBookmarkData();
      bookmark_data.MapName = QString::fromStdString(entry.name);
      bookmark_data.Position = entry.position;

      auto map_bookmark_item = new Widget::MapListBookmarkItem(bookmark_data, bookmarks_table);

      item->setData(Qt::UserRole, QVariant(bookmark_index++));
      item->setSizeHint(map_bookmark_item->minimumSizeHint());
      bookmarks_table->setItemWidget(item, map_bookmark_item);
    }

    QObject::connect(bookmarks_table, &QListWidget::itemDoubleClicked, [this](QListWidgetItem* item)
                     {

                       auto& entry(_project->Bookmarks.at(item->data(Qt::UserRole).toInt()));

                       _map_creation_wizard->_world.reset();

                       for (DBCFile::Iterator it = gMapDB.begin(); it != gMapDB.end(); ++it)
                       {
                         if (it->getInt(MapDB::MapID) == entry.map_id)
                         {
                           //     emit mapSelected(map_id); to update UI
                           _map_creation_wizard->_world = std::make_unique<World>(it->getString(MapDB::InternalName),
                                                            entry.map_id, Noggit::NoggitRenderContext::MAP_VIEW);
                           check_uid_then_enter_map(entry.position, math::degrees(entry.camera_pitch), math::degrees(entry.camera_yaw),
                                                    true
                           );
                           return;
                         }
                       }
                     }
    );

    _minimap = new minimap_widget(this);
    QSettings minimap_settings;
    bool const show_adt_grid = minimap_settings.value("mainMenu/showAdtGrid", true).toBool();
    _minimap->draw_boundaries(show_adt_grid);
    //_minimap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    QObject::connect(_minimap, &minimap_widget::map_clicked, [this](::glm::vec3 const& pos)
                     {
                        if (getWorld()->mapIndex.hasAGlobalWMO()) // skip uid check
                            enterMapAt(pos, math::degrees(30.f), math::degrees(90.f), uid_fix_mode::none, false);
                        else
                            check_uid_then_enter_map(pos, math::degrees(30.f), math::degrees(90.f));
                     }
    );

    _right_side = new QTabWidget(this);
    _right_side->setObjectName("mapModeTabs");

    auto enter_map_tab = new QWidget(_right_side);
    enter_map_tab->setObjectName("enterMapPage");
    auto enter_map_layout = new QVBoxLayout(enter_map_tab);
    enter_map_layout->setContentsMargins(18, 8, 18, 18);
    enter_map_layout->setSpacing(9);

    auto show_grid_toggle = new QCheckBox(tr("Show ADT grid"), enter_map_tab);
    show_grid_toggle->setObjectName("mapGridToggle");
    show_grid_toggle->setChecked(show_adt_grid);
    show_grid_toggle->setAccessibleName("main_menu_show_adt_grid");
    QObject::connect(show_grid_toggle, &QCheckBox::toggled, [this](bool checked)
      {
        _minimap->draw_boundaries(checked);
        QSettings settings;
        settings.setValue("mainMenu/showAdtGrid", checked);
      }
    );
    auto preview_controls = new QHBoxLayout();
    preview_controls->addWidget(show_grid_toggle);
    preview_controls->addStretch(1);

    auto appearance_button = new QPushButton(tr("Heightmap appearance..."), enter_map_tab);
    appearance_button->setObjectName("mapAppearanceButton");
    appearance_button->setAccessibleName("main_menu_heightmap_appearance");
    preview_controls->addWidget(appearance_button);
    enter_map_layout->addLayout(preview_controls);

    auto appearance_dialog = new QDialog(this);
    appearance_dialog->setWindowTitle(tr("Heightmap Appearance"));
    appearance_dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    appearance_dialog->resize(760, 580);
    auto appearance_layout = new QVBoxLayout(appearance_dialog);
    auto appearance_help = new QLabel(
      tr("Adjust each elevation stop and its color. Changes update the selected map preview live."),
      appearance_dialog);
    appearance_help->setWordWrap(true);
    appearance_layout->addWidget(appearance_help);

    auto palette_scroll = new QScrollArea(appearance_dialog);
    palette_scroll->setWidgetResizable(true);
    auto palette_widget = new QWidget(palette_scroll);
    auto palette_grid = new QGridLayout(palette_widget);
    palette_grid->addWidget(new QLabel(tr("Terrain band"), palette_widget), 0, 0);
    palette_grid->addWidget(new QLabel(tr("Elevation"), palette_widget), 0, 1);
    palette_grid->addWidget(new QLabel(tr("Value"), palette_widget), 0, 2);
    palette_grid->addWidget(new QLabel(tr("Color"), palette_widget), 0, 3);

    QStringList const palette_names = {
      tr("Deep water"), tr("Ocean"), tr("Shallow water"), tr("Shoreline"),
      tr("Lowest terrain"), tr("Lowland"), tr("Grassland"), tr("Upper grassland"),
      tr("Highland"), tr("Foothills"), tr("Mountain"), tr("High mountain"),
      tr("Snow transition"), tr("Snow"), tr("Peak snow")
    };
    auto editor_palette = std::make_shared<map_horizon_minimap_palette>(load_map_horizon_minimap_palette());
    auto palette_sliders = std::make_shared<std::vector<QSlider*>>();
    auto palette_values = std::make_shared<std::vector<QLabel*>>();
    auto palette_buttons = std::make_shared<std::vector<QPushButton*>>();
    palette_sliders->reserve(map_horizon_minimap_palette::stop_count);
    palette_values->reserve(map_horizon_minimap_palette::stop_count);
    palette_buttons->reserve(map_horizon_minimap_palette::stop_count);

    auto preview_timer = new QTimer(appearance_dialog);
    preview_timer->setSingleShot(true);
    preview_timer->setInterval(60);
    QObject::connect(preview_timer, &QTimer::timeout, [this, editor_palette]()
      {
        World* world = getWorld();
        if (!world)
          return;
        world->horizon.minimap_palette(*editor_palette);
        world->horizon.set_minimap(&world->mapIndex);
        _minimap->update();
      }
    );
    auto schedule_preview = [preview_timer, editor_palette]()
      {
        save_map_horizon_minimap_palette(*editor_palette);
        preview_timer->start();
      };

    auto refresh_slider_ranges = std::make_shared<std::function<void()>>();
    for (std::size_t i = 0; i < map_horizon_minimap_palette::stop_count; ++i)
    {
      auto name = new QLabel(palette_names.at(static_cast<int>(i)), palette_widget);
      auto slider = new QSlider(Qt::Horizontal, palette_widget);
      auto value = new QLabel(QString::number(editor_palette->stops[i].height), palette_widget);
      value->setMinimumWidth(48);
      auto color_button = new QPushButton(palette_widget);
      color_button->setMinimumWidth(90);
      auto update_swatch = [color_button](QColor const& color)
        {
          color_button->setText(color.name(QColor::HexRgb).toUpper());
          color_button->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; }")
            .arg(color.name(QColor::HexRgb), color.lightness() < 128 ? QStringLiteral("white")
                                                                    : QStringLiteral("black")));
        };
      update_swatch(editor_palette->stops[i].color);

      palette_sliders->push_back(slider);
      palette_values->push_back(value);
      palette_buttons->push_back(color_button);
      palette_grid->addWidget(name, static_cast<int>(i) + 1, 0);
      palette_grid->addWidget(slider, static_cast<int>(i) + 1, 1);
      palette_grid->addWidget(value, static_cast<int>(i) + 1, 2);
      palette_grid->addWidget(color_button, static_cast<int>(i) + 1, 3);

      QObject::connect(slider, &QSlider::valueChanged,
        [i, editor_palette, value, refresh_slider_ranges, schedule_preview](int height)
        {
          editor_palette->stops[i].height = height;
          value->setText(QString::number(height));
          if (*refresh_slider_ranges)
            (*refresh_slider_ranges)();
          schedule_preview();
        });
      QObject::connect(color_button, &QPushButton::clicked,
        [i, appearance_dialog, editor_palette, update_swatch, schedule_preview]()
        {
          QColor const selected = QColorDialog::getColor(editor_palette->stops[i].color,
                                                          appearance_dialog,
                                                          QObject::tr("Select terrain color"));
          if (!selected.isValid())
            return;
          editor_palette->stops[i].color = selected;
          update_swatch(selected);
          schedule_preview();
        });
    }

    *refresh_slider_ranges = [editor_palette, palette_sliders]()
      {
        for (std::size_t i = 0; i < palette_sliders->size(); ++i)
        {
          int const minimum = i == 0 ? -1000 : editor_palette->stops[i - 1].height + 1;
          int const maximum = i + 1 == palette_sliders->size()
                            ? 2500 : editor_palette->stops[i + 1].height - 1;
          QSignalBlocker blocker(palette_sliders->at(i));
          palette_sliders->at(i)->setRange(minimum, maximum);
          palette_sliders->at(i)->setValue(editor_palette->stops[i].height);
        }
      };
    (*refresh_slider_ranges)();

    palette_scroll->setWidget(palette_widget);
    appearance_layout->addWidget(palette_scroll, 1);
    auto reset_palette = new QPushButton(tr("Reset to reference defaults"), appearance_dialog);
    QObject::connect(reset_palette, &QPushButton::clicked,
      [editor_palette, palette_sliders, palette_values, palette_buttons,
       refresh_slider_ranges, schedule_preview]()
      {
        *editor_palette = default_map_horizon_minimap_palette();
        for (std::size_t i = 0; i < palette_sliders->size(); ++i)
        {
          palette_values->at(i)->setText(QString::number(editor_palette->stops[i].height));
          QColor const color = editor_palette->stops[i].color;
          palette_buttons->at(i)->setText(color.name(QColor::HexRgb).toUpper());
          palette_buttons->at(i)->setStyleSheet(
            QStringLiteral("QPushButton { background-color: %1; color: %2; }")
              .arg(color.name(QColor::HexRgb), color.lightness() < 128 ? QStringLiteral("white")
                                                                      : QStringLiteral("black")));
        }
        (*refresh_slider_ranges)();
        schedule_preview();
      });
    appearance_layout->addWidget(reset_palette, 0, Qt::AlignRight);
    QObject::connect(appearance_button, &QPushButton::clicked, [appearance_dialog]()
      {
        appearance_dialog->show();
        appearance_dialog->raise();
        appearance_dialog->activateWindow();
      });

    _map_preview_stack = new QStackedWidget(enter_map_tab);
    _map_preview_stack->setObjectName("mapPreviewStack");

    auto empty_preview = new MapEmptyArtworkFrame(_map_preview_stack);
    empty_preview->setObjectName("mapEmptyPreview");
    auto empty_layout = new QVBoxLayout(empty_preview);
    empty_layout->setContentsMargins(38, 22, 38, 42);
    empty_layout->addStretch(1);
    auto empty_kicker = new QLabel(tr("YOUR WORLDS"), empty_preview);
    empty_kicker->setObjectName("mapEmptyKicker");
    empty_layout->addWidget(empty_kicker);
    auto empty_title = new QLabel(tr("Choose a map"), empty_preview);
    empty_title->setObjectName("mapEmptyTitle");
    empty_layout->addWidget(empty_title);
    auto empty_hint = new QLabel(tr("Select a map on the left to see its terrain and choose where to enter."),
                                 empty_preview);
    empty_hint->setObjectName("mapEmptyHint");
    empty_hint->setWordWrap(true);
    empty_layout->addWidget(empty_hint);
    _map_preview_stack->addWidget(empty_preview);

    auto selected_preview = new QFrame(_map_preview_stack);
    selected_preview->setObjectName("mapSelectedPreview");
    auto selected_layout = new QVBoxLayout(selected_preview);
    selected_layout->setContentsMargins(18, 18, 18, 12);
    selected_layout->setSpacing(10);
    auto selected_header = new QHBoxLayout();
    auto selected_heading = new QVBoxLayout();
    selected_heading->setSpacing(3);
    auto selected_kicker = new QLabel(tr("MAP PREVIEW"), selected_preview);
    selected_kicker->setObjectName("mapPreviewKicker");
    auto selected_name = new QLabel(tr("Map preview"), selected_preview);
    selected_name->setObjectName("mapPreviewName");
    selected_heading->addWidget(selected_kicker);
    selected_heading->addWidget(selected_name);
    selected_header->addLayout(selected_heading);
    selected_header->addStretch(1);
    auto selected_id = new QLabel(selected_preview);
    selected_id->setObjectName("mapPreviewId");
    selected_header->addWidget(selected_id, 0, Qt::AlignBottom);
    selected_layout->addLayout(selected_header);

    auto minimap_holder = new QScrollArea(selected_preview);
    minimap_holder->setObjectName("mapMinimapHolder");
    minimap_holder->setWidgetResizable(true);
    minimap_holder->setAlignment(Qt::AlignCenter);
    minimap_holder->setWidget(_minimap);
    selected_layout->addWidget(minimap_holder, 1);
    auto selected_hint = new QLabel(tr("Click a terrain tile to enter the world."), selected_preview);
    selected_hint->setObjectName("mapPreviewHint");
    selected_layout->addWidget(selected_hint);
    _map_preview_stack->addWidget(selected_preview);
    _map_preview_stack->setCurrentIndex(0);
    enter_map_layout->addWidget(_map_preview_stack, 1);
    QObject::connect(this, &NoggitWindow::mapSelected, selected_preview,
                     [this, selected_name, selected_id](int map_id)
                     {
                       QString map_name = tr("Map %1").arg(map_id);
                       QListWidgetItem* item = _continents_table->currentItem();
                       if (item && item->data(Qt::UserRole).toInt() == map_id)
                       {
                         auto map_item = qobject_cast<Widget::MapListItem*>(_continents_table->itemWidget(item));
                         if (map_item)
                           map_name = map_item->name();
                       }
                       selected_name->setText(map_name);
                       selected_id->setText(tr("Map %1").arg(map_id));
                     });

    _right_side->addTab(enter_map_tab, "Enter map");
    minimap_holder->setAccessibleName("main_menu_minimap_holder");

    _map_wizard_connection = connect(_map_creation_wizard,
                                     &Noggit::Ui::Tools::MapCreationWizard::Ui::MapCreationWizard::map_dbc_updated, 
                                [this](int map_id = -1)
                                     {
                                       _buildMapListComponent->buildMapList(this);

                                       // if a new map was added select it
                                       if (map_id >= 0)
                                       {
                                           for (int i = 0; i < _continents_table->count(); ++i)
                                           {
                                               QListWidgetItem* item = _continents_table->item(i);
                                               if (item && item->data(Qt::UserRole).toInt() == map_id)
                                               {
                                                   _continents_table->setCurrentItem(item); // calls itemSelectionChanged -> loadmap
                                                   _continents_table->scrollToItem(item);
                                                   _right_side->setCurrentIndex(0); // swap to "enter map" tab
                                                   // loadMap(map_id);
                                                   break;
                                               }
                                           }
                                       }
                                     });

    _right_side->addTab(_map_creation_wizard, "Edit map");

    layout->addWidget(_right_side);

    connect(add_btn, &QPushButton::clicked
        , [&]()
        {
            _right_side->setCurrentIndex(1);
            _map_creation_wizard->addNewMap();
        });

    widget->setStyleSheet(mapSelectionStyle());
    connect(_settings, &settings::saved, widget,
            [widget]() { widget->setStyleSheet(mapSelectionStyle()); });
    _minimap->adjustSize();
  }

  void NoggitWindow::closeEvent(QCloseEvent* event)
  {
    if (map_loaded)
    {
      event->ignore();
      promptExit(event);
    } else
    {
      if (exit_to_project_selection)
      {
        auto noggit = Noggit::Application::NoggitApplication::instance();
        auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
        project_selection->show();
      }
      else
        event->accept();
    }
    exit_to_project_selection = false;
  }

  void NoggitWindow::handleEventMapListContextMenuPinMap(int mapId, std::string MapName)
  {
    _project->pinMap(mapId, MapName);
    _buildMapListComponent->buildMapList(this);
  }

  void NoggitWindow::handleEventMapListContextMenuUnpinMap(int mapId)
  {
    _project->unpinMap(mapId);
    _buildMapListComponent->buildMapList(this);
  }

  World* NoggitWindow::getWorld()
  {
    return _map_creation_wizard->getWorld();
  }


  void NoggitWindow::promptExit(QCloseEvent* event)
  {
    emit exitPromptOpened();

    QMessageBox prompt;
    prompt.setModal(true);
    prompt.setIcon(QMessageBox::Warning);
    prompt.setText("Exit?");
    prompt.setInformativeText("Any unsaved changes will be lost.");
    prompt.addButton("Exit", QMessageBox::DestructiveRole);
    if (!exit_to_project_selection)
        prompt.addButton("Return to menu", QMessageBox::AcceptRole);
    prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
    prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowStaysOnTopHint);

    prompt.exec();

    switch (prompt.buttonRole(prompt.clickedButton()))
    {
      case QMessageBox::AcceptRole:
        _stack_widget->setCurrentIndex(0);
        _stack_widget->removeLast();
        delete _map_view;
        _map_view = nullptr;
        _minimap->world(nullptr);
        if (_map_preview_stack)
          _map_preview_stack->setCurrentIndex(0);
        {
          QSignalBlocker const blocker(_continents_table);
          _continents_table->clearSelection();
          _continents_table->setCurrentRow(-1);
        }

        map_loaded = false;
        break;
      case QMessageBox::DestructiveRole:
        Noggit::Ui::Tools::ViewportManager::ViewportManager::unloadAll();
        setCentralWidget(_null_widget = new QWidget(this));
        if (exit_to_project_selection)
        {
            auto noggit = Noggit::Application::NoggitApplication::instance();
            auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
            project_selection->show();
        }
        event->accept();
        break;
      default:
        event->ignore();
        break;
    }
  }

  void NoggitWindow::promptUidFixFailure()
  {
    _stack_widget->setCurrentIndex(0);

    QMessageBox::critical
        (nullptr, "UID fix failed", "The UID fix couldn't be done because some models were missing or fucked up.\n"
                                    "The models are listed in the log file.", QMessageBox::Ok
        );
  }
  void NoggitWindow::startWowClient()
  {
      // TODO auto login

      // TODO attach to process to allow memory edits

      // std::filesystem::path WoW_path = std::filesystem::path(Noggit::Project::CurrentProject::get()->ClientPath) / "Wow.exe";
      std::filesystem::path WoW_path = std::filesystem::path(_project->ClientPath) / "Wow.exe";

      QString program_path = WoW_path.string().c_str();
      QFileInfo checkFile(program_path);

      QStringList arguments;
      // arguments << "-console"; // deosn't seem to work
      arguments << "-windowed";
      // auto login using https://github.com/FrostAtom/awesome_wotlk, requires patched Wow.exe
      // arguments << "-login \"LOGIN\" -password \"PASSWORD\" -realmlist \"REALMLIST\" -realmname \"REALMNAME\"";


      if (checkFile.exists() && checkFile.isFile())
      {
          QProcess* process = new QProcess(this); // this parent?
          process->start(program_path, arguments);

          //Noggit::Application::NoggitApplication::instance().
          if (!process->waitForStarted()) {
              // qWarning("Failed to start process");
              QMessageBox::information(this, "Error", "Failed to start process Wow.exe");
          }
      }
      else
      {
          // qWarning("File does not exist");
          QMessageBox::information(this, "Error", std::format("File {} does not exist", WoW_path.string()).c_str());
      }
  }

  void NoggitWindow::patchWowClient()
  {
      // Option to make folder patch ?

      QDialog* mpq_patch_params = new QDialog(this);
      mpq_patch_params->setWindowFlags(Qt::Dialog);
      mpq_patch_params->setWindowTitle("Patch Export Settings");
      QVBoxLayout* mpq_patch_params_layout = new QVBoxLayout(mpq_patch_params);

      mpq_patch_params_layout->addWidget(new QLabel("<font color=orange><h4> Warning :\
          \nErrors can cause MPQ Corruption, make sure to have backup.</h4></font>\
          \nWhile Noggit is writting the archive, files are not accessible by the client, please wait.", mpq_patch_params));

      mpq_patch_params_layout->addWidget(new QLabel("MPQ Name:", mpq_patch_params));

      QLineEdit* mpq_patch_params_ledit = new QLineEdit("patch-A.MPQ", mpq_patch_params);
      QSettings settings;
      // saved last set patch name
      mpq_patch_params_ledit->setText(settings.value("noggit_window/mpq_name", "patch-A.MPQ").toString());
      mpq_patch_params_layout->addWidget(mpq_patch_params_ledit);


      QCheckBox* mpq_patch_params_locale_chk = new QCheckBox("Save DBC to Locale:", mpq_patch_params);
      mpq_patch_params_locale_chk->setToolTip("This is recommended for non English clients");
      //auto set locale mode if client isn't english.
      bool non_english = false;
      BlizzardArchive::Locale locale_mode = Noggit::Application::NoggitApplication::instance()->clientData()->locale_mode();
      if (!(locale_mode != BlizzardArchive::Locale::AUTO) && !(locale_mode != BlizzardArchive::Locale::enGB)
          && !(locale_mode != BlizzardArchive::Locale::enUS))
      {
          non_english = true;
      }
      mpq_patch_params_locale_chk->setChecked(non_english);
      // Unimplemented
      mpq_patch_params_locale_chk->setHidden(true);
      mpq_patch_params_locale_chk->setDisabled(true);
      mpq_patch_params_layout->addWidget(mpq_patch_params_locale_chk);


      // mode choice between replace archive and add files to archive
      QCheckBox* mpq_overwrite_chk = new QCheckBox("Overwrite Archive", mpq_patch_params);
      mpq_overwrite_chk->setToolTip("If there is already an archive with this name, it will be deleted and replaced by a new one.");
      mpq_overwrite_chk->setChecked(false);
      // Unimplemented
      mpq_overwrite_chk->setHidden(true);
      mpq_overwrite_chk->setDisabled(true);
      mpq_patch_params_layout->addWidget(mpq_overwrite_chk);


      QCheckBox* mpq_compact_chk = new QCheckBox("Compact Archive", mpq_patch_params);
      mpq_compact_chk->setToolTip("Adding and removing files creates empty space in the MPQ, making it grow in size.\
          \nCompacting takes a few seconds depending on patch size, it is recommended to compact when many files have been added.");
      mpq_compact_chk->setChecked(false);
      mpq_patch_params_layout->addWidget(mpq_compact_chk);

      QCheckBox* mpq_compress_files_chk = new QCheckBox("Compress Files (slow)", mpq_patch_params);
      mpq_compress_files_chk->setToolTip("Files will be compressed in the MPQ, but writting becomes much slower(about 3x slower).\
          \nRecommended for distribution.");
      mpq_compress_files_chk->setChecked(true);
      mpq_patch_params_layout->addWidget(mpq_compress_files_chk);


      QPushButton* mpq_patch_params_okay = new QPushButton("Save Project to Client MPQ", mpq_patch_params);
      mpq_patch_params_layout->addWidget(mpq_patch_params_okay);

      connect(mpq_patch_params_okay, &QPushButton::clicked
          , [=]()
          {
              // check if mpq name is allowed
              if (!Noggit::Application::NoggitApplication::instance()->clientData()->isMPQNameValid(mpq_patch_params_ledit->text().toLower().toStdString(), true))
              {
                  QMessageBox::warning(this, "Name Error", "MPQ Name is not allowed.\This name is already used by base client patches, or the client can't load it.\
                     \nYour patch must be named \"patch-[4-9].MPQ\" or \"patch-[A-Z].MPQ\".");
              }
              else
              {
                QSettings settings;
                settings.setValue("noggit_window/mpq_name", mpq_patch_params_ledit->text());
                settings.sync();

                mpq_patch_params->accept();
              }
          });

      // execute dialog, and run code when Mpq_patch_params_okay calls Mpq_patch_params->accept();
      if (mpq_patch_params->exec() == QDialog::Accepted)
      {
          // OK button was pressed, do stuff.

          auto archive_name = mpq_patch_params_ledit->text().toStdString();
          auto clientData = Noggit::Application::NoggitApplication::instance()->clientData();

          namespace fs = std::filesystem;
          // progress bar, count maximum files
          // ignore directories, only count files
          auto regular_file = [](const fs::path& path) {
              std::string const extension = path.extension().string();
              // filter noggit files
              if (extension == ".noggitproj" || extension == ".json" || extension == ".ini")
                  return false;
              return fs::is_regular_file(path);
              };

          int totalItems = std::count_if(fs::recursive_directory_iterator(clientData->projectPath()), {}, regular_file);

          if (!totalItems)
          {
              QMessageBox::warning(this, "Error", std::format("No files found in project {}", clientData->projectPath()).c_str());
              return;
          }

          if (!clientData->mpqArchiveExistsOnDisk(archive_name))
          {
              try
              {
                  auto archive = clientData->tryCreateMPQArchive(archive_name);
              }
              catch (BlizzardArchive::Exceptions::Archive::ArchiveOpenError& e)
              {
                  QMessageBox::critical(nullptr, "Error", e.what());
              }
              catch (...)
              {
                  QMessageBox::critical(nullptr, "Error", "Failed to create MPQ Archive. Unhandled exception.");
              }
          }
          {
              auto archive = clientData->getMPQArchive(archive_name);
              if (archive.has_value())
              {
                  auto progress_box = new QMessageBox(QMessageBox::Information, "Working...", std::format("Saving {} files to patch {}...\
                      \nClosing the program now can corrupt the MPQ.", std::to_string(totalItems), archive_name ).c_str(), QMessageBox::StandardButton::NoButton, this);
                  progress_box->setStandardButtons(QMessageBox::NoButton);
                  progress_box->setWindowFlags(progress_box->windowFlags() & ~Qt::WindowCloseButtonHint);
                  // progress_box->exec(); // this stops code execution
                  progress_box->repaint();
                  qApp->processEvents();

                  try
                  {
                      auto start = std::chrono::high_resolution_clock::now();

                      std::array<int, 2> result = clientData->saveLocalFilesToArchive(archive.value(), mpq_compress_files_chk->isChecked(), mpq_compact_chk->isChecked());
                      int processed_files = result[0];
                      int files_failed = processed_files - result[1];

                      auto end = std::chrono::high_resolution_clock::now();
                      std::chrono::duration<double> duration = end - start;
                      std::ostringstream oss; // duration in seconds with 1 digit
                      oss << std::fixed << std::setprecision(1) << duration.count();

                      // if no file was processed, archive was most likely opened and not accessible
                      // TODO : we can throw an error message in saveLocalFilesToArchive if (!archive->openForWritting()) instead
                      if (!processed_files)
                      {
                          QMessageBox::warning(this, "Error", "Project Folder is not a valid directory or client MPQ is not accessible.\
                              \nMake sure it isn't opened by Wow or MPQ editor");
                      }
                      else
                          QMessageBox::information(this, "Archive Updated", std::format("Added {} files to existing Archive {} in {} seconds.\
                        \n{} Files failed.",  std::to_string(processed_files), archive_name, oss.str(), std::to_string(files_failed)).c_str());


                  }
                  catch (...)
                  {
                      QMessageBox::critical(nullptr, "Error", "unhandled exception");
                  }

                  progress_box->close();
              }
              else
                  QMessageBox::warning(this, "Error", std::format("Error accessing archive {}", archive_name).c_str());
          }
      }
  }
}
