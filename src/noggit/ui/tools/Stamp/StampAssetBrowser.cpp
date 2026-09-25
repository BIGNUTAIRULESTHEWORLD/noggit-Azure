// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "StampAssetBrowser.hpp"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>
#include <QWaitCondition>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

using namespace Noggit::Ui::Tools::Stamp;

namespace
{
  constexpr int stamp_path_role = Qt::UserRole;
  constexpr int stamp_shape_role = Qt::UserRole + 1;
  constexpr int stamp_valid_role = Qt::UserRole + 2;

  QString shapeName(MapStampShape shape)
  {
    switch (shape)
    {
      case MapStampShape::Circle:
        return "Circle";
      case MapStampShape::Square:
        return "Square";
      case MapStampShape::Painted:
        return "Painted";
    }
    return "Unknown";
  }

  QString sanitizeStampName(QString name)
  {
    name = name.trimmed();
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1F]"), "_");
    while (name.endsWith('.') || name.endsWith(' '))
      name.chop(1);
    return name;
  }
}

class StampAssetBrowser::PreviewLoader final : public QThread
{
public:
  explicit PreviewLoader(StampAssetBrowser* browser)
  : QThread(browser), _browser(browser)
  {
  }

  void submit(QStringList paths, quint64 generation)
  {
    QMutexLocker const lock(&_mutex);
    _paths = std::move(paths);
    _generation = generation;
    _paused = false;
    _wake.wakeAll();
  }

  void prioritize(QString const& path)
  {
    QMutexLocker const lock(&_mutex);
    if (_paths.removeAll(path) > 0)
      _paths.prepend(path);
  }

  void pause()
  {
    QMutexLocker const lock(&_mutex);
    _paused = true;
  }

  void resume()
  {
    QMutexLocker const lock(&_mutex);
    _paused = false;
    _wake.wakeAll();
  }

  void stopAndWait()
  {
    {
      QMutexLocker const lock(&_mutex);
      _stopping = true;
      _wake.wakeAll();
    }
    wait();
  }

protected:
  void run() override
  {
    for (;;)
    {
      QString path;
      quint64 generation = 0;
      {
        QMutexLocker const lock(&_mutex);
        while (!_stopping && (_paused || _paths.isEmpty()))
          _wake.wait(&_mutex);
        if (_stopping)
          return;
        path = _paths.takeFirst();
        generation = _generation;
      }

      Metadata const metadata = StampAssetBrowser::readMetadata(QFileInfo(path));
      QMetaObject::invokeMethod(_browser,
          [browser = _browser, path, metadata, generation]
          { browser->previewLoaded(path, metadata, generation); },
          Qt::QueuedConnection);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

private:
  StampAssetBrowser* _browser;
  QMutex _mutex;
  QWaitCondition _wake;
  QStringList _paths;
  quint64 _generation = 0;
  bool _paused = false;
  bool _stopping = false;
};

StampAssetBrowser::StampAssetBrowser(QString directory_path, QString active_path, QWidget* parent)
: QWidget(parent)
, _directory_path(QDir::cleanPath(std::move(directory_path)))
, _active_path(active_path.isEmpty() ? QString{} : QDir::cleanPath(active_path))
{
  setWindowTitle("Stamp Asset Browser");
  resize(900, 600);
  setMinimumSize(440, 340);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(10, 10, 10, 10);
  root->setSpacing(8);

  auto* search_row = new QHBoxLayout();
  _search = new QLineEdit(this);
  _search->setClearButtonEnabled(true);
  _search->setPlaceholderText("Search saved stamps...");
  _shape_filter = new QComboBox(this);
  _shape_filter->addItem("All footprints", -1);
  _shape_filter->addItem("Circle", static_cast<int>(MapStampShape::Circle));
  _shape_filter->addItem("Square", static_cast<int>(MapStampShape::Square));
  _shape_filter->addItem("Painted", static_cast<int>(MapStampShape::Painted));
  search_row->addWidget(_search, 1);
  search_row->addWidget(_shape_filter);
  root->addLayout(search_row);

  _splitter = new QSplitter(Qt::Horizontal, this);
  _items = new QListWidget(_splitter);
  _items->setViewMode(QListView::IconMode);
  _items->setMovement(QListView::Static);
  _items->setResizeMode(QListView::Adjust);
  _items->setSelectionMode(QAbstractItemView::SingleSelection);
  _items->setIconSize(QSize(112, 112));
  _items->setGridSize(QSize(148, 150));
  _items->setSpacing(5);
  _items->setWordWrap(true);
  _items->setUniformItemSizes(true);

  _details_panel = new QFrame(_splitter);
  _details_panel->setFrameShape(QFrame::StyledPanel);
  _details_panel->setMinimumWidth(250);
  _details_panel->setMaximumWidth(330);
  _details_layout = new QBoxLayout(QBoxLayout::TopToBottom, _details_panel);
  _preview = new QLabel(_details_panel);
  _preview->setAlignment(Qt::AlignCenter);
  _preview->setMinimumSize(180, 180);
  _preview->setMaximumSize(240, 240);
  _preview->setFrameShape(QFrame::StyledPanel);
  auto* details_text = new QWidget(_details_panel);
  auto* text_layout = new QVBoxLayout(details_text);
  text_layout->setContentsMargins(0, 0, 0, 0);
  _name = new QLabel("No stamp selected", details_text);
  QFont name_font = _name->font();
  name_font.setBold(true);
  name_font.setPointSize(name_font.pointSize() + 2);
  _name->setFont(name_font);
  _name->setWordWrap(true);
  _details = new QLabel(details_text);
  _details->setWordWrap(true);
  _details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  text_layout->addWidget(_name);
  text_layout->addWidget(_details);
  text_layout->addStretch(1);
  _details_layout->addWidget(_preview, 0, Qt::AlignHCenter);
  _details_layout->addWidget(details_text, 1);

  _splitter->addWidget(_items);
  _splitter->addWidget(_details_panel);
  _splitter->setStretchFactor(0, 1);
  _splitter->setStretchFactor(1, 0);
  root->addWidget(_splitter, 1);

  auto* actions = new QHBoxLayout();
  _rename = new QPushButton("Rename", this);
  _remove = new QPushButton("Delete", this);
  _use = new QPushButton("Use stamp", this);
  actions->addWidget(_rename);
  actions->addWidget(_remove);
  actions->addStretch(1);
  actions->addWidget(_use);
  root->addLayout(actions);

  _preview_loader = new PreviewLoader(this);
  _preview_loader->start(QThread::LowPriority);

  connect(_search, &QLineEdit::textChanged, this,
          [this] { applyFilters(); });
  connect(_shape_filter, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { applyFilters(); });
  connect(_items, &QListWidget::currentItemChanged, this,
          [this]
  {
    updateSelection();
    QListWidgetItem* const item = _items->currentItem();
    QString const path = item ? item->data(stamp_path_role).toString() : QString{};
    if (_pending_paths.removeAll(path) > 0)
    {
      _pending_paths.prepend(path);
      _preview_loader->prioritize(path);
    }
  });
  connect(_items, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem* item)
  {
    if (item && item->data(stamp_valid_role).toBool())
      chooseCurrent();
  });
  connect(_rename, &QPushButton::clicked, this,
          [this] { renameCurrent(); });
  connect(_remove, &QPushButton::clicked, this,
          [this] { deleteCurrent(); });
  connect(_use, &QPushButton::clicked, this,
          [this] { chooseCurrent(); });

  updateSelection();
}

StampAssetBrowser::~StampAssetBrowser()
{
  _preview_loader->stopAndWait();
}

void StampAssetBrowser::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  updateResponsiveLayout();
}

void StampAssetBrowser::updateResponsiveLayout()
{
  if (!_splitter)
    return;
  bool const compact = width() < 760;
  if (compact == _compact_layout)
    return;
  _compact_layout = compact;

  if (compact)
  {
    _details_panel->setMinimumWidth(0);
    _details_panel->setMaximumWidth(QWIDGETSIZE_MAX);
    _details_panel->setMinimumHeight(140);
    _details_panel->setMaximumHeight(190);
    _details_layout->setDirection(QBoxLayout::LeftToRight);
    _preview->setMinimumSize(120, 120);
    _preview->setMaximumSize(140, 140);
    _items->setIconSize(QSize(96, 96));
    _items->setGridSize(QSize(126, 132));
    _splitter->setOrientation(Qt::Vertical);
    _splitter->setSizes({std::max(100, height() - 190), 170});
  }
  else
  {
    _details_panel->setMinimumHeight(0);
    _details_panel->setMaximumHeight(QWIDGETSIZE_MAX);
    _details_panel->setMinimumWidth(250);
    _details_panel->setMaximumWidth(330);
    _details_layout->setDirection(QBoxLayout::TopToBottom);
    _preview->setMinimumSize(180, 180);
    _preview->setMaximumSize(240, 240);
    _items->setIconSize(QSize(112, 112));
    _items->setGridSize(QSize(148, 150));
    _splitter->setOrientation(Qt::Horizontal);
    _splitter->setSizes({std::max(300, width() - 300), 300});
  }
  updateSelection();
}

void StampAssetBrowser::refresh(QString const& preferred_path)
{
  QString const current_path = _items->currentItem()
      ? _items->currentItem()->data(stamp_path_role).toString() : QString{};
  populate(preferred_path.isEmpty()
      ? (current_path.isEmpty() ? _active_path : current_path) : preferred_path);
  _preview_loader->resume();
}

void StampAssetBrowser::pauseLoading()
{
  _preview_loader->pause();
}

void StampAssetBrowser::invalidate(QString const& path)
{
  _cache.remove(QDir::cleanPath(path));
  _needs_refresh = true;
}

void StampAssetBrowser::setActivePath(QString const& path)
{
  _active_path = path.isEmpty() ? QString{} : QDir::cleanPath(path);
  for (int index = 0; index < _items->count(); ++index)
  {
    QListWidgetItem* const item = _items->item(index);
    QFont font = item->font();
    font.setBold(item->data(stamp_path_role).toString().compare(
        _active_path, Qt::CaseInsensitive) == 0 && !_active_path.isEmpty());
    item->setFont(font);
  }
  updateSelection();
}

void StampAssetBrowser::populate(QString const& preferred_path, int preferred_index)
{
  QDir const directory(_directory_path);
  QFileInfoList const assets = directory.entryInfoList(
      {"*.nogstamp"}, QDir::Files, QDir::Name | QDir::IgnoreCase);

  bool unchanged = _populated && !_needs_refresh
      && assets.size() == _listed_files.size();
  if (unchanged)
  {
    for (QFileInfo const& asset_info : assets)
    {
      QString const path = QDir::cleanPath(asset_info.absoluteFilePath());
      auto const listed = _listed_files.constFind(path);
      if (listed == _listed_files.cend()
          || listed->first != asset_info.lastModified()
          || listed->second != asset_info.size())
      {
        unchanged = false;
        break;
      }
    }
  }
  if (unchanged)
  {
    selectPreferred(preferred_path, preferred_index);
    return;
  }

  ++_generation;
  _pending_paths.clear();
  _item_by_path.clear();
  _metadata.clear();
  _listed_files.clear();
  _populated = true;
  _needs_refresh = false;
  _items->setUpdatesEnabled(false);
  {
    QSignalBlocker const items_blocker(_items);
    _items->clear();
    for (QFileInfo const& asset_info : assets)
    {
      QString const path = QDir::cleanPath(asset_info.absoluteFilePath());
      _listed_files.insert(path, {asset_info.lastModified(), asset_info.size()});
      Metadata metadata;
      auto const cached = _cache.constFind(path);
      if (cached != _cache.cend() && cached->modified == asset_info.lastModified()
          && cached->size == asset_info.size())
        metadata = *cached;
      else
      {
        metadata.loading = true;
        metadata.modified = asset_info.lastModified();
        metadata.size = asset_info.size();
        _pending_paths.append(path);
      }

      auto* item = new QListWidgetItem(asset_info.completeBaseName(), _items);
      item->setData(stamp_path_role, path);
      item->setTextAlignment(Qt::AlignHCenter);
      _item_by_path.insert(path, item);
      _metadata.insert(path, metadata);
      applyMetadata(item, path, metadata);
    }

    if (_items->count() == 0)
    {
      auto* empty = new QListWidgetItem("No saved stamps", _items);
      empty->setFlags(Qt::NoItemFlags);
      empty->setTextAlignment(Qt::AlignCenter);
    }
  }
  _items->setUpdatesEnabled(true);

  setActivePath(_active_path);
  applyFilters();
  selectPreferred(preferred_path, preferred_index);
  _preview_loader->submit(_pending_paths, _generation);
}

StampAssetBrowser::Metadata StampAssetBrowser::readMetadata(QFileInfo const& asset_info)
{
  Metadata metadata;
  metadata.modified = asset_info.lastModified();
  metadata.size = asset_info.size();
  MapStampAsset asset;
  metadata.valid = asset.load(asset_info.absoluteFilePath(), &metadata.error);
  if (metadata.valid)
  {
    metadata.preview = asset.previewImage();
    metadata.shape = asset.shape();
    metadata.details = QString(
        "%1 footprint\nRadius: %2\nTerrain: %3 x %3\nTexture: %4 x %4\nLayers: %5\n%6")
        .arg(shapeName(asset.shape()))
        .arg(asset.sourceRadius(), 0, 'f', 1)
        .arg(asset.heightResolution())
        .arg(asset.textureResolution())
        .arg(static_cast<qulonglong>(asset.textureCount()))
        .arg(asset.supportsExactHeight()
             ? "Exact, Mountain, and Terrain modes"
             : "Legacy Terrain Conform only");
  }
  return metadata;
}

void StampAssetBrowser::applyMetadata(QListWidgetItem* item, QString const& path,
                                      Metadata const& metadata)
{
  item->setData(stamp_shape_role, static_cast<int>(metadata.shape));
  item->setData(stamp_valid_role, metadata.valid);
  if (metadata.loading)
  {
    item->setToolTip("Loading stamp preview...");
    item->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
  }
  else if (metadata.valid)
  {
    item->setToolTip(metadata.details + "\n\n" + path);
    QPixmap const thumbnail = QPixmap::fromImage(metadata.preview).scaled(
        _items->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    item->setIcon(QIcon(thumbnail));
  }
  else
  {
    item->setToolTip(QString("Unable to load this stamp:\n%1\n\n%2")
        .arg(metadata.error, path));
    item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
  }
}

void StampAssetBrowser::previewLoaded(QString const& path, Metadata const& metadata,
                                      quint64 generation)
{
  if (generation != _generation)
    return;
  _pending_paths.removeAll(path);
  QListWidgetItem* const item = _item_by_path.value(path, nullptr);
  if (item)
  {
    bool const selected = _items->currentItem() == item;
    _cache.insert(path, metadata);
    _metadata.insert(path, metadata);
    applyMetadata(item, path, metadata);
    item->setHidden(!matchesFilters(item));
    if (selected && item->isHidden())
      _items->setCurrentItem(nullptr);
    if (!_items->currentItem())
      selectPreferred({}, -1);
    if (selected)
      updateSelection();
  }
}

void StampAssetBrowser::selectPreferred(QString const& preferred_path, int preferred_index)
{
  if (preferred_path.isEmpty() && preferred_index < 0
      && _items->currentItem() && !_items->currentItem()->isHidden())
    return;
  int selection = -1;
  QString const clean_preferred = preferred_path.isEmpty()
      ? QString{} : QDir::cleanPath(preferred_path);
  for (int index = 0; index < _items->count(); ++index)
  {
    if (_items->item(index)->data(stamp_path_role).toString() == clean_preferred)
    {
      selection = index;
      break;
    }
  }
  if (selection < 0 && preferred_index >= 0)
    selection = std::min(preferred_index, _items->count() - 1);
  if (selection < 0)
  {
    for (int index = 0; index < _items->count(); ++index)
    {
      if (!_items->item(index)->isHidden()
          && !_items->item(index)->data(stamp_path_role).toString().isEmpty())
      {
        selection = index;
        break;
      }
    }
  }
  if (selection >= 0 && !_items->item(selection)->isHidden())
    _items->setCurrentRow(selection);
  else
    updateSelection();
}

bool StampAssetBrowser::matchesFilters(QListWidgetItem const* item) const
{
  QString const path = item->data(stamp_path_role).toString();
  if (path.isEmpty())
    return true;
  QString const query = _search->text().trimmed();
  int const shape = _shape_filter->currentData().toInt();
  auto const metadata = _metadata.constFind(path);
  return (query.isEmpty() || item->text().contains(query, Qt::CaseInsensitive))
      && (shape < 0 || (metadata != _metadata.cend() && metadata->loading)
          || (item->data(stamp_valid_role).toBool()
              && item->data(stamp_shape_role).toInt() == shape));
}

void StampAssetBrowser::applyFilters()
{
  QListWidgetItem* const current = _items->currentItem();
  for (int index = 0; index < _items->count(); ++index)
  {
    QListWidgetItem* const item = _items->item(index);
    item->setHidden(!matchesFilters(item));
  }
  if (current && current->isHidden())
    _items->setCurrentItem(nullptr);
  if (!_items->currentItem())
  {
    for (int index = 0; index < _items->count(); ++index)
    {
      if (!_items->item(index)->isHidden())
      {
        _items->setCurrentRow(index);
        break;
      }
    }
  }
  updateSelection();
}

void StampAssetBrowser::updateSelection()
{
  QListWidgetItem* const item = _items->currentItem();
  QString const path = item ? item->data(stamp_path_role).toString() : QString{};
  bool const has_item = !path.isEmpty();
  bool const valid = has_item && item->data(stamp_valid_role).toBool();
  bool const loading = has_item && _metadata.value(path).loading;
  _rename->setEnabled(has_item && !loading);
  _remove->setEnabled(has_item && !loading);
  _use->setEnabled(valid);

  if (!has_item)
  {
    _preview->clear();
    _preview->setText(_metadata.isEmpty() ? "No saved stamps" : "No matching stamps");
    _name->setText("No stamp selected");
    _details->setText("Capture a terrain stamp to add it to this project library.");
    return;
  }

  Metadata const metadata = _metadata.value(path);
  _name->setText(item->text() +
      (path.compare(_active_path, Qt::CaseInsensitive) == 0 ? " (active)" : ""));
  if (metadata.loading)
  {
    _preview->clear();
    _preview->setText("Loading preview...");
    _details->setText("This stamp is being loaded from the project library.");
    return;
  }
  if (!metadata.valid)
  {
    _preview->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(96, 96));
    _details->setText(QString("Unable to load this stamp.\n\n%1").arg(metadata.error));
    return;
  }
  int const preview_size = _compact_layout ? 120 : 220;
  _preview->setPixmap(QPixmap::fromImage(metadata.preview).scaled(
      preview_size, preview_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  _details->setText(metadata.details);
}

void StampAssetBrowser::chooseCurrent()
{
  QListWidgetItem* const item = _items->currentItem();
  if (!item || !item->data(stamp_valid_role).toBool())
    return;
  emit stampChosen(item->data(stamp_path_role).toString());
}

void StampAssetBrowser::renameCurrent()
{
  QListWidgetItem* const item = _items->currentItem();
  if (!item)
    return;
  QString const old_path = item->data(stamp_path_role).toString();
  if (old_path.isEmpty())
    return;
  bool accepted = false;
  QString const name = sanitizeStampName(QInputDialog::getText(
      this, "Rename map stamp", "Stamp name:", QLineEdit::Normal,
      QFileInfo(old_path).completeBaseName(), &accepted));
  if (!accepted || name.isEmpty())
    return;
  QString const new_path = QDir(_directory_path).filePath(name + ".nogstamp");
  if (QDir::cleanPath(new_path).compare(old_path, Qt::CaseInsensitive) == 0)
    return;
  if (QFileInfo::exists(new_path))
  {
    QMessageBox::warning(this, "Rename map stamp",
                         QString("A stamp named '%1' already exists.").arg(name));
    return;
  }
  if (!QFile::rename(old_path, new_path))
  {
    QMessageBox::warning(this, "Rename map stamp",
                         QString("Unable to rename '%1'.").arg(item->text()));
    return;
  }
  if (_active_path.compare(old_path, Qt::CaseInsensitive) == 0)
    _active_path = QDir::cleanPath(new_path);
  invalidate(old_path);
  populate(new_path);
  emit libraryChanged(_active_path);
}

void StampAssetBrowser::deleteCurrent()
{
  QListWidgetItem* const item = _items->currentItem();
  if (!item)
    return;
  QString const path = item->data(stamp_path_role).toString();
  if (path.isEmpty())
    return;
  QString const name = item->text();
  if (QMessageBox::question(this, "Delete map stamp",
        QString("Delete '%1' from the project stamp library?").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    return;
  int const previous_index = _items->row(item);
  if (!QFile::remove(path))
  {
    QMessageBox::warning(this, "Delete map stamp",
                         QString("Unable to delete '%1'.").arg(name));
    return;
  }
  bool const deleted_active = _active_path.compare(path, Qt::CaseInsensitive) == 0;
  if (deleted_active)
    _active_path.clear();
  invalidate(path);
  populate({}, previous_index);
  if (deleted_active)
  {
    int const count = _items->count();
    for (int offset = 0; offset < count; ++offset)
    {
      int const index = (previous_index + offset) % count;
      QListWidgetItem* const candidate = _items->item(index);
      if (!candidate->data(stamp_valid_role).toBool())
        continue;
      _active_path = candidate->data(stamp_path_role).toString();
      if (!candidate->isHidden())
        _items->setCurrentItem(candidate);
      break;
    }
    setActivePath(_active_path);
  }
  emit libraryChanged(_active_path);
}
