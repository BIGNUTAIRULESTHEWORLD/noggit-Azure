#include "AdtPorterDialog.hpp"

#include <noggit/MapHeaders.h>
#include <noggit/database/ClientDatabase.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/uid_storage.hpp>
#include <noggit/World.h>

#include <blizzard-archive-library/include/ClientData.hpp>
#include <blizzard-archive-library/include/ClientFile.hpp>
#include <blizzard-archive-library/include/Listfile.hpp>

#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

namespace Noggit::Ui::Windows
{
  class AdtGridWidget final : public QWidget
  {
  public:
    explicit AdtGridWidget(bool source, QWidget* parent = nullptr)
      : QWidget(parent), _source(source)
    {
      setFixedSize(448, 448);
      setMouseTracking(true);
    }

    void setOccupancy(std::array<bool, 4096> occupancy)
    {
      _occupancy = occupancy;
      _selection.clear();
      _footprint.clear();
      _overlay_image = {};
      _overlay_source_tiles.clear();
      _overlay_source_pivot = {-1, -1};
      _overlay_destination_anchor = {-1, -1};
      _anchor = {-1, -1};
      update();
    }

    void setMapImage(QImage image)
    {
      _map_image = std::move(image);
      update();
    }

    std::set<QPoint, bool(*)(QPoint const&, QPoint const&)> const& selection() const
    {
      return _selection;
    }

    QPoint anchor() const { return _anchor; }
    QImage const& mapImage() const { return _map_image; }
    bool occupied(QPoint const& point) const
    {
      return point.x() >= 0 && point.x() < 64 && point.y() >= 0 && point.y() < 64
          && _occupancy[point.y() * 64 + point.x()];
    }

    void setFootprint(std::set<QPoint, bool(*)(QPoint const&, QPoint const&)> footprint,
                      QImage source_image = {}, QPointF source_pivot = {-1, -1},
                      QPoint destination_anchor = {-1, -1},
                      std::set<QPoint, bool(*)(QPoint const&, QPoint const&)> source_tiles
                        = std::set<QPoint, bool(*)(QPoint const&, QPoint const&)>(pointLess),
                      float opacity = .82f)
    {
      _footprint = std::move(footprint);
      _overlay_image = std::move(source_image);
      _overlay_source_pivot = source_pivot;
      _overlay_destination_anchor = destination_anchor;
      _overlay_source_tiles = std::move(source_tiles);
      _overlay_opacity = opacity;
      update();
    }

    std::function<void()> changed;

  protected:
    void paintEvent(QPaintEvent*) override
    {
      QPainter painter(this);
      painter.fillRect(rect(), QColor(38, 40, 45));
      if (!_map_image.isNull())
        painter.drawImage(rect(), _map_image);
      float const cell = width() / 64.f;
      for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
          if (!_occupancy[z * 64 + x])
          {
            QRectF const tile(x * cell, z * cell, cell, cell);
            painter.fillRect(tile.adjusted(.5, .5, -.5, -.5), QColor(54, 56, 62, 235));
          }

      if (!_source && !_overlay_image.isNull() && !_overlay_source_tiles.empty()
          && _overlay_source_pivot.x() >= 0 && _overlay_destination_anchor.x() >= 0)
      {
        float const source_cell_x = static_cast<float>(_overlay_image.width()) / 64.f;
        float const source_cell_y = static_cast<float>(_overlay_image.height()) / 64.f;
        painter.save();
        painter.setOpacity(_overlay_opacity);
        painter.translate((_overlay_destination_anchor.x() + .5f) * cell,
                          (_overlay_destination_anchor.y() + .5f) * cell);
        painter.translate(-_overlay_source_pivot.x() * cell,
                          -_overlay_source_pivot.y() * cell);
        for (QPoint const& source_tile : _overlay_source_tiles)
        {
          QRectF const target(source_tile.x() * cell, source_tile.y() * cell, cell, cell);
          QRectF const source_rect(source_tile.x() * source_cell_x,
                                   source_tile.y() * source_cell_y,
                                   source_cell_x, source_cell_y);
          painter.drawImage(target, _overlay_image, source_rect);
        }
        painter.restore();
      }
      for (int z = 0; z < 64; ++z)
      {
        for (int x = 0; x < 64; ++x)
        {
          QRectF const tile(x * cell, z * cell, cell, cell);
          QPoint const point(x, z);
          if (_footprint.contains(point))
          {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(_occupancy[z * 64 + x] ? QColor(245, 139, 45)
                                                       : QColor(55, 180, 245), 1.5));
            painter.drawRect(tile.adjusted(.5, .5, -.5, -.5));
          }
          if (_source && _selection.contains(point))
          {
            painter.fillRect(tile.adjusted(1, 1, -1, -1), QColor(45, 170, 235, 75));
            painter.setPen(QPen(QColor(125, 215, 255), 1));
            painter.drawRect(tile.adjusted(.5, .5, -.5, -.5));
          }
          else if (!_source && point == _anchor)
          {
            painter.setPen(QPen(Qt::white, 1.5));
            painter.drawRect(tile.adjusted(.5, .5, -.5, -.5));
            painter.drawLine(tile.center() - QPointF(3, 0), tile.center() + QPointF(3, 0));
            painter.drawLine(tile.center() - QPointF(0, 3), tile.center() + QPointF(0, 3));
          }
        }
      }
      painter.setPen(QColor(75, 78, 85));
      for (int i = 0; i <= 64; i += 4)
      {
        painter.drawLine(QPointF(i * cell, 0), QPointF(i * cell, height()));
        painter.drawLine(QPointF(0, i * cell), QPointF(width(), i * cell));
      }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
      if (_source)
      {
        if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
          return;
        _painting = true;
        _paint_add = event->button() == Qt::LeftButton;
        _last_painted = {-1, -1};
        paintSelectionAt(event->localPos());
      }
      else
      {
        if (event->button() != Qt::LeftButton)
          return;
        _placing = true;
        placeAt(event->localPos());
      }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
      if (_source && _painting)
        paintSelectionAt(event->localPos());
      else if (!_source && _placing)
        placeAt(event->localPos());
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
      _painting = false;
      _placing = false;
      _last_painted = {-1, -1};
    }

  private:
    static bool pointLess(QPoint const& lhs, QPoint const& rhs)
    {
      return lhs.y() == rhs.y() ? lhs.x() < rhs.x() : lhs.y() < rhs.y();
    }
    void paintSelectionAt(QPointF const& position)
    {
      int const x = std::clamp(static_cast<int>(position.x() / width() * 64), 0, 63);
      int const z = std::clamp(static_cast<int>(position.y() / height() * 64), 0, 63);
      QPoint const point(x, z);
      bool changed_selection = false;
      int const steps = _last_painted.x() < 0 ? 0
        : std::max(std::abs(point.x() - _last_painted.x()), std::abs(point.y() - _last_painted.y()));
      for (int step = 0; step <= steps; ++step)
      {
        float const amount = steps ? static_cast<float>(step) / steps : 1.f;
        QPoint const sample(
          static_cast<int>(std::lround(std::lerp(static_cast<float>(_last_painted.x()),
                                                 static_cast<float>(point.x()), amount))),
          static_cast<int>(std::lround(std::lerp(static_cast<float>(_last_painted.y()),
                                                 static_cast<float>(point.y()), amount))));
        if (!_occupancy[sample.y() * 64 + sample.x()])
          continue;
        if (_paint_add)
          changed_selection |= _selection.insert(sample).second;
        else
          changed_selection |= _selection.erase(sample) != 0;
      }
      _last_painted = point;
      if (!changed_selection)
        return;
      _anchor = point;
      update();
      if (changed)
        changed();
    }
    void placeAt(QPointF const& position)
    {
      QPoint const point(
        std::clamp(static_cast<int>(position.x() / width() * 64), 0, 63),
        std::clamp(static_cast<int>(position.y() / height() * 64), 0, 63));
      if (point == _anchor)
        return;
      _anchor = point;
      update();
      if (changed)
        changed();
    }
    bool _source = false;
    std::array<bool, 4096> _occupancy{};
    QImage _map_image;
    QImage _overlay_image;
    std::set<QPoint, bool(*)(QPoint const&, QPoint const&)> _overlay_source_tiles{pointLess};
    QPointF _overlay_source_pivot{-1, -1};
    QPoint _overlay_destination_anchor{-1, -1};
    float _overlay_opacity = .82f;
    std::set<QPoint, bool(*)(QPoint const&, QPoint const&)> _selection{pointLess};
    std::set<QPoint, bool(*)(QPoint const&, QPoint const&)> _footprint{pointLess};
    QPoint _anchor{-1, -1};
    bool _painting = false;
    bool _paint_add = true;
    bool _placing = false;
    QPoint _last_painted{-1, -1};
  };
}

namespace
{
  struct ChunkHeader
  {
    std::uint32_t magic;
    std::uint32_t size;
  };

  bool readClientFile(std::shared_ptr<Noggit::Project::NoggitProject> const& project,
                      QString const& logical_path, std::vector<char>& bytes, QString& error)
  {
    try
    {
      BlizzardArchive::ClientFile file(
          BlizzardArchive::Listfile::FileKey(logical_path.toStdString()), project->ClientData.get());
      if (!file.getSize())
      {
        error = QString("File not found: %1").arg(logical_path);
        return false;
      }
      bytes.assign(file.getBuffer(), file.getBuffer() + file.getSize());
      return true;
    }
    catch (std::exception const& exception)
    {
      error = QString("Unable to read %1: %2").arg(logical_path, exception.what());
      return false;
    }
  }

  bool writeFile(QString const& path, std::vector<char> const& bytes, QString& error)
  {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())
        || !file.commit())
    {
      error = QString("Unable to write %1: %2").arg(path, file.errorString());
      return false;
    }
    return true;
  }

  template<typename Visitor>
  bool visitTopLevelChunks(std::vector<char>& bytes, Visitor visitor, QString& error)
  {
    std::size_t position = 0;
    while (position + sizeof(ChunkHeader) <= bytes.size())
    {
      auto* header = reinterpret_cast<ChunkHeader*>(bytes.data() + position);
      std::size_t const data_position = position + sizeof(ChunkHeader);
      std::size_t const end = data_position + header->size;
      if (end > bytes.size())
      {
        error = "ADT contains a truncated top-level chunk.";
        return false;
      }
      visitor(*header, bytes.data() + data_position, header->size);
      position = end;
    }
    if (position != bytes.size())
    {
      error = "ADT contains trailing data outside its chunk structure.";
      return false;
    }
    return true;
  }

  bool transformAdt(std::vector<char>& bytes, int source_x, int source_z,
                    int destination_x, int destination_z, std::uint32_t& next_uid,
                    int& model_count, int& wmo_count, QString& error)
  {
    float const dx = static_cast<float>(destination_x - source_x) * TILESIZE;
    float const dz = static_cast<float>(destination_z - source_z) * TILESIZE;
    int mcnk_count = 0;
    bool valid_version = false;
    bool okay = visitTopLevelChunks(bytes,
      [&](ChunkHeader& chunk, char* data, std::uint32_t size)
      {
        if (chunk.magic == 'MVER' && size == 4)
          valid_version = *reinterpret_cast<std::uint32_t*>(data) == 18;
        else if (chunk.magic == 'MCNK' && size >= sizeof(MapChunkHeader))
        {
          auto* header = reinterpret_cast<MapChunkHeader*>(data);
          header->xpos -= dx;
          header->zpos -= dz;
          ++mcnk_count;
        }
        else if (chunk.magic == 'MDDF' && size % sizeof(ENTRY_MDDF) == 0)
        {
          auto* entries = reinterpret_cast<ENTRY_MDDF*>(data);
          model_count = static_cast<int>(size / sizeof(ENTRY_MDDF));
          for (int i = 0; i < model_count; ++i)
          {
            entries[i].pos[0] += dx;
            entries[i].pos[2] += dz;
            entries[i].uniqueID = ++next_uid;
          }
        }
        else if (chunk.magic == 'MODF' && size % sizeof(ENTRY_MODF) == 0)
        {
          auto* entries = reinterpret_cast<ENTRY_MODF*>(data);
          wmo_count = static_cast<int>(size / sizeof(ENTRY_MODF));
          for (int i = 0; i < wmo_count; ++i)
          {
            entries[i].pos[0] += dx;
            entries[i].pos[2] += dz;
            entries[i].extents[0].x += dx;
            entries[i].extents[0].z += dz;
            entries[i].extents[1].x += dx;
            entries[i].extents[1].z += dz;
            entries[i].uniqueID = ++next_uid;
          }
        }
      }, error);
    if (!okay)
      return false;
    if (!valid_version)
    {
      error = "Only WotLK ADT version 18 is supported.";
      return false;
    }
    if (mcnk_count != 256)
    {
      error = QString("Source ADT contains %1 MCNK chunks instead of 256.").arg(mcnk_count);
      return false;
    }
    return true;
  }

  bool enableWdtTile(std::vector<char>& bytes, int x, int z, QString& error)
  {
    bool found = false;
    bool okay = visitTopLevelChunks(bytes,
      [&](ChunkHeader& chunk, char* data, std::uint32_t size)
      {
        if (chunk.magic != 'MAIN' || size < 64u * 64u * 8u)
          return;
        auto* flags = reinterpret_cast<std::uint32_t*>(data + (z * 64 + x) * 8);
        *flags |= 1u;
        found = true;
      }, error);
    if (okay && !found)
      error = "Destination WDT does not contain a valid MAIN tile table.";
    return okay && found;
  }

  bool disableWdtTile(std::vector<char>& bytes, int x, int z, QString& error)
  {
    bool found = false;
    bool okay = visitTopLevelChunks(bytes,
      [&](ChunkHeader& chunk, char* data, std::uint32_t size)
      {
        if (chunk.magic != 'MAIN' || size < 64u * 64u * 8u)
          return;
        auto* flags = reinterpret_cast<std::uint32_t*>(data + (z * 64 + x) * 8);
        *flags &= ~1u;
        found = true;
      }, error);
    if (okay && !found)
      error = "Destination WDT does not contain a valid MAIN tile table.";
    return okay && found;
  }

  bool wdtOccupancy(std::vector<char> bytes, std::array<bool, 4096>& occupancy, QString& error)
  {
    bool found = false;
    occupancy.fill(false);
    bool okay = visitTopLevelChunks(bytes,
      [&](ChunkHeader& chunk, char* data, std::uint32_t size)
      {
        if (chunk.magic != 'MAIN' || size < 64u * 64u * 8u)
          return;
        for (int index = 0; index < 4096; ++index)
          occupancy[index] = (*reinterpret_cast<std::uint32_t*>(data + index * 8) & 1u) != 0;
        found = true;
      }, error);
    if (okay && !found)
      error = "Map WDT does not contain a valid MAIN tile table.";
    return okay && found;
  }

  QString logicalAdtPath(QString const& directory, int x, int z)
  {
    return QString("World\\Maps\\%1\\%1_%2_%3.adt").arg(directory).arg(x).arg(z);
  }

  QString logicalWdtPath(QString const& directory)
  {
    return QString("World\\Maps\\%1\\%1.wdt").arg(directory);
  }

  QString diskPath(QString const& project_path, QString logical_path)
  {
    logical_path.replace('\\', '/');
    return QDir(project_path).filePath(logical_path);
  }
}

using namespace Noggit::Ui::Windows;

AdtPorterDialog::AdtPorterDialog(std::shared_ptr<Project::NoggitProject> project, QWidget* parent)
  : QDialog(parent), _project(std::move(project))
{
  setWindowTitle("ADT Porting Tool");
  setMinimumWidth(940);
  auto* root = new QVBoxLayout(this);
  auto* help = new QLabel(
      "Paint one or more WotLK ADTs to move between maps—or relocate them on one map—without opening a world. The destination is "
      "written directly to the project after a backup is created.", this);
  help->setWordWrap(true);
  root->addWidget(help);

  auto* maps = new QHBoxLayout();
  auto create_side = [this, maps](QString const& title, bool source, QComboBox*& map,
                                  AdtGridWidget*& grid)
  {
    auto* box = new QGroupBox(title, this);
    auto* layout = new QVBoxLayout(box);
    map = new QComboBox(box);
    grid = new AdtGridWidget(source, box);
    layout->addWidget(map);
    layout->addWidget(grid);
    auto* instructions = new QLabel(source
      ? "Paint with left-click and drag. Erase with right-click and drag."
      : "Click or drag to position the selection's center. The source terrain appears as a ghost.", box);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    maps->addWidget(box);
  };
  create_side("1. Select source ADTs", true, _source_map, _source_grid);
  create_side("2. Choose destination", false, _destination_map, _destination_grid);
  _same_map = new QCheckBox("Use the same map as destination", this);
  _same_map->setToolTip("Move the painted ADTs within the source map and clear their original tile flags.");
  root->addWidget(_same_map);
  auto* transform_controls = new QFormLayout();
  _preview_opacity = new QSlider(Qt::Horizontal, this);
  _preview_opacity->setRange(10, 100);
  _preview_opacity->setValue(82);
  _preview_opacity->setToolTip("Adjust the source terrain ghost over the destination map.");
  transform_controls->addRow("Preview opacity", _preview_opacity);
  root->addLayout(transform_controls);
  root->addLayout(maps);

  auto table = ClientDatabase::getTable("Map");
  auto records = table.Records();
  while (records.HasRecords())
  {
    auto record = records.Next();
    if (!World::IsEditableWorld(record))
      continue;
    QString name = QString::fromUtf8(record.Columns["MapName_lang"].Value.c_str());
    QString directory = QString::fromUtf8(record.Columns["Directory"].Value.c_str());
    QVariantMap data{{"id", static_cast<int>(record.RecordId)}, {"directory", directory}};
    QString label = QString("%1 — %2").arg(record.RecordId).arg(name.isEmpty() ? directory : name);
    _source_map->addItem(label, data);
    _destination_map->addItem(label, data);
  }

  _summary = new QLabel(this);
  _summary->setWordWrap(true);
  root->addWidget(_summary);
  _status = new QLabel(this);
  _status->setWordWrap(true);
  root->addWidget(_status);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto* port = buttons->addButton("Port ADT", QDialogButtonBox::AcceptRole);
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(port, &QPushButton::clicked, this, &AdtPorterDialog::portAdt);
  connect(_source_map, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
  {
    reloadSourceGrid();
    if (_same_map->isChecked())
    {
      if (_destination_map->currentIndex() != index)
        _destination_map->setCurrentIndex(index);
      else
        reloadDestinationGrid();
    }
  });
  connect(_destination_map, qOverload<int>(&QComboBox::currentIndexChanged),
          this, &AdtPorterDialog::reloadDestinationGrid);
  connect(_same_map, &QCheckBox::toggled, this, [this](bool enabled)
  {
    _destination_map->setEnabled(!enabled);
    if (enabled)
    {
      if (_destination_map->currentIndex() != _source_map->currentIndex())
        _destination_map->setCurrentIndex(_source_map->currentIndex());
      else
        reloadDestinationGrid();
    }
    updateSummary();
  });
  connect(_preview_opacity, &QSlider::valueChanged, this,
          [this](int) { updateSummary(); });
  _source_grid->changed = [this] { updateSummary(); };
  _destination_grid->changed = [this] { updateSummary(); };
  reloadSourceGrid();
  reloadDestinationGrid();
  updateSummary();
}

AdtPorterDialog::~AdtPorterDialog() = default;

void AdtPorterDialog::updateSummary()
{
  QVariantMap const source = _source_map->currentData().toMap();
  QVariantMap const destination = _destination_map->currentData().toMap();
  auto const& selection = _source_grid->selection();
  QPoint const anchor = _destination_grid->anchor();
  auto footprint = selection;
  footprint.clear();
  if (selection.empty() || anchor.x() < 0)
  {
    _destination_grid->setFootprint(std::move(footprint));
    _summary->setText("Select one or more source ADTs, then click a destination square.");
    return;
  }
  int min_x = 63;
  int max_x = 0;
  int min_z = 63;
  int max_z = 0;
  for (QPoint const& point : selection)
  {
    min_x = std::min(min_x, point.x());
    max_x = std::max(max_x, point.x());
    min_z = std::min(min_z, point.y());
    max_z = std::max(max_z, point.y());
  }
  QPoint const source_pivot(min_x + (max_x - min_x + 1) / 2,
                            min_z + (max_z - min_z + 1) / 2);
  int collisions = 0;
  bool outside = false;
  for (QPoint const& source_tile : selection)
  {
    QPoint const target = anchor + source_tile - source_pivot;
    if (target.x() < 0 || target.x() >= 64 || target.y() < 0 || target.y() >= 64)
      outside = true;
    else
    {
      footprint.insert(target);
      if (_destination_grid->occupied(target))
        ++collisions;
    }
  }
  _destination_grid->setFootprint(std::move(footprint), _source_grid->mapImage(),
                                  QPointF(source_pivot.x() + .5, source_pivot.y() + .5),
                                  anchor, selection,
                                  _preview_opacity->value() / 100.f);
  _summary->setText(QString("%1 selected source ADT%2 from %3 → %4, centered at %5_%6. %7")
      .arg(selection.size()).arg(selection.size() == 1 ? "" : "s")
      .arg(source.value("directory").toString(), destination.value("directory").toString())
      .arg(anchor.x()).arg(anchor.y())
      .arg(outside ? "The destination footprint extends outside the map."
                   : collisions ? QString("%1 existing destination ADT%2 will be modified.")
                                      .arg(collisions).arg(collisions == 1 ? "" : "s")
                                : "The destination footprint is clear."));
}

void AdtPorterDialog::reloadSourceGrid()
{
  std::array<bool, 4096> occupancy{};
  QVariantMap const map = _source_map->currentData().toMap();
  std::vector<char> wdt;
  QString error;
  if (!map.value("directory").toString().isEmpty()
      && readClientFile(_project, logicalWdtPath(map.value("directory").toString()), wdt, error)
      && wdtOccupancy(std::move(wdt), occupancy, error))
  {
    try
    {
      _source_preview_world = std::make_unique<World>(
        map.value("directory").toString().toStdString(), map.value("id").toInt(),
        Noggit::NoggitRenderContext::MAP_VIEW);
      _source_grid->setMapImage(_source_preview_world->horizon._qt_minimap);
      _status->clear();
    }
    catch (std::exception const& exception)
    {
      _source_preview_world.reset();
      _source_grid->setMapImage({});
      _status->setText(QString("Unable to build source map preview: %1").arg(exception.what()));
    }
  }
  else
  {
    _source_preview_world.reset();
    _source_grid->setMapImage({});
    _status->setText(error);
  }
  _source_grid->setOccupancy(occupancy);
  updateSummary();
}

void AdtPorterDialog::reloadDestinationGrid()
{
  std::array<bool, 4096> occupancy{};
  QVariantMap const map = _destination_map->currentData().toMap();
  std::vector<char> wdt;
  QString error;
  if (!map.value("directory").toString().isEmpty()
      && readClientFile(_project, logicalWdtPath(map.value("directory").toString()), wdt, error)
      && wdtOccupancy(std::move(wdt), occupancy, error))
  {
    try
    {
      _destination_preview_world = std::make_unique<World>(
        map.value("directory").toString().toStdString(), map.value("id").toInt(),
        Noggit::NoggitRenderContext::MAP_VIEW);
      _destination_grid->setMapImage(_destination_preview_world->horizon._qt_minimap);
      _status->clear();
    }
    catch (std::exception const& exception)
    {
      _destination_preview_world.reset();
      _destination_grid->setMapImage({});
      _status->setText(QString("Unable to build destination map preview: %1").arg(exception.what()));
    }
  }
  else
  {
    _destination_preview_world.reset();
    _destination_grid->setMapImage({});
    _status->setText(error);
  }
  _destination_grid->setOccupancy(occupancy);
  updateSummary();
}

void AdtPorterDialog::portAdt()
{
  QVariantMap const source = _source_map->currentData().toMap();
  QVariantMap const destination = _destination_map->currentData().toMap();
  QString const source_dir = source.value("directory").toString();
  QString const destination_dir = destination.value("directory").toString();
  int const destination_id = destination.value("id").toInt();
  bool const relocating_on_same_map = source_dir == destination_dir;
  auto const& selection = _source_grid->selection();
  QPoint const anchor = _destination_grid->anchor();
  if (source_dir.isEmpty() || destination_dir.isEmpty() || selection.empty() || anchor.x() < 0)
  {
    _status->setText("Select source ADTs and a destination location first.");
    return;
  }
  int min_x = 63;
  int max_x = 0;
  int min_z = 63;
  int max_z = 0;
  for (QPoint const& point : selection)
  {
    min_x = std::min(min_x, point.x());
    max_x = std::max(max_x, point.x());
    min_z = std::min(min_z, point.y());
    max_z = std::max(max_z, point.y());
  }
  QPoint const source_pivot(min_x + (max_x - min_x + 1) / 2,
                            min_z + (max_z - min_z + 1) / 2);
  for (QPoint const& point : selection)
  {
    QPoint const target = anchor + point - source_pivot;
    if (target.x() < 0 || target.x() >= 64 || target.y() < 0 || target.y() >= 64)
    {
      _status->setText("The destination footprint extends outside the 64×64 map grid.");
      return;
    }
  }
  if (QMessageBox::warning(this, relocating_on_same_map ? "Shift ADTs" : "Port ADTs",
      QString("%1 %2 selected ADT%3 from %4 %5 %6, centered at %7_%8?\n\n"
              "Existing ADTs inside the destination footprint will be replaced. A backup will be created first. "
              "The destination WDL is not regenerated in this test build.")
        .arg(relocating_on_same_map ? "Move" : "Port")
        .arg(selection.size()).arg(selection.size() == 1 ? "" : "s")
        .arg(source_dir).arg(relocating_on_same_map ? "within" : "into").arg(destination_dir)
        .arg(anchor.x()).arg(anchor.y()),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    return;

  QString error;
  std::vector<char> wdt;
  QString const destination_wdt = logicalWdtPath(destination_dir);
  if (!readClientFile(_project, destination_wdt, wdt, error))
  {
    _status->setText(error);
    return;
  }

  QSettings settings;
  if (settings.value("project/mysql/enabled", false).toBool())
  {
    _status->setText("This test build cannot safely update MySQL-backed UIDs. Disable MySQL UID "
                     "storage or wait for the database-aware porting pass.");
    return;
  }
  if (!uid_storage::hasMaxUIDStored(destination_id))
  {
    _status->setText("The destination map has no saved maximum UID. Open it once and complete the "
                     "UID check before using the ADT Porting Tool.");
    return;
  }

  std::vector<char> const original_wdt = wdt;
  std::uint32_t next_uid = uid_storage::getMaxUID(destination_id);
  struct Transfer
  {
    QPoint source;
    QPoint destination;
    QString logical_path;
    std::vector<char> transformed;
    std::vector<char> original;
    bool had_original = false;
  };
  std::vector<Transfer> transfers;
  transfers.reserve(selection.size());
  int models = 0;
  int wmos = 0;
  std::set<int> destination_indices;
  for (QPoint const& source_tile : selection)
  {
    QPoint const destination_tile = anchor + source_tile - source_pivot;
    destination_indices.insert(destination_tile.y() * 64 + destination_tile.x());
    Transfer transfer{source_tile, destination_tile,
      logicalAdtPath(destination_dir, destination_tile.x(), destination_tile.y())};
    QString const source_path = logicalAdtPath(source_dir, source_tile.x(), source_tile.y());
    int tile_models = 0;
    int tile_wmos = 0;
    if (!readClientFile(_project, source_path, transfer.transformed, error)
        || !transformAdt(transfer.transformed, source_tile.x(), source_tile.y(),
                        destination_tile.x(), destination_tile.y(), next_uid,
                        tile_models, tile_wmos, error)
        || !enableWdtTile(wdt, destination_tile.x(), destination_tile.y(), error))
    {
      _status->setText(error);
      return;
    }
    models += tile_models;
    wmos += tile_wmos;
    QString ignored;
    transfer.had_original = readClientFile(_project, transfer.logical_path, transfer.original, ignored);
    transfers.emplace_back(std::move(transfer));
  }
  if (relocating_on_same_map)
  {
    for (QPoint const& source_tile : selection)
    {
      if (!destination_indices.contains(source_tile.y() * 64 + source_tile.x())
          && !disableWdtTile(wdt, source_tile.x(), source_tile.y(), error))
      {
        _status->setText(error);
        return;
      }
    }
  }

  QString const stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
  QString const backup_dir = QDir(QString::fromStdString(_project->ProjectPath))
      .filePath(QString("noggit-backups/adt-port/%1").arg(stamp));
  if (!QDir().mkpath(backup_dir))
  {
    _status->setText("Unable to create the ADT port backup directory.");
    return;
  }
  if (!writeFile(QDir(backup_dir).filePath("destination-original.wdt"), original_wdt, error))
  {
    _status->setText(error);
    return;
  }
  for (Transfer const& transfer : transfers)
  {
    if (transfer.had_original
        && !writeFile(QDir(backup_dir).filePath(
              QString("destination-%1_%2-original.adt")
                .arg(transfer.destination.x()).arg(transfer.destination.y())),
            transfer.original, error))
    {
      _status->setText(error);
      return;
    }
  }

  QString const project_path = QString::fromStdString(_project->ProjectPath);
  QString const destination_wdt_disk = diskPath(QString::fromStdString(_project->ProjectPath), destination_wdt);
  std::size_t written = 0;
  for (; written < transfers.size(); ++written)
  {
    QString const path = diskPath(project_path, transfers[written].logical_path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!writeFile(path, transfers[written].transformed, error))
      break;
  }
  if (written != transfers.size() || !writeFile(destination_wdt_disk, wdt, error))
  {
    QString restore_error;
    for (std::size_t i = 0; i < written; ++i)
    {
      QString const path = diskPath(project_path, transfers[i].logical_path);
      if (transfers[i].had_original)
        writeFile(path, transfers[i].original, restore_error);
      else
        QFile::remove(path);
    }
    _status->setText(error + (restore_error.isEmpty() ? " The ADT writes were rolled back."
                                                      : " Automatic ADT rollback failed: " + restore_error)
                     + QString(" Backup: %1").arg(backup_dir));
    return;
  }
  uid_storage::saveMaxUID(destination_id, next_uid);
  _status->setStyleSheet("color: #5cb85c;");
  _status->setText(QString("Port complete: %1 ADTs (%2 chunks), %3 M2 records, %4 WMO records. Backup: %5")
                       .arg(transfers.size()).arg(transfers.size() * 256)
                       .arg(models).arg(wmos).arg(backup_dir));
  reloadDestinationGrid();
}
