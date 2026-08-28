// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkManipulatorPanel.hpp"

#include <noggit/project/CurrentProject.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

using namespace Noggit::Ui::Tools::ChunkManipulator;

ChunkManipulatorPanel::ChunkManipulatorPanel(QWidget* parent)
  : QWidget(parent)
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);

  auto* instructions = new QLabel(
    "Shift+Left selects source chunks and Ctrl+Left removes them. The square selection is copied "
    "automatically. Alt+Left drag adjusts the brush size. V pastes, X clears the source or library "
    "selection, R rotates "
    "90 degrees, F mirrors horizontally, Alt+F mirrors vertically, and Space+wheel adjusts "
    "height (hold Ctrl for fine adjustment).", this);
  instructions->setWordWrap(true);
  root->addWidget(instructions);

  _selection_status = new QLabel("Selected: 0 chunks", this);
  _clipboard_status = new QLabel("Clipboard: empty", this);
  root->addWidget(_selection_status);
  root->addWidget(_clipboard_status);

  auto* library_box = new QGroupBox("Chunk library", this);
  auto* library_layout = new QVBoxLayout(library_box);
  _asset_library = new QComboBox(library_box);
  _asset_library->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _asset_library->setMinimumContentsLength(20);
  library_layout->addWidget(_asset_library);
  auto* library_buttons = new QHBoxLayout();
  auto* save_asset = new QPushButton("Save", library_box);
  auto* load_asset = new QPushButton("Load", library_box);
  auto* delete_asset = new QPushButton("Delete", library_box);
  library_buttons->addWidget(save_asset);
  library_buttons->addWidget(load_asset);
  library_buttons->addWidget(delete_asset);
  library_layout->addLayout(library_buttons);
  _asset_status = new QLabel(library_box);
  _asset_status->setWordWrap(true);
  library_layout->addWidget(_asset_status);
  root->addWidget(library_box);
  refreshAssetLibrary();

  auto* override_box = new QGroupBox("Copy / override", this);
  auto* override_layout = new QVBoxLayout(override_box);
  auto add = [&](QString const& name, ChunkCopyFlags flag, bool checked)
  {
    override_layout->addWidget(addComponent(name, flag, checked));
  };
  add("Height", ChunkCopyFlags::TERRAIN, true);
  add("Textures", ChunkCopyFlags::TEXTURES, true);
  add("Alphamaps", ChunkCopyFlags::ALPHAMAPS, true);
  add("Ground-effect IDs", ChunkCopyFlags::GROUND_EFFECTS, true);
  add("Ground-effect exclusion", ChunkCopyFlags::GROUND_EFFECT_EXCLUSION, true);
  add("Liquids", ChunkCopyFlags::LIQUID, true);
  add("Shadows", ChunkCopyFlags::SHADOWS, false);
  add("Vertex colors / shading", ChunkCopyFlags::VERTEX_COLORS, true);
  add("Area ID", ChunkCopyFlags::AREA_ID, true);
  add("Holes", ChunkCopyFlags::HOLES, true);
  add("Chunk flags", ChunkCopyFlags::FLAGS, false);
  add("M2 models", ChunkCopyFlags::MODELS, true);
  add("WMOs", ChunkCopyFlags::WMOs, true);
  root->addWidget(override_box);

  auto* parameters = new QGroupBox("Transform", this);
  auto* form = new QFormLayout(parameters);
  _rotation = new QComboBox(parameters);
  _rotation->addItems({"0 deg", "90 deg", "180 deg", "270 deg"});
  form->addRow("Rotation", _rotation);

  _mirror_horizontal = new QCheckBox("Horizontal mirror (F)", parameters);
  _mirror_vertical = new QCheckBox("Vertical mirror (Alt+F)", parameters);
  form->addRow(_mirror_horizontal);
  form->addRow(_mirror_vertical);

  _height_mode = new QComboBox(parameters);
  _height_mode->addItems({"Normal", "Minimum", "Maximum", "Add", "Subtract"});
  form->addRow("Height mode", _height_mode);

  _height_offset = new QDoubleSpinBox(parameters);
  _height_offset->setRange(-10000.f, 10000.f);
  _height_offset->setDecimals(3);
  _height_offset->setSingleStep(0.1f);
  form->addRow("Height offset", _height_offset);

  _radius = new QDoubleSpinBox(parameters);
  _radius->setRange(1, 1000);
  _radius->setDecimals(1);
  _radius->setSingleStep(1.f);
  _radius->setValue(51);
  _radius->setToolTip("Square brush half-size in world units.");
  form->addRow("Brush size", _radius);

  auto* replace_models = new QLabel("Models: replace destination objects", parameters);
  replace_models->setToolTip("Like Noggit3, enabled M2/WMO components clear matching destination objects before paste.");
  form->addRow(replace_models);
  auto* seams = new QCheckBox("Sew terrain seams automatically", parameters);
  seams->setChecked(true);
  seams->setEnabled(false);
  seams->setToolTip("Chunk Mover always repairs destination borders after a terrain paste.");
  form->addRow(seams);
  root->addWidget(parameters);

  auto* preview_box = new QGroupBox("Preview options", this);
  auto* preview_layout = new QVBoxLayout(preview_box);
  _preview_enabled = new QCheckBox("Enable previews", preview_box);
  _preview_enabled->setChecked(true);
  _preview_m2s = new QCheckBox("M2 previews", preview_box);
  _preview_m2s->setChecked(true);
  _preview_wmos = new QCheckBox("WMO previews", preview_box);
  _preview_wmos->setChecked(true);
  _preview_heightmap = new QCheckBox("Heightmap previews", preview_box);
  _preview_heightmap->setChecked(true);
  _preview_textures = new QCheckBox("Texture previews", preview_box);
  _preview_textures->setChecked(true);
  preview_layout->addWidget(_preview_enabled);
  preview_layout->addWidget(_preview_m2s);
  preview_layout->addWidget(_preview_wmos);
  preview_layout->addWidget(_preview_heightmap);
  preview_layout->addWidget(_preview_textures);
  root->addWidget(preview_box);

  auto* buttons = new QHBoxLayout();
  auto* clear = new QPushButton("Clear selection", this);
  auto* paste = new QPushButton("Paste", this);
  buttons->addWidget(clear);
  buttons->addWidget(paste);
  root->addLayout(buttons);

  _result_status = new QLabel(this);
  _result_status->setWordWrap(true);
  root->addWidget(_result_status);
  root->addStretch();

  connect(clear, &QPushButton::clicked, this, &ChunkManipulatorPanel::clearSelectionRequested);
  connect(paste, &QPushButton::clicked, this, &ChunkManipulatorPanel::pasteRequested);
  connect(save_asset, &QPushButton::clicked, this, [this]
  {
    bool accepted = false;
    QString const name = QInputDialog::getText(this, "Save chunk asset", "Asset name:",
                                                QLineEdit::Normal, {}, &accepted).trimmed();
    if (accepted && !name.isEmpty())
      emit saveAssetRequested(name);
  });
  connect(load_asset, &QPushButton::clicked, this, [this]
  {
    QString const path = _asset_library->currentData().toString();
    if (!path.isEmpty())
      emit loadAssetRequested(path);
  });
  connect(delete_asset, &QPushButton::clicked, this, [this]
  {
    QString const path = _asset_library->currentData().toString();
    if (!path.isEmpty())
      emit deleteAssetRequested(path);
  });
  connect(_rotation, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { emit transformChanged(); });
  connect(_mirror_horizontal, &QCheckBox::toggled, this, [this] { emit transformChanged(); });
  connect(_mirror_vertical, &QCheckBox::toggled, this, [this] { emit transformChanged(); });
  connect(_height_offset, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this] { emit transformChanged(); });
  connect(_preview_enabled, &QCheckBox::toggled, this, [this](bool enabled)
  {
    _preview_m2s->setEnabled(enabled);
    _preview_wmos->setEnabled(enabled);
    _preview_heightmap->setEnabled(enabled);
    _preview_textures->setEnabled(enabled);
    emit previewChanged();
  });
  for (QCheckBox* preview_component : {_preview_m2s, _preview_wmos, _preview_heightmap,
                                       _preview_textures})
    connect(preview_component, &QCheckBox::toggled, this,
            [this] { emit previewChanged(); });
}

QCheckBox* ChunkManipulatorPanel::addComponent(QString const& label, ChunkCopyFlags flag, bool checked)
{
  auto* checkbox = new QCheckBox(label, this);
  checkbox->setChecked(checked);
  _components.emplace(flag, checkbox);
  return checkbox;
}

ChunkCopyFlags ChunkManipulatorPanel::copyFlags() const
{
  ChunkCopyFlags flags = ChunkCopyFlags::NONE;
  for (auto const& [flag, checkbox] : _components)
    if (checkbox->isChecked())
      flags |= flag;
  return flags;
}

ChunkPasteOptions ChunkManipulatorPanel::pasteOptions() const
{
  return {.components = copyFlags(), .rotation_quarter_turns = _rotation->currentIndex(),
          .mirror_horizontal = _mirror_horizontal->isChecked(),
          .mirror_vertical = _mirror_vertical->isChecked(),
          .height_offset = static_cast<float>(_height_offset->value()),
          .height_mode = static_cast<ChunkHeightMode>(_height_mode->currentIndex()),
          .automatic_seams = true};
}

ChunkPreviewOptions ChunkManipulatorPanel::previewOptions() const
{
  return {.enabled = _preview_enabled->isChecked(),
          .m2s = _preview_m2s->isChecked(),
          .wmos = _preview_wmos->isChecked(),
          .heightmap = _preview_heightmap->isChecked(),
          .textures = _preview_textures->isChecked()};
}

float ChunkManipulatorPanel::brushRadius() const
{
  return static_cast<float>(_radius->value());
}

bool ChunkManipulatorPanel::squareBrush() const
{
  return true;
}

void ChunkManipulatorPanel::changeRadius(float amount)
{
  _radius->setValue(_radius->value() + amount);
}

void ChunkManipulatorPanel::rotateClockwise()
{
  _rotation->setCurrentIndex((_rotation->currentIndex() + 1) % 4);
}

void ChunkManipulatorPanel::toggleMirrorHorizontal()
{
  _mirror_horizontal->toggle();
}

void ChunkManipulatorPanel::toggleMirrorVertical()
{
  _mirror_vertical->toggle();
}

void ChunkManipulatorPanel::adjustHeightOffset(float amount)
{
  _height_offset->setValue(_height_offset->value() + amount);
}

void ChunkManipulatorPanel::setSelectionCount(std::size_t count)
{
  _selection_status->setText(QString("Selected: %1 chunk%2").arg(count).arg(count == 1 ? "" : "s"));
}

void ChunkManipulatorPanel::setClipboardCount(std::size_t count, std::size_t m2_count,
                                               std::size_t wmo_count)
{
  _clipboard_status->setText(count
    ? QString("Clipboard: %1 chunk%2, %3 M2, %4 WMO")
        .arg(count).arg(count == 1 ? "" : "s").arg(m2_count).arg(wmo_count)
    : "Clipboard: empty");
}

void ChunkManipulatorPanel::showPasteResult(ChunkPasteResult const& result)
{
  _result_status->setText(QString("Pasted: %1 chunks, %2 objects added, %3 removed%4")
    .arg(result.chunks_changed).arg(result.objects_added).arg(result.objects_removed)
    .arg(result.textures_dropped ? QString(", %1 low-weight textures dropped").arg(result.textures_dropped) : ""));
}

void ChunkManipulatorPanel::refreshAssetLibrary(QString const& active_path)
{
  QString const previous = active_path.isEmpty() ? _asset_library->currentData().toString()
                                                  : QDir::cleanPath(active_path);
  _asset_library->clear();
  QDir const directory(QString::fromStdString(
      Noggit::Project::CurrentProject::get()->ProjectPath) + "/noggit-assets/chunks");
  QFileInfoList const assets = directory.entryInfoList({"*.nogchunk"}, QDir::Files,
                                                        QDir::Name | QDir::IgnoreCase);
  for (QFileInfo const& asset : assets)
  {
    _asset_library->addItem(asset.completeBaseName(), QDir::cleanPath(asset.absoluteFilePath()));
    _asset_library->setItemData(_asset_library->count() - 1, asset.absoluteFilePath(), Qt::ToolTipRole);
  }
  if (_asset_library->count() == 0)
  {
    _asset_library->addItem("No saved chunk assets");
    _asset_library->setEnabled(false);
  }
  else
  {
    _asset_library->setEnabled(true);
    int const index = _asset_library->findData(previous);
    if (index >= 0)
      _asset_library->setCurrentIndex(index);
  }
}

void ChunkManipulatorPanel::showAssetStatus(QString const& status, bool error)
{
  _asset_status->setText(status);
  _asset_status->setStyleSheet(error ? "color: #d9534f;" : QString{});
}

bool ChunkManipulatorPanel::hasAssetSelection() const
{
  return _asset_library->currentIndex() >= 0 && !_asset_library->currentData().toString().isEmpty();
}

void ChunkManipulatorPanel::clearAssetSelection()
{
  if (!hasAssetSelection())
    return;
  _asset_library->setCurrentIndex(-1);
  showAssetStatus("Chunk asset deselected.");
}
