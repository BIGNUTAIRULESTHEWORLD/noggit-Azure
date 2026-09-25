#include <noggit/ui/NpcTemplateBrowser.hpp>

#include <noggit/DBCFile.h>
#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapView.h>
#include <noggit/ActionManager.hpp>
#include <noggit/database/WorldDatabaseSchema.hpp>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/ModelManager.h>
#include <noggit/NpcSpawnOverlay.hpp>
#include <noggit/SceneObject.hpp>
#include <noggit/TextureManager.h>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/World.h>
#include <noggit/ui/tools/AssetBrowser/BrowserModelView.hpp>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>
#include <noggit/ui/NpcFactionSelector.hpp>

#include <blizzard-archive-library/include/Listfile.hpp>

#include <opengl/context.hpp>
#include <math/ray.hpp>

#include <glm/gtc/constants.hpp>

#include <QAbstractTableModel>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QImage>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QCursor>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace Noggit::Ui
{
  class NpcThumbnailRenderer final : public Tools::PreviewRenderer
  {
  public:
    explicit NpcThumbnailRenderer(QWidget* parent)
      : PreviewRenderer(112, 112, Noggit::NoggitRenderContext::NPC_BROWSER_PREVIEW, parent)
    {
      setVisible(false);
    }

    QPixmap renderAppearance(Noggit::NpcAppearance const& appearance,
                             std::string const& cache_variant)
    {
      if (appearance.body.model_path.empty())
        throw std::runtime_error("the creature appearance has no body model");

      // A prior background-only capture must never be reused after its model
      // or GL setup has been corrected/retried.
      clearPixmapCache();

      OpenGL::context::save_current_context const saved_context(::gl);
      if (!offscreenContext().makeCurrent(&offscreenSurface()))
        throw std::runtime_error("the NPC thumbnail OpenGL context could not be activated");
      OpenGL::context::scoped_setter const context_set(::gl, &offscreenContext());

      _attachment_ids.clear();
      PreviewRenderer::setModel(appearance.body.model_path);
      if (_model_instances.empty() || !_model_instances.front().model.get()
          || _model_instances.front().model->loading_failed()
          || _model_instances.front().model->skin_load_failed())
        throw std::runtime_error("the NPC body M2 or its skin could not be loaded");
      applyAppearance(_model_instances.front(), appearance.body);

      for (NpcAttachmentAppearance const& attachment : appearance.attachments)
      {
        if (attachment.model.model_path.empty()) continue;
        auto& instance = _model_instances.emplace_back(
          attachment.model.model_path,
          Noggit::NoggitRenderContext::NPC_BROWSER_PREVIEW);
        instance.model->wait_until_loaded();
        if (instance.model->loading_failed() || instance.model->skin_load_failed())
        {
          _model_instances.pop_back();
          continue;
        }
        applyAppearance(instance, attachment.model);
        _attachment_ids.push_back(attachment.render_attachment_id
          ? attachment.render_attachment_id : attachment.attachment_id);
      }

      // Attachments and display-specific geosets are now final. Frame the body
      // once, immediately before drawing, rather than inheriting camera state
      // from the hidden appearance/placement viewer.
      resetCamera();

      QPixmap* rendered = renderToPixmap(cache_variant);
      if (!rendered || rendered->isNull())
        throw std::runtime_error("the NPC thumbnail capture returned no image");

      QImage const image = rendered->toImage().convertToFormat(QImage::Format_ARGB32);
      int min_red = 255, min_green = 255, min_blue = 255;
      int max_red = 0, max_green = 0, max_blue = 0;
      for (int y = 0; y < image.height(); ++y)
      {
        auto const* row = reinterpret_cast<QRgb const*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
          QRgb const pixel = row[x];
          min_red = std::min(min_red, qRed(pixel));
          min_green = std::min(min_green, qGreen(pixel));
          min_blue = std::min(min_blue, qBlue(pixel));
          max_red = std::max(max_red, qRed(pixel));
          max_green = std::max(max_green, qGreen(pixel));
          max_blue = std::max(max_blue, qBlue(pixel));
        }
      }
      if (std::max({max_red - min_red, max_green - min_green, max_blue - min_blue}) < 8)
        throw std::runtime_error("the NPC draw produced only the background color");

      return *rendered;
    }

  protected:
    std::optional<glm::mat4x4> modelInstanceTransform(std::size_t index) const override
    {
      if (index > 0 && index - 1 < _attachment_ids.size())
      {
        auto const& body = _model_instances.front();
        return body.model->attachmentTransform(
          _attachment_ids[index - 1], body.transformMatrix());
      }
      return PreviewRenderer::modelInstanceTransform(index);
    }

  private:
    static void applyAppearance(ModelInstance& instance,
                                Noggit::NpcModelAppearance const& appearance)
    {
      Model* model = instance.model.get();
      if (!model)
        throw std::runtime_error("the creature model reference is unavailable");

      std::size_t const geosets = std::min(model->showGeosets.size(),
                                           appearance.geosets.size());
      for (std::size_t index = 0; index < geosets; ++index)
        model->showGeosets[index] = appearance.geosets[index];

      for (auto const& [type, filename] : appearance.replacement_textures)
      {
        if (!filename.empty())
          model->_replaceTextures.insert_or_assign(
            type, scoped_blp_texture_reference(
              filename, Noggit::NoggitRenderContext::NPC_BROWSER_PREVIEW));
      }
    }

    std::vector<unsigned> _attachment_ids;
  };

namespace
{
    constexpr std::uint64_t PLACEMENT_OVERLAY_GUID = std::numeric_limits<std::uint64_t>::max();
    struct NpcTemplate
    {
      unsigned entry = 0;
      QString name;
      QString subname;
      unsigned model = 0;
      unsigned faction = 0;
      unsigned flags = 0;
      int type = 0;
      int family = 0;
      int min_level = 0;
      int max_level = 0;
      int rank = 0;
      QString ai_name;
      QString script_name;
      QIcon thumbnail;
      QString thumbnail_error;
    };

    QString typeName(int type)
    {
      static char const* const names[] = {
        "Unspecified", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical",
        "Unspecified", "Totem", "Non-combat pet", "Gas cloud"
      };
      return type >= 0 && type < static_cast<int>(sizeof(names) / sizeof(names[0]))
        ? QString::fromLatin1(names[type]) : QString("Type %1").arg(type);
    }

    QString roleName(unsigned flags)
    {
      QStringList roles;
      if (flags & 2u) roles << "Quest giver";
      if (flags & 128u) roles << "Vendor";
      if (flags & 16u) roles << "Trainer";
      if (flags & 8192u) roles << "Flight master";
      if (flags & 1u) roles << "Gossip";
      return roles.isEmpty() ? "None" : roles.join(", ");
    }

    QString templateDetails(NpcTemplate const& npc)
    {
      return QString("<b>%1</b> %2<br>Template ID: %3<br>Faction template ID: %4<br>"
                     "Type: %5 · Family ID: %6 · Rank: %7<br>Level: %8-%9<br>"
                     "Role: %10<br>Display ID: %11<br>AI: %12 · Script: %13")
        .arg(npc.name.toHtmlEscaped(), npc.subname.toHtmlEscaped())
        .arg(npc.entry).arg(npc.faction).arg(typeName(npc.type))
        .arg(npc.family).arg(npc.rank).arg(npc.min_level).arg(npc.max_level)
        .arg(roleName(npc.flags)).arg(npc.model)
        .arg(npc.ai_name.toHtmlEscaped(), npc.script_name.toHtmlEscaped());
    }

    std::string texturePath(std::string texture, std::string const& directory)
    {
      std::replace(texture.begin(), texture.end(), '/', '\\');
      if (texture.empty()) return {};
      QString const name = QString::fromStdString(texture);
      if (!name.endsWith(".blp", Qt::CaseInsensitive)) texture += ".blp";
      if (texture.find('\\') == std::string::npos) texture = directory + texture;
      return texture;
    }

    std::string itemModelStem(std::string stem)
    {
      std::replace(stem.begin(), stem.end(), '/', '\\');
      QString const name = QString::fromStdString(stem);
      if (name.endsWith(".mdx", Qt::CaseInsensitive)
          || name.endsWith(".mdl", Qt::CaseInsensitive)
          || name.endsWith(".m2", Qt::CaseInsensitive))
        stem.resize(stem.find_last_of('.'));
      return stem;
    }

    std::string helmetModelPath(std::string stem, std::string const& race_prefix, unsigned sex)
    {
      stem = itemModelStem(std::move(stem));
      if (stem.empty() || race_prefix.empty() || sex > 1) return {};
      if (stem.find('\\') != std::string::npos) return stem + ".m2";
      return "Item\\ObjectComponents\\Head\\" + stem + "_" + race_prefix
          + (sex == 0 ? "M" : "F") + ".m2";
    }

    std::string shoulderModelPath(std::string stem)
    {
      stem = itemModelStem(std::move(stem));
      if (stem.empty()) return {};
      return stem.find('\\') == std::string::npos
        ? "Item\\ObjectComponents\\Shoulder\\" + stem + ".m2" : stem + ".m2";
    }

    bool setWeaponAttachment(Tools::AssetBrowser::ModelViewer* preview,
                             DBCFile* item_display_info,
                             std::shared_ptr<BlizzardArchive::ClientData> const& client_data,
                             unsigned display_id, unsigned inventory_type,
                             unsigned attachment_id,
                             unsigned render_attachment_id = 0)
    {
      if (!preview || !display_id || !item_display_info || !client_data) return false;
      try
      {
        auto const display_record = item_display_info->getByID(display_id);
        std::string stem = itemModelStem(display_record.getString(1));
        if (stem.empty()) return false;

        std::vector<std::string> directories;
        if (stem.find('\\') != std::string::npos)
          directories.emplace_back();
        else if (inventory_type == 14 || inventory_type == 23)
          directories = {"Item\\ObjectComponents\\Shield\\",
                         "Item\\ObjectComponents\\Weapon\\",
                         "Item\\ObjectComponents\\Misc\\"};
        else
          directories = {"Item\\ObjectComponents\\Weapon\\",
                         "Item\\ObjectComponents\\Shield\\",
                         "Item\\ObjectComponents\\Misc\\"};

        std::string model_path;
        for (std::string const& directory : directories)
        {
          std::string const candidate = directory + stem + ".m2";
          if (client_data->exists(BlizzardArchive::Listfile::FileKey(candidate)))
          {
            model_path = candidate;
            break;
          }
        }
        if (model_path.empty()) return false;

        std::string texture_path = display_record.getString(3);
        std::replace(texture_path.begin(), texture_path.end(), '/', '\\');
        if (!texture_path.empty())
        {
          if (texture_path.find('\\') == std::string::npos)
            texture_path = model_path.substr(0, model_path.find_last_of("\\/") + 1)
                + texture_path;
          if (!QString::fromStdString(texture_path).endsWith(".blp", Qt::CaseInsensitive))
            texture_path += ".blp";
          if (!client_data->exists(BlizzardArchive::Listfile::FileKey(texture_path)))
            texture_path.clear();
        }

        // Character shields use socket 0; other off-hand items use socket 2.
        if (inventory_type == 14 && attachment_id == 2)
          attachment_id = 0;
        return preview->setCreatureAttachment(attachment_id, model_path, texture_path,
                                              render_attachment_id);
      }
      catch (...) { return false; }
    }

    float normalizedYaw(float degrees)
    {
      degrees = std::fmod(degrees, 360.0f);
      if (degrees > 180.0f) degrees -= 360.0f;
      if (degrees <= -180.0f) degrees += 360.0f;
      return degrees;
    }

    float serverOrientation(float yaw_degrees)
    {
      yaw_degrees = normalizedYaw(yaw_degrees);
      float const model_angle = yaw_degrees < 0.0f
        ? std::abs(yaw_degrees) + 180.0f : std::abs(yaw_degrees - 180.0f);
      float orientation = 2.0f * glm::pi<float>() - glm::radians(model_angle);
      orientation = std::fmod(orientation, 2.0f * glm::pi<float>());
      return orientation < 0.0f ? orientation + 2.0f * glm::pi<float>() : orientation;
    }
  }

  class NpcTemplateTableModel : public QAbstractTableModel
  {
  public:
    using QAbstractTableModel::QAbstractTableModel;

    int rowCount(QModelIndex const& parent = {}) const override
    {
      return parent.isValid() ? 0 : static_cast<int>(_entries.size());
    }

    int columnCount(QModelIndex const& parent = {}) const override
    {
      return parent.isValid() ? 0 : 7;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
      if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
      static char const* const headers[] = {"Name", "ID", "Faction", "Type", "Level", "Role", "Display ID"};
      return section >= 0 && section < 7 ? QVariant(headers[section]) : QVariant();
    }

    QVariant data(QModelIndex const& index, int role) const override
    {
      if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
      auto const& npc = _entries[static_cast<std::size_t>(index.row())];
      if (role == Qt::DecorationRole && index.column() == 0)
        return npc.thumbnail;
      if (role == Qt::ToolTipRole && index.column() == 0)
        return QString("%1\nTemplate %2 · Display %3\n%4 · Level %5-%6%7")
          .arg(npc.name).arg(npc.entry).arg(npc.model).arg(typeName(npc.type))
          .arg(npc.min_level).arg(npc.max_level)
          .arg(npc.thumbnail_error.isEmpty() ? QString()
            : QString("\nPreview failed: %1").arg(npc.thumbnail_error));
      if (role == Qt::UserRole)
      {
        switch (index.column())
        {
          case 0: return npc.name.toCaseFolded();
          case 1: return npc.entry;
          case 2: return npc.faction;
          case 3: return npc.type;
          case 4: return npc.min_level;
          case 5: return roleName(npc.flags);
          case 6: return npc.model;
          default: break;
        }
      }
      if (role != Qt::DisplayRole) return {};
      switch (index.column())
      {
        case 0: return QString("%1\nID %2").arg(npc.name).arg(npc.entry);
        case 1: return npc.entry;
        case 2: return npc.faction;
        case 3: return typeName(npc.type);
        case 4: return npc.min_level == npc.max_level
          ? QString::number(npc.min_level)
          : QString("%1-%2").arg(npc.min_level).arg(npc.max_level);
        case 5: return roleName(npc.flags);
        case 6: return npc.model;
        default: return {};
      }
    }

    NpcTemplate const* entry(int row) const
    {
      return row >= 0 && row < rowCount() ? &_entries[static_cast<std::size_t>(row)] : nullptr;
    }

    void replace(std::vector<NpcTemplate> entries)
    {
      beginResetModel();
      _entries = std::move(entries);
      endResetModel();
    }

    std::vector<NpcTemplate> const& entries() const { return _entries; }

    void setThumbnail(int row, QIcon icon)
    {
      if (row < 0 || row >= rowCount()) return;
      _entries[static_cast<std::size_t>(row)].thumbnail = std::move(icon);
      _entries[static_cast<std::size_t>(row)].thumbnail_error.clear();
      QModelIndex const changed = index(row, 0);
      emit dataChanged(changed, changed, {Qt::DecorationRole, Qt::ToolTipRole});
    }

    void setThumbnailError(int row, QString error)
    {
      if (row < 0 || row >= rowCount()) return;
      _entries[static_cast<std::size_t>(row)].thumbnail_error = std::move(error);
      QModelIndex const changed = index(row, 0);
      emit dataChanged(changed, changed, {Qt::ToolTipRole});
    }

  private:
    std::vector<NpcTemplate> _entries;
  };

  class NpcTemplateFilterModel : public QSortFilterProxyModel
  {
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setCriteria(QString search, int faction, int type, int role)
    {
      _search = std::move(search);
      _faction = faction;
      _type = type;
      _role = role;
      invalidateFilter();
    }

  protected:
    bool filterAcceptsRow(int source_row, QModelIndex const&) const override
    {
      auto const* model = static_cast<NpcTemplateTableModel const*>(sourceModel());
      auto const* npc = model->entry(source_row);
      if (!npc) return false;
      if (_faction >= 0 && static_cast<int>(npc->faction) != _faction) return false;
      if (_type >= 0 && npc->type != _type) return false;
      if (_role == -2 && npc->flags != 0) return false;
      if (_role > 0 && !(npc->flags & static_cast<unsigned>(_role))) return false;
      if (_search.isEmpty()) return true;
      return npc->name.contains(_search, Qt::CaseInsensitive)
          || npc->subname.contains(_search, Qt::CaseInsensitive)
          || QString::number(npc->entry).contains(_search)
          || QString::number(npc->faction).contains(_search)
          || QString::number(npc->model).contains(_search);
    }

  private:
    QString _search;
    int _faction = -1;
    int _type = -1;
    int _role = -1;
  };

  class NpcPageFilterModel : public QSortFilterProxyModel
  {
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setPage(int page)
    {
      int const maximum = std::max(0, pageCount() - 1);
      int const next = std::clamp(page, 0, maximum);
      if (_page == next) return;
      _page = next;
      invalidateFilter();
    }

    void resetPage()
    {
      _page = 0;
      invalidateFilter();
    }

    int page() const { return _page; }
    int pageSize() const { return _page_size; }
    int pageCount() const
    {
      int const rows = sourceModel() ? sourceModel()->rowCount() : 0;
      return std::max(1, (rows + _page_size - 1) / _page_size);
    }

  protected:
    bool filterAcceptsRow(int source_row, QModelIndex const&) const override
    {
      int const first = _page * _page_size;
      return source_row >= first && source_row < first + _page_size;
    }

  private:
    int _page = 0;
    int _page_size = 8;
  };

  NpcTemplateBrowser::NpcTemplateBrowser(std::shared_ptr<BlizzardArchive::ClientData> client_data,
                                         MapView* map_view, QWidget* parent)
    : QWidget(parent), _map_view(map_view), _client_data(std::move(client_data))
  {
    setMinimumWidth(248);
    setMaximumWidth(300);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    _connection_toggle = new QToolButton(this);
    _connection_toggle->setText("World database connection");
    _connection_toggle->setCheckable(true);
    _connection_toggle->setChecked(false);
    _connection_toggle->setArrowType(Qt::RightArrow);
    _connection_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(_connection_toggle);
    _connection_panel = new QWidget(this);
    auto* connection_panel_layout = new QVBoxLayout(_connection_panel);
    connection_panel_layout->setContentsMargins(0, 0, 0, 0);
    auto* connection = new QFormLayout();
    connection->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    QSettings settings;
    _host = new QLineEdit(settings.value("npc_browser/host", settings.value("project/mysql/server", "localhost")).toString(), this);
    _port = new QSpinBox(this);
    _port->setRange(1, 65535);
    _port->setValue(settings.value("npc_browser/port", settings.value("project/mysql/port", 3306)).toInt());
    QString world_database = settings.value("npc_browser/database", "acore_world").toString();
    if (world_database == "world") world_database = "acore_world";
    _database = new QLineEdit(world_database, this);
    _user = new QLineEdit(settings.value("npc_browser/user", settings.value("project/mysql/user", "root")).toString(), this);
    _password = new QLineEdit(this);
    _password->setEchoMode(QLineEdit::Password);
    _password->setPlaceholderText("Not saved");
    _client_data_folder = new QLineEdit(
      settings.value("npc_browser/client_data_folder").toString(), this);
    _server_data_folder = new QLineEdit(
      settings.value("npc_browser/server_data_folder").toString(), this);
    connection->addRow("Server", _host);
    connection->addRow("Port", _port);
    connection->addRow("World database", _database);
    connection->addRow("User", _user);
    connection->addRow("Password", _password);

    auto directory_field = [this](QLineEdit* field, QString const& title,
                                  QString const& setting_key)
    {
      auto* row = new QWidget(_connection_panel);
      auto* row_layout = new QHBoxLayout(row);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->addWidget(field, 1);
      auto* browse = new QPushButton("...", row);
      browse->setMaximumWidth(32);
      row_layout->addWidget(browse);
      connect(browse, &QPushButton::clicked, this, [this, field, title, setting_key]
      {
        QString const selected = QFileDialog::getExistingDirectory(
          this, title, field->text().trimmed(), QFileDialog::ShowDirsOnly);
        if (selected.isEmpty()) return;
        field->setText(QDir::toNativeSeparators(selected));
        QSettings().setValue(setting_key, field->text().trimmed());
      });
      connect(field, &QLineEdit::editingFinished, this, [field, setting_key]
      {
        QSettings().setValue(setting_key, field->text().trimmed());
      });
      return row;
    };
    _client_data_folder->setToolTip(
      "World of Warcraft Data folder. Updated DBCs are copied into DBFilesClient.");
    _server_data_folder->setToolTip(
      "World server data folder. Updated DBCs are copied into its dbc directory.");
    connection->addRow("Client Data folder",
      directory_field(_client_data_folder, "Select client Data folder",
                      "npc_browser/client_data_folder"));
    connection->addRow("Server data folder",
      directory_field(_server_data_folder, "Select server data folder",
                      "npc_browser/server_data_folder"));
    connection_panel_layout->addLayout(connection);

    auto* deploy_dbcs = new QPushButton("Deploy NPC DBCs now", _connection_panel);
    deploy_dbcs->setToolTip(
      "Copy the project's NPC display and sound DBC files to the configured client and server folders.");
    connection_panel_layout->addWidget(deploy_dbcs);
    connect(deploy_dbcs, &QPushButton::clicked, this, [this]
    {
      QString error;
      if (deployNpcDbcs(error))
        _status->setText("NPC display and sound DBCs deployed to the client and server data folders.");
      else
        QMessageBox::critical(this, "NPC DBC deployment failed", error);
    });

    _load = new QPushButton("Load NPC templates", _connection_panel);
    connection_panel_layout->addWidget(_load);
    layout->addWidget(_connection_panel);
    _connection_panel->hide();
    connect(_load, &QPushButton::clicked, this, [this] { loadTemplates(); });
    connect(_connection_toggle, &QToolButton::toggled, this, [this](bool expanded)
    {
      _connection_panel->setVisible(expanded);
      _connection_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    auto* phase_row = new QHBoxLayout;
    auto* phase_label = new QLabel("Active phase mask", this);
    _phase_mask = new QComboBox(this);
    _phase_mask->setEditable(true);
    _phase_mask->setInsertPolicy(QComboBox::NoInsert);
    _phase_mask->setValidator(new QRegularExpressionValidator(
      QRegularExpression("[0-9]{1,10}"), _phase_mask));
    _phase_mask->setToolTip(
      "Select a phase bit (1 = phase 1, 2 = phase 2, 4 = phase 3, etc.), "
      "or type a combined mask such as 3 for phases 1 and 2. "
      "NPCs and gameobjects are shown when their phaseMask shares a selected bit.");
    for (unsigned phase = 0; phase < 32; ++phase)
    {
      unsigned const mask = 1u << phase;
      _phase_mask->addItem(QString::number(mask));
      _phase_mask->setItemData(phase,
        QString("Phase %1 (mask %2)").arg(phase + 1).arg(mask), Qt::ToolTipRole);
    }
    _phase_mask->addItem("4294967295");
    _phase_mask->setItemData(32, "All 32 phases", Qt::ToolTipRole);
    _phase_mask->setCurrentIndex(0);
    phase_row->addWidget(phase_label);
    phase_row->addWidget(_phase_mask, 1);
    layout->addLayout(phase_row);
    connect(_phase_mask, &QComboBox::currentTextChanged, this,
            [this](QString const& text)
    {
      bool valid = false;
      qulonglong const mask = text.toULongLong(&valid);
      if (!valid || mask == 0 || mask > std::numeric_limits<unsigned>::max()) return;
      setActivePhaseMask(static_cast<unsigned>(mask));
    });

    _event_selector = new QToolButton(this);
    _event_selector->setText("Active events: None");
    _event_selector->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _event_selector->setPopupMode(QToolButton::InstantPopup);
    _event_selector->setToolTip(
      "Preview AzerothCore game events independently of the phase mask. "
      "No events shows the normal world without holiday or event-only spawns.");
    _event_menu = new QMenu(_event_selector);
    _event_selector->setMenu(_event_menu);
    _event_selector->setEnabled(false);
    layout->addWidget(_event_selector);

    _load_nearby_spawns = new QPushButton("Refresh map NPC/gameobject cache", this);
    _load_nearby_spawns->setToolTip(
      "Read the map's NPC and server gameobject spawns once, index them by tile, and show nearby spawns "
      "automatically as loaded tiles and the camera change.");
    layout->addWidget(_load_nearby_spawns);
    connect(_load_nearby_spawns, &QPushButton::clicked, this,
            [this] { loadNearbySpawns(); });

    _create_npc = new QToolButton(this);
    _create_npc->setText("Create NPC...");
    _create_npc->setToolButtonStyle(Qt::ToolButtonTextOnly);
    _create_npc->setPopupMode(QToolButton::InstantPopup);
    auto* create_menu = new QMenu(_create_npc);
    auto* create_humanoid = create_menu->addAction("New humanoid NPC...");
    create_humanoid->setToolTip(
      "Build a playable-race NPC from race, sex, skin, face, hair, and facial-hair options.");
    auto* create_from_template = create_menu->addAction("Create from selected template...");
    create_from_template->setToolTip(
      "Clone the selected template into a new custom NPC, edit it, then place it.");
    auto* customize_spawn = create_menu->addAction("Customize selected spawn...");
    customize_spawn->setToolTip(
      "Create a unique template for the selected saved spawn and edit its equipment.");
    _create_npc->setMenu(create_menu);
    layout->addWidget(_create_npc);
    connect(create_humanoid, &QAction::triggered, this,
            [this] { openNpcEditor(false, true); });
    connect(create_from_template, &QAction::triggered, this,
            [this] { openNpcEditor(false); });
    connect(customize_spawn, &QAction::triggered, this,
            [this] { openNpcEditor(true); });
    connect(create_menu, &QMenu::aboutToShow, this,
            [this, create_from_template, customize_spawn]
    {
      create_from_template->setEnabled(selectedTemplateEntry() != 0);
      customize_spawn->setEnabled(_selected_spawn_guid.has_value());
    });

    _delete_npc_template = new QPushButton("Delete selected custom NPC...", this);
    _delete_npc_template->setEnabled(false);
    _delete_npc_template->setToolTip(
      "Permanently remove a humanoid NPC created by Noggit. Built-in and shared server "
      "templates are protected.");
    layout->addWidget(_delete_npc_template);
    connect(_delete_npc_template, &QPushButton::clicked, this,
            [this] { deleteSelectedCustomNpc(); });

    _search = new QLineEdit(this);
    _search->setPlaceholderText("Search name, template ID, faction ID, or display ID");
    layout->addWidget(_search);
    auto* filters = new QHBoxLayout();
    _faction = new QComboBox(this);
    _type = new QComboBox(this);
    _role = new QComboBox(this);
    _faction->addItem("All factions", -1);
    _type->addItem("All types", -1);
    _role->addItem("All roles", -1);
    _role->addItem("Quest giver", 2);
    _role->addItem("Vendor", 128);
    _role->addItem("Trainer", 16);
    _role->addItem("Flight master", 8192);
    _role->addItem("Gossip", 1);
    _role->addItem("No role flags", -2);
    filters->addWidget(_faction);
    filters->addWidget(_type);
    filters->addWidget(_role);
    layout->addLayout(filters);

    _model = new NpcTemplateTableModel(this);
    _filter = new NpcTemplateFilterModel(this);
    _filter->setSourceModel(_model);
    _filter->setSortRole(Qt::UserRole);
    _filter->sort(0, Qt::AscendingOrder);
    _page_filter = new NpcPageFilterModel(this);
    _page_filter->setSourceModel(_filter);
    _table = new QListView(this);
    _table->setModel(_page_filter);
    _table->setViewMode(QListView::IconMode);
    _table->setMovement(QListView::Static);
    _table->setResizeMode(QListView::Adjust);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setIconSize(QSize(88, 88));
    _table->setGridSize(QSize(116, 132));
    _table->setSpacing(4);
    _table->setWordWrap(true);
    _table->setUniformItemSizes(true);
    _table->installEventFilter(this);
    _table->viewport()->installEventFilter(this);

    _properties_panel = new QWidget(this);
    _properties_panel->setMinimumWidth(300);
    _properties_panel->setMaximumWidth(360);
    auto* details_layout = new QVBoxLayout(_properties_panel);
    details_layout->setContentsMargins(6, 6, 6, 6);
    _details = new QLabel("Select an NPC to inspect its template.", _properties_panel);
    _details->setWordWrap(true);
    details_layout->addWidget(_details);
    _preview = new Tools::AssetBrowser::ModelViewer(
      _properties_panel, Noggit::NoggitRenderContext::NPC_BROWSER, 112, 112);
    _cache_preview = new Tools::AssetBrowser::ModelViewer(
      _properties_panel, Noggit::NoggitRenderContext::NPC_SPAWN_CACHE, 112, 112);
    _cache_preview->hide();
    _thumbnail_renderer = std::make_unique<NpcThumbnailRenderer>(this);
    _preview->setMinimumHeight(180);
    _preview_speed_note = new QLabel(_properties_panel);
    _preview_speed_note->hide();
    auto const update_speed_note = [this]
    {
      _preview_speed_note->setText(QString("Preview speed: %1x (wheel up faster, wheel down slower)")
                                    .arg(_preview->getMoveSensitivity() / 0.5f, 0, 'f', 2));
    };
    connect(_preview, &Tools::AssetBrowser::ModelViewer::sensitivity_changed,
            this, update_speed_note);
    update_speed_note();
    _preview_note = new QLabel("Select an NPC to preview its client model and textures.", _properties_panel);
    _preview_note->setWordWrap(true);
    _preview_note->hide();

    _placement_panel = new QGroupBox("NPC stamp tool", _properties_panel);
    auto* placement_layout = new QVBoxLayout(_placement_panel);
    auto* placement_help = new QLabel(
      "Move over loaded terrain or a WMO surface and left-click to create a spawn. "
      "Keep clicking to place more. "
      "Hold R and move the mouse horizontally to rotate, or press Esc to stop.",
      _placement_panel);
    placement_help->setWordWrap(true);
    placement_layout->addWidget(placement_help);
    _placement_summary = new QLabel(_placement_panel);
    _placement_summary->setWordWrap(true);
    _placement_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    placement_layout->addWidget(_placement_summary);
    auto* spawn_settings = new QFormLayout();
    _respawn_seconds = new QSpinBox(_placement_panel);
    _respawn_seconds->setRange(1, 7 * 24 * 60 * 60);
    _respawn_seconds->setValue(120);
    _respawn_seconds->setSuffix(" seconds");
    spawn_settings->addRow("Respawn", _respawn_seconds);
    placement_layout->addLayout(spawn_settings);
    _cancel_placement = new QPushButton("Stop placing (Esc)", _placement_panel);
    placement_layout->addWidget(_cancel_placement);
    auto* placement_note = new QLabel(
      "Each left-click writes one creature row to the world database. The ADT is unchanged.",
      _placement_panel);
    placement_note->setWordWrap(true);
    placement_layout->addWidget(placement_note);
    _placement_panel->hide();
    details_layout->addWidget(_placement_panel);

    _phase_assignment_panel = new QGroupBox("Selected server spawn phase", _properties_panel);
    auto* phase_assignment_layout = new QVBoxLayout(_phase_assignment_panel);
    _phase_assignment_identity = new QLabel(_phase_assignment_panel);
    _phase_assignment_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _phase_assignment_identity->setWordWrap(true);
    phase_assignment_layout->addWidget(_phase_assignment_identity);
    auto* phase_assignment_form = new QFormLayout;
    _phase_assignment_mask = new QComboBox(_phase_assignment_panel);
    _phase_assignment_mask->setEditable(true);
    _phase_assignment_mask->setInsertPolicy(QComboBox::NoInsert);
    _phase_assignment_mask->setValidator(new QRegularExpressionValidator(
      QRegularExpression("[0-9]{1,10}"), _phase_assignment_mask));
    _phase_assignment_mask->setToolTip(
      "Choose one phase bit or type a combined mask, such as 3 for phases 1 and 2.");
    for (unsigned phase = 0; phase < 32; ++phase)
      _phase_assignment_mask->addItem(QString::number(1u << phase));
    _phase_assignment_mask->addItem("4294967295");
    phase_assignment_form->addRow("Spawn phaseMask", _phase_assignment_mask);
    phase_assignment_layout->addLayout(phase_assignment_form);
    auto* save_phase_assignment = new QPushButton("Save phase for this spawn",
                                                  _phase_assignment_panel);
    phase_assignment_layout->addWidget(save_phase_assignment);
    auto* phase_assignment_note = new QLabel(
      "Writes this NPC or gameobject's phaseMask to the world database. "
      "To view it afterward, select a matching active phase above.",
      _phase_assignment_panel);
    phase_assignment_note->setWordWrap(true);
    phase_assignment_layout->addWidget(phase_assignment_note);
    _phase_assignment_panel->hide();
    details_layout->addWidget(_phase_assignment_panel);
    connect(save_phase_assignment, &QPushButton::clicked, this,
            [this] { saveSelectedPhaseMask(); });

    _spawn_editor = new QGroupBox("Selected NPC spawn", _properties_panel);
    auto* editor_layout = new QVBoxLayout(_spawn_editor);
    _spawn_identity = new QLabel(_spawn_editor);
    _spawn_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    editor_layout->addWidget(_spawn_identity);

    auto* editor_tabs = new QTabWidget(_spawn_editor);
    editor_tabs->setDocumentMode(true);
    editor_tabs->setStyleSheet("QTabBar::tab { min-width: 0px; padding: 4px 4px; }");
    editor_tabs->tabBar()->setExpanding(false);
    editor_tabs->tabBar()->setUsesScrollButtons(true);
    editor_tabs->tabBar()->setElideMode(Qt::ElideNone);
    auto* spawn_tab = new QWidget(editor_tabs);
    auto* spawn_tab_layout = new QVBoxLayout(spawn_tab);
    auto* movement_tab = new QWidget(editor_tabs);
    auto* movement_tab_layout = new QVBoxLayout(movement_tab);
    auto* appearance_tab = new QWidget(editor_tabs);
    auto* appearance_tab_layout = new QVBoxLayout(appearance_tab);
    auto* animation_tab = new QWidget(editor_tabs);
    auto* animation_tab_layout = new QVBoxLayout(animation_tab);
    editor_tabs->addTab(spawn_tab, "Spawn");
    editor_tabs->addTab(movement_tab, "Movement");
    editor_tabs->addTab(appearance_tab, "Appearance");
    editor_tabs->addTab(animation_tab, "Animation");
    editor_layout->addWidget(editor_tabs);

    auto* spawn_form = new QFormLayout();
    _edit_position_label = new QLabel(_spawn_editor);
    _edit_position_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _move_spawn_button = new QPushButton("Move NPC by clicking terrain", _spawn_editor);
    auto* rotate_spawn_button = new QPushButton("Rotate NPC with gizmo", _spawn_editor);
    rotate_spawn_button->setToolTip(
      "Select the placed NPC, then drag the rotation ring. Save spawn settings to keep its facing.");
    _edit_yaw = new QDoubleSpinBox(_spawn_editor);
    _edit_yaw->setRange(-180.0, 180.0);
    _edit_yaw->setDecimals(1);
    _edit_yaw->setSuffix(" degrees");
    _edit_respawn = new QSpinBox(_spawn_editor);
    _edit_respawn->setRange(1, 7 * 24 * 60 * 60);
    _edit_respawn->setSuffix(" seconds");
    _edit_wander = new QDoubleSpinBox(_spawn_editor);
    _edit_wander->setRange(0.0, 1000.0);
    _edit_wander->setDecimals(1);
    _edit_wander->setEnabled(false);
    _edit_movement = new QComboBox(_spawn_editor);
    _edit_movement->addItem("Stationary", 0);
    _edit_movement->addItem("Wander", 1);
    _edit_movement->addItem("Waypoints", 2);
    _edit_display = new QSpinBox(_spawn_editor);
    _edit_display->setRange(0, std::numeric_limits<int>::max());
    _edit_display->setSpecialValueText("Use template display");
    spawn_form->addRow("Position", _edit_position_label);
    spawn_form->addRow(_move_spawn_button);
    spawn_form->addRow(rotate_spawn_button);
    spawn_form->addRow("Facing", _edit_yaw);
    spawn_form->addRow("Respawn", _edit_respawn);
    spawn_tab_layout->addLayout(spawn_form);
    auto* save_spawn_button = new QPushButton("Save spawn settings", _spawn_editor);
    spawn_tab_layout->addWidget(save_spawn_button);
    auto* delete_spawn_button = new QPushButton("Delete NPC spawn...", _spawn_editor);
    delete_spawn_button->setToolTip(
      "Permanently delete this spawn and its unshared waypoint path from the world database. "
      "The NPC template is not deleted.");
    spawn_tab_layout->addWidget(delete_spawn_button);
    spawn_tab_layout->addStretch();

    auto* movement_form = new QFormLayout();
    movement_form->addRow("Movement type", _edit_movement);
    movement_form->addRow("Wander radius", _edit_wander);
    movement_tab_layout->addLayout(movement_form);
    auto* movement_help = new QLabel(
      "Stationary keeps the NPC at its spawn. Wander uses the radius above. "
      "Waypoint patrols are authored in the Waypoint Path section below.", movement_tab);
    movement_help->setWordWrap(true);
    movement_tab_layout->addWidget(movement_help);

    auto* speed_group = new QGroupBox("Patrol speed", movement_tab);
    auto* speed_form = new QFormLayout(speed_group);
    _edit_speed_walk = new QDoubleSpinBox(speed_group);
    _edit_speed_run = new QDoubleSpinBox(speed_group);
    for (QDoubleSpinBox* speed : {_edit_speed_walk, _edit_speed_run})
    {
      speed->setRange(0.05, 10.0);
      speed->setDecimals(5);
      speed->setSingleStep(0.05);
      speed->setSuffix(" x");
    }
    _edit_speed_walk->setValue(1.0);
    _edit_speed_run->setValue(1.14286);
    _edit_speed_walk->setToolTip(
      "Multiplier stored in creature_template.speed_walk. 1.0 is about 2.5 yards/second.");
    _edit_speed_run->setToolTip(
      "Multiplier stored in creature_template.speed_run. 1.0 is about 7 yards/second.");
    speed_form->addRow("Walk multiplier", _edit_speed_walk);
    speed_form->addRow("Run multiplier", _edit_speed_run);
    _speed_usage = new QLabel(speed_group);
    _speed_usage->setWordWrap(true);
    speed_form->addRow(_speed_usage);
    movement_tab_layout->addWidget(speed_group);

    auto* save_movement_button = new QPushButton("Save movement and speed", movement_tab);
    save_movement_button->setToolTip(
      "Save MovementType, wander radius, and the template walk/run speed multipliers, then verify them in the database.");
    movement_tab_layout->addWidget(save_movement_button);
    _preview_movement = new QCheckBox("Live patrol playback in Noggit", movement_tab);
    _preview_movement->setChecked(true);
    _preview_movement->setToolTip(
      "Waypoints and their arrival animations play in the viewport while you edit them. "
      "Uncheck this only when you want to pause the live patrol.");
    movement_tab_layout->addWidget(_preview_movement);

    _edit_emote = new QComboBox(_spawn_editor);
    _edit_emote->addItem("0 = None (normal idle)", 0);
    _edit_emote->setItemData(0, 0u, EmoteAnimationRole);
    _edit_emote->setItemData(0, 2u, EmoteProcedureRole);
    _edit_emote->setItemData(0, 0u, EmoteParameterRole);
    _edit_emote->setMaxVisibleItems(24);
    _edit_emote->setToolTip("Persistent server emote from Emotes.dbc. Continuous state emotes are best suited to creature spawns.");
    auto* save_emote_button = new QPushButton("Save server emote", _spawn_editor);
    auto* emote_form = new QFormLayout();
    emote_form->addRow("Server emote", _edit_emote);
    auto* server_emote_sound = new QLabel("Event sound: none", animation_tab);
    server_emote_sound->setWordWrap(true);
    emote_form->addRow("Event sound", server_emote_sound);
    emote_form->addRow(save_emote_button);
    animation_tab_layout->addLayout(emote_form);

    auto* preview_group = new QGroupBox("Noggit animation preview", animation_tab);
    auto* preview_form = new QFormLayout(preview_group);
    _preview_animation = new QComboBox(preview_group);
    _preview_animation->setEditable(true);
    _preview_animation->setInsertPolicy(QComboBox::NoInsert);
    _preview_animation->addItem("Automatic (idle / walk / run)", 0);
    _preview_animation->setToolTip("Choose a client animation included in this M2. Automatic uses idle while stopped and walk/run while moving.");
    preview_form->addRow("Animation override", _preview_animation);
    animation_tab_layout->addWidget(preview_group);
    animation_tab_layout->addStretch();

    auto* display_form = new QFormLayout();
    display_form->addRow("Spawn display override", _edit_display);
    appearance_tab_layout->addLayout(display_form);
    auto* weapon_preview = new QGroupBox("Weapon visibility in Noggit", appearance_tab);
    auto* weapon_preview_layout = new QVBoxLayout(weapon_preview);
    _show_main_off_hand = new QCheckBox("Show main-hand and off-hand", weapon_preview);
    _show_ranged_weapon = new QCheckBox("Show ranged weapon", weapon_preview);
    weapon_preview_layout->addWidget(_show_main_off_hand);
    weapon_preview_layout->addWidget(_show_ranged_weapon);
    auto* weapon_preview_note = new QLabel(
      "These switches affect only Noggit's viewport. Both weapon sets remain in the "
      "NPC's equipment data; showing both can overlap on some models.", weapon_preview);
    weapon_preview_note->setWordWrap(true);
    weapon_preview_layout->addWidget(weapon_preview_note);
    appearance_tab_layout->addWidget(weapon_preview);
    auto* in_game_weapons = new QGroupBox("In-game drawn weapon", appearance_tab);
    auto* in_game_form = new QFormLayout(in_game_weapons);
    _edit_weapon_stance = new QComboBox(in_game_weapons);
    _edit_weapon_stance->addItem("Keep current / server default", -1);
    _edit_weapon_stance->addItem("Melee (main-hand / off-hand)", 1);
    _edit_weapon_stance->addItem("Ranged (bow / gun / crossbow)", 2);
    in_game_form->addRow("Drawn weapon", _edit_weapon_stance);
    auto* save_weapon_stance = new QPushButton("Save in-game weapon stance", in_game_weapons);
    in_game_form->addRow(save_weapon_stance);
    auto* in_game_note = new QLabel(
      "Saves this spawn's drawn weapon to the world database. The NPC keeps all equipped "
      "weapons; combat AI may change its stance. Reload the world server to see it in game.",
      in_game_weapons);
    in_game_note->setWordWrap(true);
    in_game_form->addRow(in_game_note);
    appearance_tab_layout->addWidget(in_game_weapons);
    auto* template_group = new QGroupBox("Unique template for this NPC", appearance_tab);
    auto* template_form = new QFormLayout(template_group);
    _edit_name = new QLineEdit(template_group);
    _edit_subname = new QLineEdit(template_group);
    _edit_minlevel = new QSpinBox(template_group);
    _edit_maxlevel = new QSpinBox(template_group);
    _edit_faction = new NpcFactionSelector(template_group);
    _edit_template_display = new QSpinBox(template_group);
    for (QSpinBox* field : {_edit_minlevel, _edit_maxlevel}) field->setRange(1, 255);
    _edit_template_display->setRange(0, std::numeric_limits<int>::max());
    template_form->addRow("Name", _edit_name);
    template_form->addRow("Subname", _edit_subname);
    template_form->addRow("Min level", _edit_minlevel);
    template_form->addRow("Max level", _edit_maxlevel);
    template_form->addRow("Faction template", _edit_faction);
    template_form->addRow("Display ID", _edit_template_display);
    auto* save_template_button = new QPushButton("Create unique template and apply", template_group);
    template_form->addRow(save_template_button);
    auto* open_npc_editor_button = new QPushButton("Open full NPC editor...", template_group);
    open_npc_editor_button->setToolTip(
      "Edit identity, appearance display, and main-hand, off-hand, and ranged equipment.");
    template_form->addRow(open_npc_editor_button);
    appearance_tab_layout->addWidget(template_group);
    appearance_tab_layout->addStretch();

    _waypoint_panel = new QGroupBox("Waypoint Path", movement_tab);
    auto* waypoint_layout = new QVBoxLayout(_waypoint_panel);
    auto* waypoint_help = new QLabel(
      "Add, order, and tune this NPC's patrol points without leaving the Movement tab. "
      "Use Shift to select a range or Ctrl to toggle individual points.",
      _waypoint_panel);
    waypoint_help->setWordWrap(true);
    waypoint_layout->addWidget(waypoint_help);
    _waypoint_list = new QListWidget(_waypoint_panel);
    _waypoint_list->setMinimumHeight(120);
    _waypoint_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    waypoint_layout->addWidget(_waypoint_list);
    auto* waypoint_form = new QFormLayout();
    _waypoint_behavior = new QComboBox(_waypoint_panel);
    _waypoint_behavior->addItem("Retrace (back and forth)", 1);
    _waypoint_behavior->addItem("Loop (last to first)", 0);
    _waypoint_behavior->setToolTip(
      "Retrace automatically follows the authored points in reverse after reaching the end. "
      "Loop connects the final point directly to the first point.");
    _waypoint_delay = new QSpinBox(_waypoint_panel);
    _waypoint_delay->setRange(0, 600000);
    _waypoint_delay->setSuffix(" ms");
    _waypoint_emote = new QComboBox(_waypoint_panel);
    _waypoint_emote->addItem("0 = None (normal idle)", 0);
    _waypoint_emote->setItemData(0, 0u, EmoteAnimationRole);
    _waypoint_emote->setItemData(0, 2u, EmoteProcedureRole);
    _waypoint_emote->setItemData(0, 0u, EmoteParameterRole);
    _waypoint_emote->setMaxVisibleItems(24);
    _waypoint_emote->setToolTip(
      "Animation or emote played after reaching this waypoint. It remains active during the selected point delay.");
    _waypoint_run = new QCheckBox("Run to this point", _waypoint_panel);
    _conform_waypoints = new QCheckBox("Conform Noggit preview to terrain / WMO", _waypoint_panel);
    _conform_waypoints->setChecked(true);
    _conform_waypoints->setToolTip(
      "Samples the edited terrain between authored waypoints for smooth live playback in "
      "Noggit. Preview support points are never added to the saved server path.");
    waypoint_form->addRow("Path behavior", _waypoint_behavior);
    waypoint_form->addRow("Selected point delay", _waypoint_delay);
    waypoint_form->addRow("Arrival animation", _waypoint_emote);
    auto* waypoint_emote_sound = new QLabel("Event sound: none", _waypoint_panel);
    waypoint_emote_sound->setWordWrap(true);
    waypoint_form->addRow("Event sound", waypoint_emote_sound);
    waypoint_form->addRow(_waypoint_run);
    waypoint_form->addRow(_conform_waypoints);
    waypoint_layout->addLayout(waypoint_form);
    _capture_waypoint_button = new QPushButton("Add points by clicking terrain", _waypoint_panel);
    waypoint_layout->addWidget(_capture_waypoint_button);
    auto* waypoint_buttons = new QHBoxLayout();
    auto* remove_waypoint = new QPushButton("Remove selected", _waypoint_panel);
    auto* move_waypoint_up = new QPushButton("Up", _waypoint_panel);
    auto* move_waypoint_down = new QPushButton("Down", _waypoint_panel);
    auto* save_waypoints_button = new QPushButton("Save path", _waypoint_panel);
    waypoint_buttons->addWidget(remove_waypoint);
    waypoint_buttons->addWidget(move_waypoint_up);
    waypoint_buttons->addWidget(move_waypoint_down);
    waypoint_buttons->addWidget(save_waypoints_button);
    waypoint_layout->addLayout(waypoint_buttons);
    auto const update_waypoint_buttons =
      [this, remove_waypoint, move_waypoint_up, move_waypoint_down]
    {
      QModelIndexList const selected = _waypoint_list->selectionModel()->selectedRows();
      int const row = _waypoint_list->currentRow();
      bool const one_selected = selected.size() == 1;
      remove_waypoint->setEnabled(!selected.isEmpty());
      move_waypoint_up->setEnabled(one_selected && row > 0);
      move_waypoint_down->setEnabled(one_selected && row >= 0
        && row + 1 < static_cast<int>(_waypoints.size()));
    };
    connect(_waypoint_list, &QListWidget::itemSelectionChanged,
            this, update_waypoint_buttons);
    update_waypoint_buttons();
    movement_tab_layout->addWidget(_waypoint_panel, 1);
    _waypoint_panel->setEnabled(false);

    _spawn_editor->hide();
    details_layout->addWidget(_spawn_editor);
    layout->addWidget(_table, 1);
    auto* page_controls = new QHBoxLayout();
    _previous_page = new QPushButton("Previous", this);
    _next_page = new QPushButton("Next", this);
    _page_status = new QLabel("Page 1 of 1", this);
    _page_status->setAlignment(Qt::AlignCenter);
    page_controls->addWidget(_previous_page);
    page_controls->addWidget(_page_status, 1);
    page_controls->addWidget(_next_page);
    layout->addLayout(page_controls);

    // This one-pixel shared viewer supplies appearance data and renders the
    // catalogue thumbnails offscreen. It is deliberately not a visible
    // properties-panel preview.
    _preview->setFixedSize(1, 1);
    layout->addWidget(_preview, 0, Qt::AlignRight);

    _status = new QLabel("Connect to a world database to load NPC templates.", this);
    _status->setWordWrap(true);
    layout->addWidget(_status);

    connect(_search, &QLineEdit::textChanged, this, [this] { updateFilters(); });
    connect(_faction, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateFilters(); });
    connect(_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateFilters(); });
    connect(_role, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateFilters(); });
    connect(_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this] { updateDetails(); });
    connect(_table, &QListView::clicked, this, [this](QModelIndex const&)
    {
      if (!_placement_entry && !_selected_model_path.empty()) startPlacement();
    });
    connect(_previous_page, &QPushButton::clicked, this, [this]
    {
      _page_filter->setPage(_page_filter->page() - 1);
      updateBrowserPage();
    });
    connect(_next_page, &QPushButton::clicked, this, [this]
    {
      _page_filter->setPage(_page_filter->page() + 1);
      updateBrowserPage();
    });
    connect(_cancel_placement, &QPushButton::clicked, this,
            [this] { cancelPlacement("NPC placement stopped. Click an NPC row to start again."); });
    connect(save_spawn_button, &QPushButton::clicked, this, [this] { saveSelectedSpawn(); });
    connect(save_movement_button, &QPushButton::clicked, this,
            [this] { saveMovementSettings(); });
    connect(delete_spawn_button, &QPushButton::clicked, this, [this] { deleteSelectedSpawn(); });
    connect(save_emote_button, &QPushButton::clicked, this, [this] { saveServerEmote(); });
    connect(_move_spawn_button, &QPushButton::clicked, this, [this]
    {
      _moving_spawn = !_moving_spawn;
      _move_spawn_button->setText(_moving_spawn
        ? "Stop moving NPC" : "Move NPC by clicking terrain");
    });
    connect(rotate_spawn_button, &QPushButton::clicked, this, [this]
    {
      if (!_selected_spawn_guid || !_map_view || !_map_view->getWorld()
          || !_map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
        return;
      if (_placement_entry) cancelPlacement();
      _map_view->getWorld()->selectNpcSpawnOverlay(*_selected_spawn_guid);
      _map_view->activateNpcRotationGizmo();
      _status->setText("Drag the NPC rotation ring, or hold R and move the mouse. "
                       "Save spawn settings to keep its new facing.");
    });
    connect(_edit_yaw, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value)
    {
      if (!_selected_spawn_guid || !_map_view) return;
      auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid);
      if (!overlay) return;
      _map_view->getWorld()->updateNpcSpawnOverlay(*_selected_spawn_guid,
        _edit_spawn_position, static_cast<float>(value), overlay->scale());
      updateMovementPreview();
    });
    connect(save_template_button, &QPushButton::clicked, this, [this] { saveUniqueTemplate(); });
    connect(open_npc_editor_button, &QPushButton::clicked, this,
            [this] { openNpcEditor(true); });
    connect(save_waypoints_button, &QPushButton::clicked, this, [this] { saveWaypoints(); });
    connect(_capture_waypoint_button, &QPushButton::clicked, this, [this]
    {
      _capturing_waypoints = !_capturing_waypoints;
      if (_capturing_waypoints)
      {
        _edit_movement->setCurrentIndex(std::max(0, _edit_movement->findData(2)));
        if (_waypoints.empty())
        {
          _waypoints.push_back({_edit_spawn_position, 0, false});
          updateWaypointList();
          _waypoint_list->setCurrentRow(0);
          _status->setText("Waypoint 1 was placed at the NPC's current position. "
                           "Click terrain or a WMO to add the next point.");
        }
        if (!_preview_movement->isChecked())
          _preview_movement->setChecked(true);
        updateMovementPreview();
      }
      else
        updateMovementPreview();
      _capture_waypoint_button->setText(_capturing_waypoints
        ? "Stop adding points" : "Add points by clicking terrain");
    });
    connect(remove_waypoint, &QPushButton::clicked, this,
            [this, update_waypoint_buttons]
    {
      QModelIndexList const selected = _waypoint_list->selectionModel()->selectedRows();
      std::vector<int> rows;
      rows.reserve(selected.size());
      for (QModelIndex const& index : selected)
        if (index.row() >= 0 && index.row() < static_cast<int>(_waypoints.size()))
          rows.push_back(index.row());
      if (rows.empty()) return;

      std::sort(rows.begin(), rows.end(), std::greater<int>());
      int const next_row = rows.back();
      for (int const row : rows)
        _waypoints.erase(_waypoints.begin() + row);
      updateWaypointList();
      if (!_waypoints.empty())
        _waypoint_list->setCurrentRow(
          std::min(next_row, static_cast<int>(_waypoints.size()) - 1));
      update_waypoint_buttons();
      updateMovementPreview();
    });
    connect(move_waypoint_up, &QPushButton::clicked, this, [this]
    {
      int const row = _waypoint_list->currentRow();
      if (row <= 0 || row >= static_cast<int>(_waypoints.size())) return;
      std::swap(_waypoints[row], _waypoints[row - 1]);
      updateWaypointList();
      _waypoint_list->setCurrentRow(row - 1);
      updateMovementPreview();
    });
    connect(move_waypoint_down, &QPushButton::clicked, this, [this]
    {
      int const row = _waypoint_list->currentRow();
      if (row < 0 || row + 1 >= static_cast<int>(_waypoints.size())) return;
      std::swap(_waypoints[row], _waypoints[row + 1]);
      updateWaypointList();
      _waypoint_list->setCurrentRow(row + 1);
      updateMovementPreview();
    });
    connect(_waypoint_list, &QListWidget::currentRowChanged, this, [this](int row)
    {
      if (row < 0 || row >= static_cast<int>(_waypoints.size())) return;
      QSignalBlocker const delay_block(_waypoint_delay);
      QSignalBlocker const run_block(_waypoint_run);
      QSignalBlocker const emote_block(_waypoint_emote);
      _waypoint_delay->setValue(_waypoints[row].delay_ms);
      _waypoint_run->setChecked(_waypoints[row].run);
      int emote_index = _waypoint_emote->findData(_waypoints[row].emote_id);
      if (emote_index < 0)
      {
        _waypoint_emote->addItem(
          QString("%1 = Unknown/custom emote").arg(_waypoints[row].emote_id),
          _waypoints[row].emote_id);
        emote_index = _waypoint_emote->count() - 1;
      }
      _waypoint_emote->setCurrentIndex(emote_index);
    });
    connect(_waypoint_delay, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value)
    {
      int const row = _waypoint_list->currentRow();
      if (row < 0 || row >= static_cast<int>(_waypoints.size())) return;
      _waypoints[row].delay_ms = value;
      _waypoint_list->item(row)->setText(waypointListText(static_cast<std::size_t>(row)));
      updateMovementPreview();
    });
    connect(_waypoint_run, &QCheckBox::toggled, this, [this](bool run)
    {
      int const row = _waypoint_list->currentRow();
      if (row < 0 || row >= static_cast<int>(_waypoints.size())) return;
      _waypoints[row].run = run;
      _waypoint_list->item(row)->setText(waypointListText(static_cast<std::size_t>(row)));
      updateMovementPreview();
    });
    connect(_waypoint_emote, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index)
    {
      int const row = _waypoint_list->currentRow();
      if (row < 0 || row >= static_cast<int>(_waypoints.size()) || index < 0) return;
      _waypoints[row].emote_id = _waypoint_emote->itemData(index).toUInt();
      _waypoint_list->item(row)->setText(waypointListText(static_cast<std::size_t>(row)));
      updateMovementPreview();
    });
    connect(_conform_waypoints, &QCheckBox::toggled,
            this, [this] { updateMovementPreview(); });
    connect(_waypoint_behavior, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { updateMovementPreview(); });
    connect(_preview_movement, &QCheckBox::toggled, this, [this] { updateMovementPreview(); });
    connect(_show_main_off_hand, &QCheckBox::toggled,
            this, [this] { saveSelectedWeaponVisibility(); });
    connect(_show_ranged_weapon, &QCheckBox::toggled,
            this, [this] { saveSelectedWeaponVisibility(); });
    connect(_edit_weapon_stance, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { previewSelectedWeaponStance(); });
    connect(save_weapon_stance, &QPushButton::clicked,
            this, [this] { saveSelectedWeaponStance(); });
    connect(_preview_animation, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { updateMovementPreview(); });
    connect(_preview_animation->lineEdit(), &QLineEdit::textEdited,
            this, [this] { updateMovementPreview(); });
    connect(_edit_emote, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { updateMovementPreview(); });
    connect(_edit_movement, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]
    {
      _edit_wander->setEnabled(_edit_movement->currentData().toInt() == 1);
      updateMovementPreview();
    });
    connect(_edit_wander, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { updateMovementPreview(); });
    connect(_edit_speed_walk, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { updateMovementPreview(); });
    connect(_edit_speed_run, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { updateMovementPreview(); });
    connect(_respawn_seconds, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { updatePlacementSummary(); });

    // Register the resulting editor state after the existing handlers have
    // updated the draft/preview. Programmatic loads and undo restores are
    // guarded in recordNpcEdit.
    auto track_spin = [this](QSpinBox* widget, unsigned domain)
    {
      connect(widget, QOverload<int>::of(&QSpinBox::valueChanged), this,
              [this, domain] { recordNpcEdit(domain); });
    };
    auto track_double = [this](QDoubleSpinBox* widget, unsigned domain)
    {
      connect(widget, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
              [this, domain] { recordNpcEdit(domain); });
    };
    auto track_combo = [this](QComboBox* widget, unsigned domain)
    {
      connect(widget, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
              [this, domain] { recordNpcEdit(domain); });
    };
    auto track_text = [this](QLineEdit* widget, unsigned domain)
    {
      connect(widget, &QLineEdit::textEdited, this,
              [this, domain] { recordNpcEdit(domain); });
    };
    track_double(_edit_yaw, EditTransform);
    track_double(_edit_speed_walk, EditTemplate);
    track_double(_edit_speed_run, EditTemplate);
    track_spin(_edit_respawn, EditSpawn);
    track_double(_edit_wander, EditSpawn);
    track_combo(_edit_movement, EditSpawn);
    track_spin(_edit_display, EditSpawn);
    track_spin(_waypoint_delay, EditPath);
    connect(_waypoint_run, &QCheckBox::toggled, this, [this] { recordNpcEdit(EditPath); });
    track_combo(_waypoint_emote, EditPath);
    track_combo(_waypoint_behavior, EditPath);
    connect(_conform_waypoints, &QCheckBox::toggled, this, [this] { recordNpcEdit(EditPath); });
    connect(_capture_waypoint_button, &QPushButton::clicked, this,
            [this] { recordNpcEdit(EditPath); });
    for (QPushButton* button : {remove_waypoint, move_waypoint_up, move_waypoint_down})
      connect(button, &QPushButton::clicked, this, [this] { recordNpcEdit(EditPath); });
    track_combo(_edit_emote, EditEmote);
    track_text(_edit_name, EditTemplate);
    track_text(_edit_subname, EditTemplate);
    track_spin(_edit_minlevel, EditTemplate);
    track_spin(_edit_maxlevel, EditTemplate);
    track_combo(_edit_faction, EditTemplate);
    connect(_edit_faction->lineEdit(), &QLineEdit::textEdited, this,
            [this] { recordNpcEdit(EditTemplate); });
    track_spin(_edit_template_display, EditTemplate);
    connect(_preview_movement, &QCheckBox::toggled, this,
            [this] { recordNpcEdit(EditPreview); });
    track_combo(_preview_animation, EditPreview);
    connect(_preview_animation->lineEdit(), &QLineEdit::textEdited, this,
            [this] { recordNpcEdit(EditPreview); });

    if (_map_view)
    {
      _map_view->installEventFilter(this);
      connect(_map_view, &MapView::npcSpawnTransformed, this,
              [this](qulonglong guid, float x, float y, float z, float yaw)
      {
        if (!_selected_spawn_guid || *_selected_spawn_guid != guid)
          return;
        auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(
          *_selected_spawn_guid);
        if (!overlay)
          return;

        // A movement preview writes its simulated route position into the
        // rendered body every frame. Stop that playback when the user takes
        // direct control with the gizmo, otherwise releasing the gizmo makes
        // the body appear to jump back to its old route/start position.
        overlay->setPreviewRoute({}, false);

        _edit_spawn_position = {x, y, z};
        _pending_transform_edit = true;
        _edit_position_label->setText(QString("%1, %2, %3")
          .arg(x, 0, 'f', 2).arg(y, 0, 'f', 2).arg(z, 0, 'f', 2));
        {
          QSignalBlocker const blocker(_edit_yaw);
          _edit_yaw->setValue(normalizedYaw(yaw));
        }
        _status->setText("NPC transformed in Noggit. Save spawn settings to update the database; "
                         "the live patrol resumes when the path is edited or the NPC is reselected.");
      });
      connect(_map_view, &QObject::destroyed, this, [this]
      {
        _map_view = nullptr;
        _placement_overlay_guid.reset();
      });
    }

    try
    {
      _display_info = std::make_unique<DBCFile>("DBFilesClient\\CreatureDisplayInfo.dbc");
      _model_data = std::make_unique<DBCFile>("DBFilesClient\\CreatureModelData.dbc");
      _display_info->open(_client_data);
      _model_data->open(_client_data);
    }
    catch (std::exception const&)
    {
      _display_info.reset();
      _model_data.reset();
      _preview_note->setText("Creature model data is unavailable in the configured client files.");
    }
    try
    {
      _display_extra = std::make_unique<DBCFile>("DBFilesClient\\CreatureDisplayInfoExtra.dbc");
      _display_extra->open(_client_data);
    }
    catch (std::exception const&)
    {
      _display_extra.reset();
    }
    try
    {
      _char_sections = std::make_unique<DBCFile>("DBFilesClient\\CharSections.dbc");
      _char_sections->open(_client_data);
      if (_char_sections->getFieldCount() < 10) _char_sections.reset();
    }
    catch (std::exception const&)
    {
      _char_sections.reset();
    }
    auto open_optional = [this](char const* path) -> std::unique_ptr<DBCFile>
    {
      try
      {
        auto file = std::make_unique<DBCFile>(path);
        file->open(_client_data);
        return file;
      }
      catch (std::exception const&) { return {}; }
    };
    _hair_geosets = open_optional("DBFilesClient\\CharHairGeosets.dbc");
    _facial_hair_styles = open_optional("DBFilesClient\\CharacterFacialHairStyles.dbc");
    _item_display_info = open_optional("DBFilesClient\\ItemDisplayInfo.dbc");
    _gameobject_display_info = open_optional("DBFilesClient\\GameObjectDisplayInfo.dbc");
    _item_data = open_optional("DBFilesClient\\Item.dbc");
    _helmet_geoset_vis = open_optional("DBFilesClient\\HelmetGeosetVisData.dbc");
    _chr_races = open_optional("DBFilesClient\\ChrRaces.dbc");
    _npc_sounds = open_optional("DBFilesClient\\NPCSounds.dbc");
    if (_npc_sounds && _npc_sounds->getFieldCount() != 5) _npc_sounds.reset();
    _creature_sound_data = open_optional("DBFilesClient\\CreatureSoundData.dbc");
    if (_creature_sound_data && _creature_sound_data->getFieldCount() < 38)
      _creature_sound_data.reset();
    _footstep_terrain_lookup = open_optional("DBFilesClient\\FootstepTerrainLookup.dbc");
    if (_footstep_terrain_lookup && _footstep_terrain_lookup->getFieldCount() < 5)
      _footstep_terrain_lookup.reset();
    _animation_data = open_optional("DBFilesClient\\AnimationData.dbc");
    if (_animation_data && _animation_data->getFieldCount() >= 2)
    {
      for (std::size_t row = 0; row < _animation_data->getRecordCount(); ++row)
      {
        auto const animation = _animation_data->getRecord(row);
        unsigned const id = animation.getUInt(0);
        if (!id) continue;
        QString name = QString::fromStdString(animation.getString(1));
        if (name.isEmpty()) name = QString("Animation %1").arg(id);
        _preview_animation->addItem(QString("%1 — %2").arg(id).arg(name), id);
      }
    }
    _emote_data = open_optional("DBFilesClient\\Emotes.dbc");
    if (_emote_data && _emote_data->getFieldCount() >= 7)
    {
      for (std::size_t row = 0; row < _emote_data->getRecordCount(); ++row)
      {
        auto const emote = _emote_data->getRecord(row);
        unsigned const id = emote.getUInt(0);
        if (!id) continue;
        QString name = QString::fromStdString(emote.getString(1));
        if (name.isEmpty()) name = QString("Emote %1").arg(id);
        if (name.startsWith("EMOTE_ONESHOT_")) name.remove(0, 14);
        else if (name.startsWith("EMOTE_STATE_")) name.remove(0, 12);
        else if (name.startsWith("EMOTE_")) name.remove(0, 6);
        name.replace('_', ' ');
        name = name.toLower();
        if (!name.isEmpty()) name[0] = name[0].toUpper();
        unsigned const procedure = emote.getUInt(4);
        QString const kind = procedure == 2 ? "continuous state"
          : procedure == 1 ? "stand state" : "one-shot";
        QString const label = QString("%1 = %2 (%3)").arg(id).arg(name).arg(kind);
        for (QComboBox* combo : {_edit_emote, _waypoint_emote})
        {
          combo->addItem(label, id);
          int const index = combo->count() - 1;
          combo->setItemData(index, emote.getUInt(2), EmoteAnimationRole);
          combo->setItemData(index, procedure, EmoteProcedureRole);
          combo->setItemData(index, emote.getUInt(5), EmoteParameterRole);
        }
      }
      int const none = _edit_emote->findData(0);
      if (none >= 0) _edit_emote->setCurrentIndex(none);
      int const waypoint_none = _waypoint_emote->findData(0);
      if (waypoint_none >= 0) _waypoint_emote->setCurrentIndex(waypoint_none);
    }
    auto update_emote_sound = [this](QComboBox* combo, QLabel* label)
    {
      unsigned const emote_id = combo->currentData().toUInt();
      unsigned sound_id = 0;
      if (_emote_data && emote_id)
      {
        try { sound_id = _emote_data->getByID(emote_id).getUInt(6); }
        catch (std::exception const&) {}
      }
      if (!sound_id) { label->setText("None"); return; }
      QString name = "Missing SoundEntries row";
      if (gSoundEntriesDB.CheckIfIdExists(sound_id))
        name = QString::fromUtf8(gSoundEntriesDB.getByID(sound_id)
                                   .getString(SoundEntriesDB::Name));
      label->setText(QString("%1 — %2").arg(sound_id).arg(name));
    };
    connect(_edit_emote, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [=](int) { update_emote_sound(_edit_emote, server_emote_sound); });
    connect(_waypoint_emote, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [=](int) { update_emote_sound(_waypoint_emote, waypoint_emote_sound); });
    update_emote_sound(_edit_emote, server_emote_sound);
    update_emote_sound(_waypoint_emote, waypoint_emote_sound);
    _spawn_visibility_timer = new QTimer(this);
    _spawn_visibility_timer->setInterval(250);
    connect(_spawn_visibility_timer, &QTimer::timeout, this,
            [this] { updateVisibleCachedSpawns(); });
    _spawn_visibility_timer->start();
  }

  NpcTemplateBrowser::~NpcTemplateBrowser()
  {
    // History callbacks target this editor. Remove its unsaved entries when
    // the editor itself is destroyed, without touching terrain/object edits.
    for (auto const& draft : _draft_snapshots)
      NOGGIT_ACTION_MGR->discardNpcEdits(draft.first, EditAll);
    if (_selected_spawn_guid)
      NOGGIT_ACTION_MGR->discardNpcEdits(*_selected_spawn_guid, EditAll);
    // The properties and waypoint widgets live in sibling dock windows and
    // may already be gone while the main window is tearing down. Remove the
    // viewport-only placement overlay without touching those widgets.
    if (_placement_overlay_guid && _map_view && _map_view->getWorld())
      _map_view->getWorld()->removeNpcSpawnOverlay(*_placement_overlay_guid);
    _placement_overlay_guid.reset();
    _placement_appearance.reset();
    _placement_entry = 0;
    _placement_equipment_id = 0;
    _placement_weapon_stance = -1;
    if (_selected_spawn_guid && _map_view && _map_view->getWorld())
    {
      if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
      {
        overlay->setPreviewRoute({}, false);
        overlay->setTransform(_saved_spawn_position, _saved_spawn_yaw, overlay->scale());
      }
      _map_view->getWorld()->selectNpcSpawnOverlay({});
    }
  }

  void NpcTemplateBrowser::populateEventMenu()
  {
    if (!_event_menu || !_event_selector) return;
    _event_menu->clear();
    auto* none = _event_menu->addAction("No events (normal world)");
    none->setCheckable(true);
    none->setChecked(_active_event_ids.empty());
    connect(none, &QAction::triggered, this, [this, none]
    {
      _active_event_ids.clear();
      for (QAction* action : _event_menu->actions())
      {
        QSignalBlocker const blocker(action);
        action->setChecked(action == none);
      }
      updateEventPreview();
    });
    _event_menu->addSeparator();
    for (auto const& [id, description] : _event_descriptions)
    {
      auto* action = _event_menu->addAction(
        QString("%1 — %2").arg(id).arg(description));
      action->setCheckable(true);
      action->setChecked(_active_event_ids.contains(id));
      connect(action, &QAction::toggled, this, [this, none, id](bool checked)
      {
        if (checked) _active_event_ids.insert(id);
        else _active_event_ids.erase(id);
        QSignalBlocker const blocker(none);
        none->setChecked(_active_event_ids.empty());
        updateEventPreview();
      });
    }
    _event_selector->setEnabled(_spawn_cache_loaded);
    updateEventPreview();
  }

  void NpcTemplateBrowser::updateEventPreview()
  {
    if (_event_selector)
    {
      if (_active_event_ids.empty())
        _event_selector->setText("Active events: None");
      else if (_active_event_ids.size() == 1)
      {
        int const id = *_active_event_ids.begin();
        auto const found = _event_descriptions.find(id);
        _event_selector->setText(QString("Active event: %1 — %2")
          .arg(id).arg(found == _event_descriptions.end() ? QString("Unknown")
                                                     : found->second));
      }
      else
        _event_selector->setText(
          QString("Active events: %1 selected")
            .arg(static_cast<qulonglong>(_active_event_ids.size())));
    }
    reconcileSelectedSpawnVisibility();
    updateVisibleCachedSpawns();
  }

  void NpcTemplateBrowser::loadTemplates()
  {
    if (!QSqlDatabase::isDriverAvailable("QMYSQL"))
    {
      _status->setText("Qt MySQL driver is unavailable. Check Noggit Azure's sqldrivers folder.");
      return;
    }

    QString const host = _host->text().trimmed();
    QString const database = _database->text().trimmed();
    QString const user = _user->text().trimmed();
    if (host.isEmpty() || database.isEmpty() || user.isEmpty())
    {
      _status->setText("Enter a server, world database, and user.");
      return;
    }

    _status->setText("Loading NPC templates...");
    std::vector<NpcTemplate> entries;
    QString error;
    QString const connection_name = QString("npc_template_browser_%1").arg(reinterpret_cast<quintptr>(this));
    {
      QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", connection_name);
      db.setHostName(host);
      db.setPort(_port->value());
      db.setDatabaseName(database);
      db.setUserName(user);
      db.setPassword(_password->text());
      db.setConnectOptions("MYSQL_OPT_CONNECT_TIMEOUT=5");
      if (!db.open())
      {
        error = db.lastError().text();
      }
      else
      {
        Sql::WorldDatabaseSchema schema;
        if (!Sql::inspectWorldDatabaseSchema(db, schema, error))
        {
          db.close();
        }
        else
        {
          QSqlQuery query(db);
          query.setForwardOnly(true);
          QString const sql = QString(
            "SELECT ct.`entry`, ct.`name`, ct.`subname`, %1 AS `display_id`, "
            "ct.`faction`, ct.`npcflag`, ct.`type`, ct.`family`, ct.`minlevel`, "
            "ct.`maxlevel`, ct.`rank`, ct.`AIName`, ct.`ScriptName` "
            "FROM `creature_template` ct ORDER BY ct.`entry`")
              .arg(schema.templateDisplay("ct"));
          if (!query.exec(sql))
          {
            error = query.lastError().text();
          }
          else
          {
            while (query.next())
            {
              NpcTemplate npc;
              npc.entry = query.value(0).toUInt();
              npc.name = query.value(1).toString();
              npc.subname = query.value(2).toString();
              npc.model = query.value(3).toUInt();
              npc.faction = query.value(4).toUInt();
              npc.flags = query.value(5).toUInt();
              npc.type = query.value(6).toInt();
              npc.family = query.value(7).toInt();
              npc.min_level = query.value(8).toInt();
              npc.max_level = query.value(9).toInt();
              npc.rank = query.value(10).toInt();
              npc.ai_name = query.value(11).toString();
              npc.script_name = query.value(12).toString();
              entries.push_back(std::move(npc));
            }
          }
        }
      }
      db.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    if (!error.isEmpty())
    {
      _status->setText("Could not load creature_template: " + error);
      return;
    }

    QSettings settings;
    settings.setValue("npc_browser/host", host);
    settings.setValue("npc_browser/port", _port->value());
    settings.setValue("npc_browser/database", database);
    settings.setValue("npc_browser/user", user);
    settings.setValue("npc_browser/client_data_folder", _client_data_folder->text().trimmed());
    settings.setValue("npc_browser/server_data_folder", _server_data_folder->text().trimmed());

    _model->replace(std::move(entries));
    _connection_toggle->setChecked(false);
    std::vector<unsigned> factions;
    std::vector<int> types;
    for (auto const& npc : _model->entries())
    {
      factions.push_back(npc.faction);
      types.push_back(npc.type);
    }
    std::sort(factions.begin(), factions.end());
    factions.erase(std::unique(factions.begin(), factions.end()), factions.end());
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    _faction->clear();
    _faction->addItem("All factions", -1);
    for (unsigned id : factions) _faction->addItem(factionTemplateLabel(id), id);
    _type->clear();
    _type->addItem("All types", -1);
    for (int id : types) _type->addItem(typeName(id), id);
    updateFilters();
    QTimer::singleShot(0, this, [this] { loadNearbySpawns(); });
  }

  void NpcTemplateBrowser::updateFilters()
  {
    _filter->setCriteria(_search->text().trimmed(), _faction->currentData().toInt(),
                         _type->currentData().toInt(), _role->currentData().toInt());
    _page_filter->resetPage();
    updateDetails();
    updateBrowserPage();
  }

  void NpcTemplateBrowser::updateBrowserPage()
  {
    int const pages = _page_filter->pageCount();
    int const page = std::clamp(_page_filter->page(), 0, pages - 1);
    if (page != _page_filter->page())
      _page_filter->setPage(page);
    _previous_page->setEnabled(page > 0);
    _next_page->setEnabled(page + 1 < pages);
    _page_status->setText(QString("Page %1 of %2 · %3 NPCs")
                            .arg(page + 1).arg(pages).arg(_filter->rowCount()));
    _table->clearSelection();
    _table->setCurrentIndex({});
    if (_delete_npc_template) _delete_npc_template->setEnabled(false);
    _status->setText(QString("Showing page %1 of %2 (%3 of %4 NPC templates).")
                       .arg(page + 1).arg(pages)
                       .arg(_filter->rowCount()).arg(_model->rowCount()));
    renderVisibleThumbnails();
  }

  void NpcTemplateBrowser::renderVisibleThumbnails()
  {
    int rendered_count = 0;
    int failed_count = 0;
    QString first_failure;
    for (int row = 0; row < _page_filter->rowCount(); ++row)
    {
      QModelIndex const page_index = _page_filter->index(row, 0);
      QModelIndex const filtered_index = _page_filter->mapToSource(page_index);
      QModelIndex const source_index = _filter->mapToSource(filtered_index);
      if (!source_index.isValid()) continue;
      if (!source_index.data(Qt::DecorationRole).value<QIcon>().isNull()) continue;
      auto const* npc = _model->entry(source_index.row());
      if (!npc || !npc->model) continue;
      try
      {
        QString display_failure;
        if (!loadDisplay(npc->model, false, &display_failure))
          throw std::runtime_error(display_failure.toStdString());
        auto const appearance = _preview->creatureAppearance();
        if (!appearance)
          throw std::runtime_error("the display did not produce a creature appearance");
        QPixmap const thumbnail = _thumbnail_renderer->renderAppearance(
          *appearance, std::to_string(npc->model));
        _model->setThumbnail(source_index.row(), QIcon(thumbnail));
        ++rendered_count;
      }
      catch (std::exception const& e)
      {
        ++failed_count;
        _model->setThumbnailError(source_index.row(), QString::fromUtf8(e.what()));
        if (first_failure.isEmpty())
          first_failure = QString("Display %1: %2").arg(npc->model).arg(e.what());
        // A bad client display should not prevent the rest of the page from
        // being browsed or placed.
        LogError << "The NPC browser could not render display " << npc->model
                 << ": " << e.what() << std::endl;
      }
    }
    if (failed_count)
    {
      _status->setText(QString("Rendered %1 NPC preview(s); %2 failed. First failure: %3")
        .arg(rendered_count).arg(failed_count).arg(first_failure));
    }
  }

  void NpcTemplateBrowser::updateDetails()
  {
    cancelPlacement();
    _selected_gameobject_guid.reset();
    if (_phase_assignment_panel) _phase_assignment_panel->hide();
    if (_selected_spawn_guid && _map_view)
    {
      if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
      {
        overlay->setPreviewRoute({}, false);
        overlay->setTransform(_saved_spawn_position, _saved_spawn_yaw, overlay->scale());
      }
      _map_view->getWorld()->selectNpcSpawnOverlay({});
    }
    _selected_spawn_guid.reset();
    _moving_spawn = false;
    if (_move_spawn_button) _move_spawn_button->setText("Move NPC by clicking terrain");
    if (_spawn_editor) _spawn_editor->hide();
    if (_waypoint_panel) _waypoint_panel->setEnabled(false);
    _selected_model_path.clear();
    _selected_model_scale = 1.0f;
    QModelIndex const current = _table->currentIndex();
    QModelIndex const filtered = current.isValid() ? _page_filter->mapToSource(current) : QModelIndex{};
    auto const* npc = filtered.isValid()
      ? _model->entry(_filter->mapToSource(filtered).row()) : nullptr;
    if (!npc)
    {
      if (_delete_npc_template) _delete_npc_template->setEnabled(false);
      _details->setText("Select an NPC to inspect its template.");
      _preview->hide();
      return;
    }

    if (_delete_npc_template)
    {
      bool const generated = generatedNpcAssets(npc->model);
      _delete_npc_template->setEnabled(generated);
      _delete_npc_template->setToolTip(generated
        ? "Permanently delete this Noggit-created NPC after all of its placed spawns are removed."
        : "Only humanoid NPCs created from scratch by Noggit can be deleted here. Shared server "
          "templates are protected.");
    }

    showPropertiesPanel();

    _details->setText(templateDetails(*npc));

    loadDisplay(npc->model, true);
  }

  unsigned NpcTemplateBrowser::selectedTemplateEntry() const
  {
    if (!_table || !_page_filter || !_filter || !_model) return 0;
    QModelIndex const current = _table->currentIndex();
    QModelIndex const filtered = current.isValid()
      ? _page_filter->mapToSource(current) : QModelIndex{};
    QModelIndex const source = filtered.isValid()
      ? _filter->mapToSource(filtered) : QModelIndex{};
    auto const* npc = source.isValid() ? _model->entry(source.row()) : nullptr;
    return npc ? npc->entry : 0;
  }

  unsigned NpcTemplateBrowser::selectedTemplateDisplay() const
  {
    if (!_table || !_page_filter || !_filter || !_model) return 0;
    QModelIndex const current = _table->currentIndex();
    QModelIndex const filtered = current.isValid()
      ? _page_filter->mapToSource(current) : QModelIndex{};
    QModelIndex const source = filtered.isValid()
      ? _filter->mapToSource(filtered) : QModelIndex{};
    auto const* npc = source.isValid() ? _model->entry(source.row()) : nullptr;
    return npc ? npc->model : 0;
  }

  bool NpcTemplateBrowser::generatedNpcAssets(unsigned display_id, unsigned* extra_id,
                                               QString* texture_stem) const
  {
    if (!_display_info || !_display_extra || !display_id
        || _display_info->getFieldCount() <= 3 || _display_extra->getFieldCount() <= 20)
      return false;
    try
    {
      auto const display = _display_info->getByID(display_id);
      unsigned const resolved_extra = display.getUInt(3);
      if (!resolved_extra) return false;
      auto const extra = _display_extra->getByID(resolved_extra);
      QString const stem = QString::fromUtf8(extra.getString(20));
      if (stem != QString("NoggitNpc_%1").arg(display_id)) return false;
      if (extra_id) *extra_id = resolved_extra;
      if (texture_stem) *texture_stem = stem;
      return true;
    }
    catch (...) { return false; }
  }

  bool NpcTemplateBrowser::selectTemplate(unsigned entry, bool begin_placement)
  {
    if (!entry || !_model || !_filter || !_page_filter || !_table) return false;

    // Programmatic selection must not run updateDetails through intermediate
    // filter/page changes: that would clear the selected world spawn.
    QSignalBlocker const selection_blocker(_table->selectionModel());
    QSignalBlocker const search_blocker(_search);
    QSignalBlocker const faction_blocker(_faction);
    QSignalBlocker const type_blocker(_type);
    QSignalBlocker const role_blocker(_role);
    _search->clear();
    _faction->setCurrentIndex(0);
    _type->setCurrentIndex(0);
    _role->setCurrentIndex(0);
    if (begin_placement)
      updateFilters();
    else
    {
      _filter->setCriteria({}, -1, -1, -1);
      _page_filter->resetPage();
      updateBrowserPage();
    }

    int source_row = -1;
    for (int row = 0; row < _model->rowCount(); ++row)
    {
      auto const* npc = _model->entry(row);
      if (npc && npc->entry == entry)
      {
        source_row = row;
        break;
      }
    }
    if (source_row < 0) return false;

    QModelIndex const filtered = _filter->mapFromSource(_model->index(source_row, 0));
    if (!filtered.isValid()) return false;
    _page_filter->setPage(filtered.row() / _page_filter->pageSize());
    updateBrowserPage();
    QModelIndex const paged = _page_filter->mapFromSource(filtered);
    if (!paged.isValid()) return false;

    _table->setCurrentIndex(paged);
    _table->scrollTo(paged);
    if (begin_placement)
      updateDetails();
    else
    {
      auto const* npc = _model->entry(source_row);
      _details->setText(templateDetails(*npc));
      if (_delete_npc_template)
        _delete_npc_template->setEnabled(generatedNpcAssets(npc->model));
    }
    return true;
  }

  std::pair<unsigned, unsigned> NpcTemplateBrowser::itemVisual(
    unsigned item_entry, unsigned database_display,
    unsigned database_inventory_type) const
  {
    unsigned display = database_display;
    unsigned inventory_type = database_inventory_type;
    if (item_entry && _item_data && _item_data->getFieldCount() >= 7)
    {
      try
      {
        auto const item = _item_data->getByID(item_entry);
        if (!display) display = item.getUInt(5); // WotLK Item.dbc DisplayInfoID
        if (!inventory_type) inventory_type = item.getUInt(6);
      }
      catch (DBCFile::NotFound const&) {}
    }
    return {display, inventory_type};
  }

  unsigned NpcTemplateBrowser::rangedHandAttachment(unsigned item_entry,
                                                     unsigned database_subclass) const
  {
    // Bows are held in the left palm; guns and crossbows use the right palm.
    if (item_entry && _item_data && _item_data->getFieldCount() >= 3)
    {
      try
      {
        auto const item = _item_data->getByID(item_entry);
        return item.getUInt(1) == 2 && item.getUInt(2) == 2 ? 2 : 1;
      }
      catch (DBCFile::NotFound const&) {}
    }
    return database_subclass == 2 ? 2 : 1;
  }

  std::string NpcTemplateBrowser::npcModelPath(unsigned display_id) const
  {
    if (!_display_info || !_model_data || !display_id) return {};
    try
    {
      auto const display = _display_info->getByID(display_id);
      std::string path = _model_data->getByID(display.getUInt(1)).getString(2);
      if (path.size() >= 4)
      {
        QString const extension = QString::fromStdString(path.substr(path.size() - 4));
        if (extension.compare(".mdx", Qt::CaseInsensitive) == 0
            || extension.compare(".mdl", Qt::CaseInsensitive) == 0)
          path.replace(path.size() - 4, 4, ".m2");
      }
      return path;
    }
    catch (std::exception const&) { return {}; }
  }

  Model* NpcTemplateBrowser::prefetchNpcModel(
    std::string const& path, Noggit::NoggitRenderContext context)
  {
    if (path.empty()) return nullptr;
    auto const key = std::make_pair(context, path);
    auto found = _prefetched_npc_models.find(key);
    if (found == _prefetched_npc_models.end())
    {
      if (_prefetched_npc_models.size() >= 256)
      {
        auto const ready = std::find_if(_prefetched_npc_models.begin(),
          _prefetched_npc_models.end(), [](auto const& entry)
          {
            return entry.second->get()->finishedLoading();
          });
        if (ready != _prefetched_npc_models.end())
          _prefetched_npc_models.erase(ready);
      }
      found = _prefetched_npc_models.emplace(key,
        std::make_unique<scoped_model_reference>(
          BlizzardArchive::Listfile::FileKey(path), context)).first;
    }
    return found->second->get();
  }

  blp_texture* NpcTemplateBrowser::prefetchNpcTexture(
    std::string const& path, Noggit::NoggitRenderContext context)
  {
    if (path.empty()) return nullptr;
    auto const key = std::make_pair(context, path);
    auto found = _prefetched_npc_textures.find(key);
    if (found == _prefetched_npc_textures.end())
    {
      if (_prefetched_npc_textures.size() >= 256)
      {
        auto const ready = std::find_if(_prefetched_npc_textures.begin(),
          _prefetched_npc_textures.end(), [](auto const& entry)
          {
            return entry.second->get()->finishedLoading();
          });
        if (ready != _prefetched_npc_textures.end())
          _prefetched_npc_textures.erase(ready);
      }
      found = _prefetched_npc_textures.emplace(key,
        std::make_unique<scoped_blp_texture_reference>(path, context)).first;
    }
    return found->second->get();
  }

  bool NpcTemplateBrowser::loadDisplay(unsigned display_id, bool begin_placement,
                                       QString* failure,
                                       Tools::AssetBrowser::ModelViewer* target_preview,
                                       float* target_scale, bool* pending)
  {
    if (pending) *pending = false;
    auto* preview = target_preview ? target_preview : _preview;
    if (failure) failure->clear();
    auto fail = [this, begin_placement, failure](QString const& message)
    {
      if (failure) *failure = message;
      if (begin_placement) _status->setText(message);
      return false;
    };
    if (!target_preview)
    {
      preview->hide();
      _selected_model_path.clear();
      _selected_model_scale = 1.0f;
    }
    if (!_display_info || !_model_data || display_id == 0)
    {
      return fail("This NPC has no usable client display data.");
    }
    try
    {
      auto const display = _display_info->getByID(display_id);
      unsigned const model_id = display.getUInt(1);
      auto const model_data = _model_data->getByID(model_id);
      std::string filename = model_data.getString(2);
      if (filename.size() >= 4)
      {
        auto const extension = filename.substr(filename.size() - 4);
        if (extension == ".mdx" || extension == ".MDX" || extension == ".mdl" || extension == ".MDL")
          filename.replace(filename.size() - 4, 4, ".m2");
      }
      if (!filename.empty())
      {
        if (target_preview == _cache_preview)
        {
          Model* const prefetched = prefetchNpcModel(
            filename, Noggit::NoggitRenderContext::NPC_SPAWN_CACHE);
          if (!prefetched) return fail("Could not request NPC model.");
          if (!prefetched->finishedLoading())
          {
            if (pending) *pending = true;
            return false;
          }
          if (prefetched->loading_failed() || prefetched->skin_load_failed())
            return fail("NPC model could not be loaded.");
        }
        float const display_scale = display.getFloat(4);
        float const model_scale = model_data.getFloat(4);
        float const combined_scale = display_scale * model_scale;
        float const scale = std::isfinite(combined_scale) && combined_scale > 0.0f
          ? std::clamp(combined_scale, 0.01f, 100.0f) : 1.0f;
        if (target_scale) *target_scale = scale;
        if (!target_preview)
        {
          _selected_model_path = filename;
          _selected_model_scale = scale;
          preview->show();
        }
        preview->setModel(filename);

        std::size_t applied = 0;
        bool helmet_added = false;
        unsigned shoulders_added = 0;
        std::map<unsigned, unsigned> geoset_variants;
        bool character_model = false;
        bool show_scalp = false;
        auto apply_texture = [this, preview, &applied](std::size_t type, std::string const& path)
        {
          if (!path.empty() && _client_data->exists(BlizzardArchive::Listfile::FileKey(path))
              && preview->setCreatureTexture(type, path))
            ++applied;
        };

        auto const separator = filename.find_last_of("\\/");
        std::string const model_directory = separator == std::string::npos
          ? std::string() : filename.substr(0, separator + 1);
        for (std::size_t variation = 0; variation < 3; ++variation)
          apply_texture(11 + variation,
                        texturePath(display.getString(6 + variation), model_directory));

        if (_display_extra && display.getUInt(3) != 0)
        {
          try
          {
            auto const extra = _display_extra->getByID(display.getUInt(3));
            character_model = true;
            apply_texture(1, texturePath(extra.getString(20), "Textures\\BakedNpcTextures\\"));

            unsigned const race = extra.getUInt(1);
            unsigned const sex = extra.getUInt(2);
            if (_hair_geosets)
            {
              for (std::size_t row = 0; row < _hair_geosets->getRecordCount(); ++row)
              {
                auto const hair = _hair_geosets->getRecord(row);
                if (hair.getUInt(1) == race && hair.getUInt(2) == sex
                    && hair.getUInt(3) == extra.getUInt(5))
                {
                  geoset_variants[0] = hair.getUInt(4) % 100;
                  show_scalp = _hair_geosets->getFieldCount() > 5
                    && hair.getUInt(5) != 0;
                  break;
                }
              }
            }
            std::map<unsigned, unsigned> base_facial_variants{{1, 0}, {2, 0}, {3, 0}};
            std::map<unsigned, unsigned> selected_facial_variants;
            if (_facial_hair_styles && _facial_hair_styles->getFieldCount() >= 6)
            {
              // WotLK's eight-field DBC stores facial geosets at 3-5.
              // Some nine-field clients have padding there and use 6-8.
              std::size_t const first = _facial_hair_styles->getFieldCount() >= 9 ? 6 : 3;
              for (std::size_t row = 0; row < _facial_hair_styles->getRecordCount(); ++row)
              {
                auto const facial = _facial_hair_styles->getRecord(row);
                if (facial.getUInt(0) != race || facial.getUInt(1) != sex) continue;
                unsigned const style = facial.getUInt(2);
                if (style != 0 && style != extra.getUInt(7)) continue;
                auto& target = style == 0 ? base_facial_variants : selected_facial_variants;
                unsigned const group1 = facial.getUInt(first);
                unsigned const group2 = facial.getUInt(first + 2);
                unsigned const group3 = facial.getUInt(first + 1);
                if (group1 < 100) target[1] = group1;
                if (group2 < 100) target[2] = group2;
                if (group3 < 100) target[3] = group3;
              }
            }
            for (unsigned group = 1; group < 4; ++group)
              geoset_variants[group] = selected_facial_variants.contains(group)
                ? selected_facial_variants[group] : base_facial_variants[group];

            if (_item_display_info)
            {
              if (_chr_races && extra.getUInt(8) && sex < 2)
              {
                try
                {
                  auto const race_record = _chr_races->getByID(race);
                  auto const helmet = _item_display_info->getByID(extra.getUInt(8));
                  auto const model_path = helmetModelPath(
                      helmet.getString(1), race_record.getString(6), sex);
                  if (!model_path.empty()
                      && _client_data->exists(BlizzardArchive::Listfile::FileKey(model_path)))
                  {
                    auto const texture_path = texturePath(
                        helmet.getString(3), "Item\\ObjectComponents\\Head\\");
                    helmet_added = preview->setCreatureAttachment(
                        11, model_path,
                        !texture_path.empty()
                            && _client_data->exists(BlizzardArchive::Listfile::FileKey(texture_path))
                          ? texture_path : std::string());
                  }
                }
                catch (DBCFile::NotFound const&) {}
              }

              if (extra.getUInt(9))
              {
                try
                {
                  auto const shoulders = _item_display_info->getByID(extra.getUInt(9));
                  for (unsigned side = 0; side < 2; ++side)
                  {
                    auto const model_path = shoulderModelPath(shoulders.getString(1 + side));
                    if (model_path.empty()
                        || !_client_data->exists(BlizzardArchive::Listfile::FileKey(model_path)))
                      continue;
                    auto const texture_path = texturePath(
                        shoulders.getString(3 + side), "Item\\ObjectComponents\\Shoulder\\");
                    if (preview->setCreatureAttachment(
                            side == 0 ? 6u : 5u, model_path,
                            !texture_path.empty()
                                && _client_data->exists(BlizzardArchive::Listfile::FileKey(texture_path))
                              ? texture_path : std::string()))
                      ++shoulders_added;
                  }
                }
                catch (DBCFile::NotFound const&) {}
              }

              auto apply_item = [this, &extra, &geoset_variants](std::size_t slot,
                                                                  unsigned first_group,
                                                                  unsigned second_group = 0,
                                                                  unsigned third_group = 0)
              {
                unsigned const item_id = extra.getUInt(slot);
                if (!item_id) return;
                try
                {
                  auto const item = _item_display_info->getByID(item_id);
                  if (first_group) geoset_variants[first_group] = 1 + item.getUInt(7);
                  if (second_group) geoset_variants[second_group] = 1 + item.getUInt(8);
                  // Group 13 is the robe/skirt shape. A zero in a later
                  // item's third field means it has no robe; it must not
                  // erase a robe requested by the shirt or leg item.
                  if (third_group && item.getUInt(9) != 0)
                    geoset_variants[third_group] = 1 + item.getUInt(9);
                }
                catch (DBCFile::NotFound const&) {}
              };
              // ItemDisplayInfo's three geoset values target different body
              // groups according to the equipment slot. Zero selects the
              // base variant for the first two groups, but does not cancel
              // a robe selected by another equipped item.
              apply_item(13, 11, 9, 13); // legs: pants, kneepads, robe
              apply_item(10, 8, 10, 13); // shirt: sleeves, chest, robe
              apply_item(11, 8, 10, 13); // chest overrides shirt
              apply_item(12, 18);         // belt
              apply_item(14, 5);          // boots
              apply_item(16, 4, 23);      // gloves and hand attachments
              apply_item(17, 12); // tabard
              if (extra.getUInt(18))
              {
                try
                {
                  auto const cape = _item_display_info->getByID(extra.getUInt(18));
                  geoset_variants[15] = 1 + cape.getUInt(7);

                  // Cloaks use replacement-texture slot 2. Enabling the cloak
                  // geoset without supplying this texture renders it black.
                  std::string stem = cape.getString(3);
                  std::replace(stem.begin(), stem.end(), '/', '\\');
                  if (!stem.empty())
                  {
                    QString const name = QString::fromStdString(stem);
                    if (name.endsWith(".blp", Qt::CaseInsensitive))
                      stem.resize(stem.size() - 4);
                    bool cape_applied = false;
                    for (std::string const& folder :
                         {std::string("Item\\ObjectComponents\\Cape\\"),
                          std::string("Item\\TextureComponents\\Cape\\")})
                    {
                      for (std::string const& suffix :
                           {sex == 0 ? std::string("_M") : std::string("_F"),
                            std::string("_U"), std::string()})
                      {
                        std::string const path = (stem.find('\\') == std::string::npos
                          ? folder : std::string()) + stem + suffix + ".blp";
                        if (_client_data->exists(BlizzardArchive::Listfile::FileKey(path)))
                        {
                          apply_texture(2, path);
                          cape_applied = true;
                          break;
                        }
                      }
                      if (cape_applied) break;
                    }
                  }
                }
                catch (DBCFile::NotFound const&) {}
              }

              if (extra.getUInt(14))
              {
                try
                {
                  auto const boots = _item_display_info->getByID(extra.getUInt(14));
                  // The character feet group starts at geoset 2002 when
                  // boots are equipped, plus ItemDisplayInfo's second value.
                  geoset_variants[20] = 2 + boots.getUInt(8);
                }
                catch (DBCFile::NotFound const&) {}
              }

              if (_helmet_geoset_vis && helmet_added && race < 32 && sex < 2)
              {
                try
                {
                  auto const helm = _item_display_info->getByID(extra.getUInt(8));
                  unsigned const vis_id = helm.getUInt(13 + sex);
                  if (vis_id)
                  {
                    auto const vis = _helmet_geoset_vis->getByID(vis_id);
                    auto hide_selected_if_masked = [&geoset_variants, &vis]
                      (unsigned group, unsigned mask_field, unsigned default_variant,
                       unsigned replacement_variant)
                    {
                      unsigned variant = default_variant;
                      if (auto const selected = geoset_variants.find(group);
                          selected != geoset_variants.end())
                        variant = selected->second;
                      // Facial style zero supplies the base lower-face pieces.
                      // Replacing a masked style with variant zero can remove
                      // the face when that model has no zero variant.
                      if (variant < 32 && (vis.getUInt(mask_field) & (1u << variant)))
                        geoset_variants[group] = replacement_variant;
                    };
                    hide_selected_if_masked(0, 1, 0, 0); // hair
                    for (unsigned group = 1; group < 4; ++group)
                      hide_selected_if_masked(group, 1 + group, base_facial_variants[group],
                                              base_facial_variants[group]);
                    hide_selected_if_masked(7, 5, 2, 0); // ears
                  }
                }
                catch (DBCFile::NotFound const&) {}
              }
            }

            // In the 3.3.5 CharSections layout, fields 4-6 are texture paths,
            // and fields 8-9 select the hair style and color.
            if (_char_sections)
            {
              std::string hair_texture;
              bool hair_section_found = false;
              for (std::size_t row = 0; row < _char_sections->getRecordCount(); ++row)
              {
                auto const section = _char_sections->getRecord(row);
                if (section.getUInt(1) != extra.getUInt(1) // race
                    || section.getUInt(2) != extra.getUInt(2) // sex
                    || section.getUInt(3) != 3 // hair
                    || section.getUInt(8) != extra.getUInt(5) // style
                    || section.getUInt(9) != extra.getUInt(6)) // color
                  continue;
                hair_texture = section.getString(4);
                hair_section_found = true;
                break;
              }

              if (hair_section_found)
              {
                // Character M2s live one directory below their race textures.
                std::string race_directory = model_directory;
                if (!race_directory.empty())
                {
                  auto const parent = race_directory.find_last_of("\\/", race_directory.size() - 2);
                  if (parent != std::string::npos) race_directory.resize(parent + 1);
                }
                if (hair_texture.empty() && extra.getUInt(2) == 0)
                  hair_texture = QString("Hair00_%1.blp")
                    .arg(extra.getUInt(6), 2, 10, QChar('0')).toStdString();
                apply_texture(6, texturePath(hair_texture, race_directory));
              }
            }
          }
          catch (DBCFile::NotFound const&) {}
        }
        preview->setCreatureGeosets(geoset_variants, character_model, show_scalp);
        if (!target_preview) _preview_note->setText(
            QString("Applied %1 client display texture(s). %2 %3 Equipment and some character details may still differ in game.")
              .arg(applied)
              .arg(helmet_added ? "Helmet model loaded." : "")
              .arg(shoulders_added ? QString("%1 shoulder model(s) loaded.").arg(shoulders_added)
                                   : QString()));
        if (_map_view && begin_placement) startPlacement();
        return true;
      }
    }
    catch (std::exception const& e)
    {
      if (!target_preview) preview->hide();
      return fail(QString("Could not load display %1: %2").arg(display_id).arg(e.what()));
    }
    return fail(QString("Display %1 has no model filename.").arg(display_id));
  }

  bool NpcTemplateBrowser::eventFilter(QObject* watched, QEvent* event)
  {
    bool const map_event = watched == _map_view;
    bool const table_event = watched == _table || watched == _table->viewport();
    if ((!map_event && !table_event) || (table_event && !_placement_entry))
      return QWidget::eventFilter(watched, event);

    if (map_event && event->type() == QEvent::FocusOut)
    {
      _selected_spawn_rotating = false;
      finishNpcTransformEdit();
    }

    auto scene_surface = [this](QPoint const& mouse_position)
      -> std::optional<std::pair<float, glm::vec3>>
    {
      if (!_map_view) return {};
      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const context(::gl, _map_view->context());
      math::ray const ray = _map_view->intersect_ray(mouse_position);
      std::optional<std::pair<float, glm::vec3>> closest;
      for (auto const& result : _map_view->intersect_result(mouse_position, false, true))
      {
        if (result.second.index() == eEntry_MapChunk)
        {
          if (result.first >= 0.0f && (!closest || result.first < closest->first))
            closest = std::make_pair(
              result.first, std::get<selected_chunk_type>(result.second).position);
          continue;
        }

        SceneObject* const object = std::get<selected_object_type>(result.second);
        if (!object || object->which() != eWMO)
          continue;

        // WMO triangle intersections are measured in the instance's normalized
        // local ray, so restore uniform instance scale before projecting the
        // hit point back along the world-space click ray.
        float const distance = result.first * std::max(std::abs(object->scale), 0.001f);
        if (distance >= 0.0f && (!closest || distance < closest->first))
          closest = std::make_pair(distance, ray.position(distance));
      }
      return closest;
    };

    if (map_event && _selected_spawn_rotating && event->type() == QEvent::MouseMove)
    {
      auto* mouse = static_cast<QMouseEvent*>(event);
      int const delta_x = mouse->pos().x() - _selected_spawn_rotation_mouse_position.x();
      _selected_spawn_rotation_mouse_position = mouse->pos();
      if (delta_x)
      {
        rotateSelectedSpawn(static_cast<float>(delta_x) * 0.6f);
        _pending_transform_edit = true;
      }
      event->accept();
      return true;
    }

    if (map_event && _placement_entry && event->type() == QEvent::MouseMove)
    {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (_placement_rotating)
      {
        int const delta_x = mouse->pos().x() - _placement_rotation_mouse_position.x();
        _placement_rotation_mouse_position = mouse->pos();
        if (delta_x)
          rotatePlacement(static_cast<float>(delta_x) * 0.6f);
        event->accept();
        return true;
      }
      if (auto const surface = scene_surface(mouse->pos()))
        setPlacementPosition(surface->second);
      return mouse->buttons().testFlag(Qt::LeftButton);
    }

    if (map_event && event->type() == QEvent::MouseButtonPress)
    {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() == Qt::LeftButton)
      {
        _consume_npc_left_release = false;
        if (_selected_spawn_guid
            && _map_view->getWorld()->selectedNpcSpawnOverlay()
            && _map_view->transformGizmoCapturesMouse())
          return QWidget::eventFilter(watched, event);

        if (_capturing_waypoints && _selected_spawn_guid)
        {
          if (auto const surface = scene_surface(mouse->pos()))
            addWaypoint(surface->second);
          else
            _status->setText("Click loaded terrain or a WMO surface to add a waypoint.");
          _consume_npc_left_release = true;
          event->accept();
          return true;
        }

        if (_moving_spawn && _selected_spawn_guid)
        {
          if (auto const surface = scene_surface(mouse->pos()))
          {
            _edit_spawn_position = surface->second;
            _edit_position_label->setText(QString("%1, %2, %3")
              .arg(surface->second.x, 0, 'f', 2).arg(surface->second.y, 0, 'f', 2)
              .arg(surface->second.z, 0, 'f', 2));
            if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
              overlay->setTransform(surface->second, static_cast<float>(_edit_yaw->value()),
                                    overlay->scale());
            updateMovementPreview();
            recordNpcEdit(EditTransform);
            _status->setText("NPC moved in Noggit. Save spawn settings to update the database.");
          }
          _consume_npc_left_release = true;
          event->accept();
          return true;
        }

        float surface_distance = std::numeric_limits<float>::max();
        if (auto const surface = scene_surface(mouse->pos()))
          surface_distance = surface->first;
        if (auto const picked = _map_view->getWorld()->pickNpcSpawnOverlay(
              _map_view->intersect_ray(mouse->pos()), surface_distance + 0.5f))
        {
          cancelPlacement();
          selectSpawn(picked->first, picked->second);
          _consume_npc_left_release = true;
          event->accept();
          return true;
        }
        if (auto const picked = _map_view->getWorld()->pickServerGameObjectOverlay(
              _map_view->intersect_ray(mouse->pos()), surface_distance + 0.5f))
        {
          cancelPlacement();
          selectGameObjectSpawn(picked->first, picked->second);
          _consume_npc_left_release = true;
          event->accept();
          return true;
        }

        // A normal viewport click that misses every NPC hands control back to
        // Noggit's object/terrain selection. Clear only the viewport NPC
        // selection so a previously edited spawn cannot suppress that gizmo.
        if (_selected_spawn_guid)
          _map_view->getWorld()->selectNpcSpawnOverlay({});

        if (!_placement_entry) return QWidget::eventFilter(watched, event);
        if (auto const surface = scene_surface(mouse->pos()))
        {
          setPlacementPosition(surface->second);
          if (_placement_overlay_guid) createSpawn();
        }
        else
          _status->setText("NPC placement needs loaded terrain or a WMO surface.");
        event->accept();
        return true;
      }
    }

    if (map_event && _consume_npc_left_release
        && event->type() == QEvent::MouseButtonRelease)
    {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() == Qt::LeftButton)
      {
        _consume_npc_left_release = false;
        event->accept();
        return true;
      }
    }

    if (map_event && event->type() == QEvent::MouseButtonRelease)
      finishNpcTransformEdit();

    if (map_event && _placement_entry && (event->type() == QEvent::MouseButtonRelease
        || event->type() == QEvent::MouseButtonDblClick))
    {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() == Qt::LeftButton)
      {
        event->accept();
        return true;
      }
    }

    if (event->type() == QEvent::KeyPress)
    {
      auto* key = static_cast<QKeyEvent*>(event);
      if (key->key() == Qt::Key_Escape)
      {
        if (_selected_spawn_rotating)
          _selected_spawn_rotating = false;
        else if (_capturing_waypoints)
        {
          _capturing_waypoints = false;
          _capture_waypoint_button->setText("Add points by clicking terrain");
        }
        else if (_moving_spawn)
        {
          _moving_spawn = false;
          _move_spawn_button->setText("Move NPC by clicking terrain");
        }
        else if (_placement_entry)
          cancelPlacement("NPC placement stopped. Click an NPC row to start again.");
        else
          return QWidget::eventFilter(watched, event);
        event->accept();
        return true;
      }
      if (_placement_entry && key->key() == Qt::Key_R)
      {
        if (!key->isAutoRepeat())
        {
          _placement_rotating = true;
          _placement_rotation_mouse_position = _map_view->mapFromGlobal(QCursor::pos());
        }
        event->accept();
        return true;
      }
      if (map_event && !_placement_entry && !_capturing_waypoints && !_moving_spawn
          && _selected_spawn_guid
          && _map_view->getWorld()->selectedNpcSpawnOverlay()
          && key->key() == Qt::Key_R)
      {
        if (!key->isAutoRepeat())
        {
          _selected_spawn_rotating = true;
          _selected_spawn_rotation_mouse_position = _map_view->mapFromGlobal(QCursor::pos());
        }
        event->accept();
        return true;
      }
    }

    if (event->type() == QEvent::KeyRelease)
    {
      auto* key = static_cast<QKeyEvent*>(event);
      if (_placement_entry && key->key() == Qt::Key_R)
      {
        if (!key->isAutoRepeat())
          _placement_rotating = false;
        event->accept();
        return true;
      }
      if (_selected_spawn_rotating && key->key() == Qt::Key_R)
      {
        if (!key->isAutoRepeat())
        {
          _selected_spawn_rotating = false;
          finishNpcTransformEdit();
        }
        event->accept();
        return true;
      }
    }

    return QWidget::eventFilter(watched, event);
  }

  void NpcTemplateBrowser::hideEvent(QHideEvent* event)
  {
    finishNpcTransformEdit();
    _selected_gameobject_guid.reset();
    if (_phase_assignment_panel) _phase_assignment_panel->hide();
    if (_selected_spawn_guid)
      _draft_snapshots[*_selected_spawn_guid] = captureEditorSnapshot();
    _selected_spawn_rotating = false;
    if (_placement_entry)
      cancelPlacement("NPC placement stopped because the browser was closed.");
    if (_selected_spawn_guid && _map_view && _map_view->getWorld())
    {
      if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(*_selected_spawn_guid))
      {
        overlay->setPreviewRoute({}, false);
        overlay->setTransform(_saved_spawn_position, _saved_spawn_yaw, overlay->scale());
      }
      _map_view->getWorld()->selectNpcSpawnOverlay({});
      _selected_spawn_guid.reset();
      _spawn_editor->hide();
      if (_waypoint_panel) _waypoint_panel->setEnabled(false);
      _capturing_waypoints = false;
      _moving_spawn = false;
    }
    QWidget::hideEvent(event);
  }

  void NpcTemplateBrowser::startPlacement()
  {
    QModelIndex const current = _table->currentIndex();
    QModelIndex const filtered = current.isValid() ? _page_filter->mapToSource(current) : QModelIndex{};
    auto const* npc = filtered.isValid()
      ? _model->entry(_filter->mapToSource(filtered).row()) : nullptr;
    if (!npc || !_map_view || _selected_model_path.empty())
    {
      _status->setText("Select an NPC with a valid client model before placing it.");
      return;
    }

    cancelPlacement();
    _selected_spawn_rotating = false;
    _map_view->getWorld()->selectNpcSpawnOverlay({});
    _placement_entry = npc->entry;
    _placement_display_id = npc->model;
    _placement_equipment_id = 0;
    _placement_weapon_stance = -1;
    _placement_name = npc->name;
    _placement_position = _map_view->cursorPosition();
    _placement_yaw = 0.0f;
    _placement_scale = _selected_model_scale;

    // A creature's held items are not part of CreatureDisplayInfo. Resolve the
    // template's preferred equipment set separately, attach it to the cursor
    // preview, and carry the same set ID into the new creature row.
    QString equipment_warning;
    QString const equipment_connection = QString("npc_placement_equipment_%1")
      .arg(reinterpret_cast<quintptr>(this));
    {
      QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", equipment_connection);
      db.setHostName(_host->text().trimmed());
      db.setPort(_port->value());
      db.setDatabaseName(_database->text().trimmed());
      db.setUserName(_user->text().trimmed());
      db.setPassword(_password->text());
      db.setConnectOptions("MYSQL_OPT_CONNECT_TIMEOUT=5");
      if (!db.open())
        equipment_warning = " Could not load the template's equipment: "
          + db.lastError().text();
      else
      {
        QSqlQuery equipment(db);
        equipment.prepare(
          "SELECT e.ID, e.ItemID1, i1.displayid, i1.InventoryType, "
          "e.ItemID2, i2.displayid, i2.InventoryType, "
          "e.ItemID3, i3.displayid, i3.InventoryType, i3.Subclass "
          "FROM creature_equip_template e "
          "LEFT JOIN item_template i1 ON i1.entry = e.ItemID1 "
          "LEFT JOIN item_template i2 ON i2.entry = e.ItemID2 "
          "LEFT JOIN item_template i3 ON i3.entry = e.ItemID3 "
          "WHERE e.CreatureID = ? "
          "ORDER BY CASE WHEN e.ID = 1 THEN 0 ELSE 1 END, e.ID LIMIT 1");
        equipment.addBindValue(_placement_entry);
        if (!equipment.exec())
          equipment_warning = " Could not load the template's equipment: "
            + equipment.lastError().text();
        else if (equipment.next())
        {
          _placement_equipment_id = equipment.value(0).toInt();
          static constexpr std::array<unsigned, 3> attachment_ids{1, 2, 12};
          for (std::size_t slot = 0; slot < attachment_ids.size(); ++slot)
          {
            int const column = 1 + static_cast<int>(slot) * 3;
            auto const [display_id, inventory_type] = itemVisual(
              equipment.value(column).toUInt(), equipment.value(column + 1).toUInt(),
              equipment.value(column + 2).toUInt());
            if (display_id)
              setWeaponAttachment(_preview, _item_display_info.get(), _client_data,
                                  display_id, inventory_type, attachment_ids[slot],
                                  slot == 2
                                    ? rangedHandAttachment(equipment.value(column).toUInt(),
                                        equipment.value(10).toUInt()) : 0);
          }
        }
        db.close();
      }
    }
    QSqlDatabase::removeDatabase(equipment_connection);

    try
    {
      auto const appearance = _preview->creatureAppearance();
      if (!appearance)
        throw std::runtime_error("NPC appearance is unavailable");
      _placement_appearance = *appearance;
      _map_view->getWorld()->addNpcSpawnOverlay(
        PLACEMENT_OVERLAY_GUID, _placement_entry, _placement_position,
        _placement_yaw, _placement_scale, *_placement_appearance);
      _placement_overlay_guid = PLACEMENT_OVERLAY_GUID;
    }
    catch (std::exception const& exception)
    {
      _placement_appearance.reset();
      _placement_entry = 0;
      _status->setText(QString("Could not start NPC placement: %1").arg(exception.what()));
      return;
    }

    _placement_panel->show();
    updatePlacementSummary();
    _status->setText("Move over loaded terrain or a WMO surface and left-click to spawn. "
                     "Keep clicking to place more; Esc stops." + equipment_warning);
    _map_view->invalidate();
    _map_view->update();
  }

  void NpcTemplateBrowser::cancelPlacement(QString const& message)
  {
    if (_placement_overlay_guid && _map_view && _map_view->getWorld())
    {
      _map_view->getWorld()->removeNpcSpawnOverlay(*_placement_overlay_guid);
      _map_view->invalidate();
      _map_view->update();
    }
    _placement_overlay_guid.reset();
    _placement_appearance.reset();
    _placement_entry = 0;
    _placement_equipment_id = 0;
    _placement_weapon_stance = -1;
    _placement_rotating = false;
    if (_placement_panel) _placement_panel->hide();
    if (!message.isEmpty() && _status) _status->setText(message);
  }

  void NpcTemplateBrowser::setPlacementPosition(glm::vec3 const& position)
  {
    if (!_placement_entry || !_map_view || !_placement_appearance)
      return;
    _placement_position = position;
    if (_placement_overlay_guid)
      _map_view->getWorld()->updateNpcSpawnOverlay(
        *_placement_overlay_guid, _placement_position, _placement_yaw, _placement_scale);
    else
    {
      try
      {
        _map_view->getWorld()->addNpcSpawnOverlay(
          PLACEMENT_OVERLAY_GUID, _placement_entry, _placement_position,
          _placement_yaw, _placement_scale, *_placement_appearance);
        if (_placement_weapon_stance == 1 || _placement_weapon_stance == 2)
          if (auto* overlay = _map_view->getWorld()->findNpcSpawnOverlay(
                PLACEMENT_OVERLAY_GUID))
            overlay->setWeaponVisibility(_placement_weapon_stance == 1,
                                         _placement_weapon_stance == 2);
        _placement_overlay_guid = PLACEMENT_OVERLAY_GUID;
      }
      catch (std::exception const& exception)
      {
        cancelPlacement(QString("Could not show the NPC placement preview: %1").arg(exception.what()));
        return;
      }
    }
    updatePlacementSummary();
    _map_view->invalidate();
    _map_view->update();
  }

  void NpcTemplateBrowser::rotatePlacement(float degrees)
  {
    if (!_placement_entry || !_map_view)
      return;
    _placement_yaw = normalizedYaw(_placement_yaw + degrees);
    if (_placement_overlay_guid)
      _map_view->getWorld()->updateNpcSpawnOverlay(
        *_placement_overlay_guid, _placement_position, _placement_yaw, _placement_scale);
    updatePlacementSummary();
    _map_view->invalidate();
    _map_view->update();
  }

  void NpcTemplateBrowser::rotateSelectedSpawn(float degrees)
  {
    if (!_selected_spawn_guid || !_map_view || !_map_view->getWorld())
      return;
    auto* overlay = _map_view->getWorld()->selectedNpcSpawnOverlay();
    if (!overlay || overlay->guid() != *_selected_spawn_guid)
      return;

    float const yaw = normalizedYaw(overlay->anchorYaw() + degrees);
    overlay->setPreviewRoute({}, false);
    overlay->setTransform(_edit_spawn_position, yaw, overlay->scale());
    {
      QSignalBlocker const blocker(_edit_yaw);
      _edit_yaw->setValue(yaw);
    }
    _status->setText("NPC facing changed. Save spawn settings to update the database.");
    _map_view->invalidate();
    _map_view->update();
  }

  void NpcTemplateBrowser::updatePlacementSummary()
  {
    if (!_placement_summary || !_placement_entry || !_map_view) return;
    float const server_x = ZEROPOINT - _placement_position.z;
    float const server_y = ZEROPOINT - _placement_position.x;
    float const server_z = _placement_position.y;
    _placement_summary->setText(
      QString("<b>%1</b> (template %2)<br>Display: %3 · Server model: uses this display<br>"
              "Equipment set: %4<br>State: %5<br>Map: %6<br>Server position: %7, %8, %9<br>"
              "Orientation: %10 radians<br>Movement: stationary · Wander distance: 0<br>"
              "Difficulty/spawn mask: default 1 · Phase mask: default 1")
        .arg(_placement_name.toHtmlEscaped()).arg(_placement_entry).arg(_placement_display_id)
        .arg(_placement_equipment_id ? QString::number(_placement_equipment_id) : "none")
        .arg("click terrain to spawn")
        .arg(_map_view->getWorld()->getMapID())
        .arg(server_x, 0, 'f', 3).arg(server_y, 0, 'f', 3).arg(server_z, 0, 'f', 3)
        .arg(serverOrientation(_placement_yaw), 0, 'f', 5));
  }

  void NpcTemplateBrowser::createSpawn()
  {
    if (!_placement_entry || !_placement_overlay_guid || !_placement_appearance || !_map_view)
    {
      _status->setText("Select an NPC and click loaded terrain or a WMO surface to create a spawn.");
      return;
    }

    _status->setText("Creating NPC spawn...");
    std::uint64_t guid = 0;
    QString error;
    if (!insertSpawn(guid, error))
    {
      _status->setText("Could not create NPC spawn: " + error);
      // A failed insert leaves the cursor preview in place for another attempt.
      _map_view->invalidate();
      _map_view->update();
      return;
    }
    if (!guid)
    {
      cancelPlacement("The NPC row was created, but its GUID could not be read. Placement stopped to avoid creating a duplicate. Check the creature table before trying again.");
      return;
    }

    updateCachedSpawn(guid, _placement_entry, _placement_position,
                      _placement_display_id, _placement_yaw,
                      &*_placement_appearance, _placement_scale, _active_phase_mask);

    QString overlay_warning;
    std::uint64_t const temporary_overlay_guid = *_placement_overlay_guid;
    World* world = _map_view->getWorld();
    try
    {
      bool const committed = world->commitNpcSpawnOverlay(
        temporary_overlay_guid, guid, _placement_entry);
      if (!committed)
      {
        world->addNpcSpawnOverlay(
          guid, _placement_entry, _placement_position, _placement_yaw,
          _placement_scale, *_placement_appearance);
        if (_placement_weapon_stance == 1 || _placement_weapon_stance == 2)
          if (auto* overlay = world->findNpcSpawnOverlay(guid))
            overlay->setWeaponVisibility(_placement_weapon_stance == 1,
                                         _placement_weapon_stance == 2);
        world->removeNpcSpawnOverlay(temporary_overlay_guid);
      }
      _placement_overlay_guid.reset();
    }
    catch (std::exception const& exception)
    {
      world->removeNpcSpawnOverlay(temporary_overlay_guid);
      _placement_overlay_guid.reset();
      overlay_warning = QString(" The database row was created, but showing it in Noggit failed: %1")
        .arg(exception.what());
    }

    _map_view->invalidate();
    _map_view->update();
    QString const stance_warning = error.isEmpty() ? QString()
      : QString(" The spawn exists, but its drawn weapon was not saved: %1").arg(error);
    _status->setText(QString("Created %1 (template %2), GUID %3. Move the cursor and click to place another. Reload the world server to see it in game.%4%5")
      .arg(_placement_name).arg(_placement_entry).arg(guid)
      .arg(overlay_warning, stance_warning));
    if (!error.isEmpty())
      QMessageBox::warning(this, "NPC spawned without weapon stance",
        QString("Spawn GUID %1 was created, but its in-game drawn weapon could not be saved: %2")
          .arg(guid).arg(error));
  }

  bool NpcTemplateBrowser::insertSpawn(std::uint64_t& guid, QString& error)
  {
    if (!_map_view || !_placement_entry)
    {
      error = "The placement is no longer available.";
      return false;
    }
    if (!QSqlDatabase::isDriverAvailable("QMYSQL"))
    {
      error = "Qt MySQL driver is unavailable.";
      return false;
    }

    QString const host = _host->text().trimmed();
    QString const database = _database->text().trimmed();
    QString const user = _user->text().trimmed();
    if (host.isEmpty() || database.isEmpty() || user.isEmpty())
    {
      error = "Enter a server, world database, and user.";
      return false;
    }

    QString const connection_name = QString("npc_spawn_writer_%1").arg(reinterpret_cast<quintptr>(this));
    bool success = false;
    {
      QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", connection_name);
      db.setHostName(host);
      db.setPort(_port->value());
      db.setDatabaseName(database);
      db.setUserName(user);
      db.setPassword(_password->text());
      db.setConnectOptions("MYSQL_OPT_CONNECT_TIMEOUT=5");
      if (!db.open())
      {
        error = db.lastError().text();
      }
      else
      {
        success = [&]() -> bool
        {
          // This path writes one creature row. A fresh connection with session
          // autocommit also works on servers that cannot start SQL transactions.
          QSqlQuery session(db);
          if (!session.exec("SET SESSION autocommit = 1"))
          {
            error = "Could not enable autocommit for the NPC spawn: " + session.lastError().text();
            return false;
          }

          struct ColumnInfo
          {
            QString name;
            bool auto_increment = false;
          };
          QMap<QString, ColumnInfo> columns;
          QSqlQuery schema(db);
          if (!schema.exec("SHOW COLUMNS FROM `creature`"))
          {
            error = "Could not inspect creature: " + schema.lastError().text();
            return false;
          }
          while (schema.next())
          {
            ColumnInfo info;
            info.name = schema.value(0).toString();
            info.auto_increment = schema.value(5).toString().contains(
              "auto_increment", Qt::CaseInsensitive);
            columns.insert(info.name.toCaseFolded(), std::move(info));
          }

          Sql::WorldDatabaseSchema world_schema;
          if (!Sql::inspectWorldDatabaseSchema(db, world_schema, error)) return false;

          QStringList const required = {
            "guid", world_schema.creature_entry_column, "map", "position_x", "position_y",
            "position_z", "orientation"
          };
          for (QString const& name : required)
            if (!columns.contains(name))
            {
              error = QString("Unsupported creature schema: missing %1.").arg(name);
              return false;
            }

          if (_placement_weapon_stance == 1 || _placement_weapon_stance == 2)
          {
            QSqlQuery addon_schema(db);
            if (!addon_schema.exec("SHOW COLUMNS FROM `creature_addon` LIKE 'bytes2'")
                || !addon_schema.next())
            {
              error = "The world database needs creature_addon.bytes2 to save a drawn weapon.";
              return false;
            }
          }

          bool const auto_guid = columns.value("guid").auto_increment;
          bool guid_lock = false;
          if (!auto_guid)
          {
            QSqlQuery lock(db);
            lock.prepare("SELECT GET_LOCK(?, 5)");
            lock.addBindValue(QString("noggit_npc_guid_%1").arg(database).left(64));
            if (!lock.exec() || !lock.next() || lock.value(0).toInt() != 1)
            {
              error = "Could not acquire the creature GUID allocation lock.";
              return false;
            }
            guid_lock = true;
          }

          auto release_guid_lock = [&]
          {
            if (!guid_lock) return;
            QSqlQuery unlock(db);
            unlock.prepare("SELECT RELEASE_LOCK(?)");
            unlock.addBindValue(QString("noggit_npc_guid_%1").arg(database).left(64));
            unlock.exec();
            guid_lock = false;
          };

          QSqlQuery verify(db);
          verify.prepare("SELECT 1 FROM `creature_template` WHERE `entry` = ? LIMIT 1");
          verify.addBindValue(_placement_entry);
          if (!verify.exec() || !verify.next())
          {
            error = verify.lastError().isValid()
              ? "Could not validate the creature template: " + verify.lastError().text()
              : "The selected creature template no longer exists.";
            release_guid_lock();
            return false;
          }

          std::uint64_t allocated_guid = 0;
          if (!auto_guid)
          {
            QSqlQuery next_guid(db);
            if (!next_guid.exec("SELECT COALESCE(MAX(`guid`), 0) + 1 FROM `creature`")
                || !next_guid.next())
            {
              error = "Could not allocate a creature GUID: " + next_guid.lastError().text();
              release_guid_lock();
              return false;
            }
            allocated_guid = next_guid.value(0).toULongLong();
            if (!allocated_guid)
            {
              error = "The database returned an invalid creature GUID.";
              release_guid_lock();
              return false;
            }
          }

          QStringList insert_columns;
          QVector<QVariant> values;
          auto add_value = [&](QString const& requested_name, QVariant const& value) -> bool
          {
            auto const found = columns.constFind(requested_name.toCaseFolded());
            if (found == columns.cend()) return false;
            insert_columns.push_back(found->name);
            values.push_back(value);
            return true;
          };

          if (!auto_guid)
            add_value("guid", QVariant::fromValue<qulonglong>(allocated_guid));
          add_value(world_schema.creature_entry_column, _placement_entry);
          if (world_schema.creature_entry_column.compare("id1", Qt::CaseInsensitive) == 0)
          {
            add_value("id2", 0);
            add_value("id3", 0);
          }
          add_value("map", _map_view->getWorld()->getMapID());
          add_value("zoneId", 0);
          add_value("areaId", 0);
          bool const has_spawn_mask = add_value("spawnMask", 1);
          if (!has_spawn_mask) add_value("spawnDifficulties", QString("0"));
          add_value("phaseMask", _active_phase_mask);
          if (!world_schema.creature_spawn_display_column.isEmpty())
            add_value(world_schema.creature_spawn_display_column, _placement_display_id);
          add_value("equipment_id", _placement_equipment_id);
          add_value("position_x", ZEROPOINT - _placement_position.z);
          add_value("position_y", ZEROPOINT - _placement_position.x);
          add_value("position_z", _placement_position.y);
          add_value("orientation", serverOrientation(_placement_yaw));
          add_value("spawntimesecs", _respawn_seconds->value());
          if (!add_value("wander_distance", 0.0)) add_value("spawndist", 0.0);
          add_value("currentwaypoint", 0);
          add_value("curhealth", 1);
          add_value("curmana", 0);
          add_value("MovementType", 0);

          QStringList escaped_columns;
          QStringList placeholders;
          for (QString column : insert_columns)
          {
            column.replace('`', "``");
            escaped_columns.push_back('`' + column + '`');
            placeholders.push_back("?");
          }
          QSqlQuery insert(db);
          insert.prepare(QString("INSERT INTO `creature` (%1) VALUES (%2)")
                           .arg(escaped_columns.join(", "), placeholders.join(", ")));
          for (QVariant const& value : values) insert.addBindValue(value);
          if (!insert.exec())
          {
            error = insert.lastError().text();
            release_guid_lock();
            return false;
          }

          guid = auto_guid ? insert.lastInsertId().toULongLong() : allocated_guid;
          if (auto_guid && !guid)
          {
            QSqlQuery last_id(db);
            if (last_id.exec("SELECT LAST_INSERT_ID()") && last_id.next())
              guid = last_id.value(0).toULongLong();
          }
          if (!guid)
          {
            // The INSERT has already succeeded. Report that truthfully and stop
            // stamping so a second click cannot silently create a duplicate.
            release_guid_lock();
            return true;
          }
          if (_placement_weapon_stance == 1 || _placement_weapon_stance == 2)
          {
            QString stance_error;
            if (!writeSpawnWeaponStance(db, guid, _placement_entry,
                                        static_cast<unsigned>(_placement_weapon_stance),
                                        stance_error))
              error = stance_error;
          }
          release_guid_lock();
          return true;
        }();
        db.close();
      }
    }
    QSqlDatabase::removeDatabase(connection_name);
    return success;
  }
}
