// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "StampAssetBrowser.hpp"

#include <QAbstractItemView>
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
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
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

StampAssetBrowser::StampAssetBrowser(QString directory_path, QString active_path, QWidget* parent)
: QDialog(parent)
, _directory_path(QDir::cleanPath(std::move(directory_path)))
, _active_path(active_path.isEmpty() ? QString{} : QDir::cleanPath(active_path))
{
  setWindowTitle("Stamp Asset Browser");
  setModal(true);
  resize(900, 600);
  setMinimumSize(700, 460);

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

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  _items = new QListWidget(splitter);
  _items->setViewMode(QListView::IconMode);
  _items->setMovement(QListView::Static);
  _items->setResizeMode(QListView::Adjust);
  _items->setSelectionMode(QAbstractItemView::SingleSelection);
  _items->setIconSize(QSize(112, 112));
  _items->setGridSize(QSize(148, 150));
  _items->setSpacing(5);
  _items->setWordWrap(true);
  _items->setUniformItemSizes(true);

  auto* details_panel = new QFrame(splitter);
  details_panel->setFrameShape(QFrame::StyledPanel);
  details_panel->setMinimumWidth(250);
  details_panel->setMaximumWidth(330);
  auto* details_layout = new QVBoxLayout(details_panel);
  _preview = new QLabel(details_panel);
  _preview->setAlignment(Qt::AlignCenter);
  _preview->setMinimumSize(220, 220);
  _preview->setFrameShape(QFrame::StyledPanel);
  _name = new QLabel("No stamp selected", details_panel);
  QFont name_font = _name->font();
  name_font.setBold(true);
  name_font.setPointSize(name_font.pointSize() + 2);
  _name->setFont(name_font);
  _name->setWordWrap(true);
  _details = new QLabel(details_panel);
  _details->setWordWrap(true);
  _details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  details_layout->addWidget(_preview);
  details_layout->addWidget(_name);
  details_layout->addWidget(_details);
  details_layout->addStretch(1);

  splitter->addWidget(_items);
  splitter->addWidget(details_panel);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 0);
  root->addWidget(splitter, 1);

  auto* actions = new QHBoxLayout();
  _rename = new QPushButton("Rename", this);
  _remove = new QPushButton("Delete", this);
  _cancel = new QPushButton("Cancel", this);
  _use = new QPushButton("Use stamp", this);
  _use->setDefault(true);
  actions->addWidget(_rename);
  actions->addWidget(_remove);
  actions->addStretch(1);
  actions->addWidget(_cancel);
  actions->addWidget(_use);
  root->addLayout(actions);

  connect(_search, &QLineEdit::textChanged, this,
          [this] { applyFilters(); });
  connect(_shape_filter, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { applyFilters(); });
  connect(_items, &QListWidget::currentItemChanged, this,
          [this] { updateSelection(); });
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
  connect(_cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(_use, &QPushButton::clicked, this,
          [this] { chooseCurrent(); });

  populate(_active_path);
}

QString StampAssetBrowser::selectedPath() const
{
  return _selected_path;
}

QString StampAssetBrowser::activePath() const
{
  return _active_path;
}

void StampAssetBrowser::populate(QString const& preferred_path, int preferred_index)
{
  _items->clear();
  _metadata.clear();

  QDir const directory(_directory_path);
  QFileInfoList const assets = directory.entryInfoList(
      {"*.nogstamp"}, QDir::Files, QDir::Name | QDir::IgnoreCase);
  for (QFileInfo const& asset_info : assets)
  {
    QString const path = QDir::cleanPath(asset_info.absoluteFilePath());
    MapStampAsset asset;
    QString error;
    Metadata metadata;
    metadata.valid = asset.load(path, &error);
    metadata.error = error;
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

    auto* item = new QListWidgetItem(asset_info.completeBaseName(), _items);
    item->setData(stamp_path_role, path);
    item->setData(stamp_shape_role, static_cast<int>(metadata.shape));
    item->setData(stamp_valid_role, metadata.valid);
    item->setTextAlignment(Qt::AlignHCenter);
    item->setToolTip(metadata.valid
        ? metadata.details + "\n\n" + path
        : QString("Unable to load this stamp:\n%1\n\n%2").arg(error, path));
    if (metadata.valid)
    {
      QPixmap const thumbnail = QPixmap::fromImage(metadata.preview).scaled(
          _items->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
      item->setIcon(QIcon(thumbnail));
    }
    else
      item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    _metadata.insert(path, std::move(metadata));
  }

  if (_items->count() == 0)
  {
    auto* empty = new QListWidgetItem("No saved stamps", _items);
    empty->setFlags(Qt::NoItemFlags);
    empty->setTextAlignment(Qt::AlignCenter);
  }

  applyFilters();
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

void StampAssetBrowser::applyFilters()
{
  QString const query = _search->text().trimmed();
  int const shape = _shape_filter->currentData().toInt();
  QListWidgetItem* const current = _items->currentItem();
  for (int index = 0; index < _items->count(); ++index)
  {
    QListWidgetItem* const item = _items->item(index);
    QString const path = item->data(stamp_path_role).toString();
    bool const matches_text = query.isEmpty()
        || item->text().contains(query, Qt::CaseInsensitive);
    bool const matches_shape = shape < 0
        || (item->data(stamp_valid_role).toBool()
            && item->data(stamp_shape_role).toInt() == shape);
    item->setHidden(!path.isEmpty() && (!matches_text || !matches_shape));
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
  _rename->setEnabled(has_item);
  _remove->setEnabled(has_item);
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
  _name->setText(item->text());
  if (!metadata.valid)
  {
    _preview->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(96, 96));
    _details->setText(QString("Unable to load this stamp.\n\n%1").arg(metadata.error));
    return;
  }
  _preview->setPixmap(QPixmap::fromImage(metadata.preview).scaled(
      240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  _details->setText(metadata.details);
}

void StampAssetBrowser::chooseCurrent()
{
  QListWidgetItem* const item = _items->currentItem();
  if (!item || !item->data(stamp_valid_role).toBool())
    return;
  _selected_path = item->data(stamp_path_role).toString();
  accept();
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
  populate(new_path);
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
  if (_active_path.compare(path, Qt::CaseInsensitive) == 0)
    _active_path.clear();
  populate({}, previous_index);
}
