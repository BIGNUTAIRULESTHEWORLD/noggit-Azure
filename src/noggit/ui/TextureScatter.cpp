#include "TextureScatter.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/World.h>
#include <noggit/texture_set.hpp>
#include <noggit/tools/TextureScatterSampling.hpp>
#include <noggit/tools/ScatterAlignment.hpp>
#include <noggit/ui/ObjectEditor.h>
#include <opengl/context.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <map>

namespace
{
  constexpr float cellSize = TEXDETAILSIZE * 2.0f;
  constexpr std::size_t maxObjects = 5000;
}

namespace Noggit::Ui
{
  TextureScatter::TextureScatter(MapView* view, object_editor* editor)
    : QGroupBox("Texture Scatter", editor), _view(view), _editor(editor)
  {
    setCheckable(true);
    setChecked(false);
    auto outerLayout = new QVBoxLayout(this);
    auto body = new QWidget(this);
    outerLayout->addWidget(body);
    auto layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto help = new QLabel("Alt + left-drag: resize brush.\n"
                          "Pick texture, then click terrain (or Ctrl + Alt + click).\n"
                          "Drag left: highlight area. Ctrl + drag: erase.\n"
                          "Cyan: eligible texture. Orange: texture rejected.\n"
                          "Choose an M2 in the Asset Browser, then Add copied M2s.", this);
    help->setWordWrap(true);
    layout->addWidget(help);
    _textureLabel = new QLabel("No texture picked", this);
    _textureLabel->setWordWrap(true);
    layout->addWidget(_textureLabel);
    _pickTexture = new QPushButton("Pick texture", this);
    _pickTexture->setCheckable(true);
    _pickTexture->setToolTip("Click here, then click terrain to sample its strongest texture. Click again to cancel.");
    layout->addWidget(_pickTexture);
    _models = new QTableWidget(0, 2, this);
    _models->setHorizontalHeaderLabels({"M2", "Weight"});
    _models->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _models->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _models->setMaximumHeight(110);
    layout->addWidget(_models);
    auto modelButtons = new QHBoxLayout;
    auto add = new QPushButton("Add copied M2s", this);
    auto remove = new QPushButton("Remove", this);
    modelButtons->addWidget(add);
    modelButtons->addWidget(remove);
    layout->addLayout(modelButtons);
    auto form = new QFormLayout;
    auto number = [this, form](char const* label, double low, double high, double value)
    {
      auto box = new QDoubleSpinBox(this);
      box->setRange(low, high);
      box->setDecimals(2);
      box->setValue(value);
      form->addRow(label, box);
      return box;
    };
    _radius = number("Selection radius", 1, 100, 20);
    _radius->setToolTip("Alt + left-drag horizontally to resize. For a square, radius is half the side length.");
    _shape = new QComboBox(this);
    _shape->addItems({"Circle", "Square"});
    form->addRow("Brush shape", _shape);
    _density = number("M2s / 100 square units", 0.01, 1000, 5);
    _density->setToolTip("Target density on eligible ground. Minimum spacing can reduce the final count.");
    _spacing = number("Minimum spacing", 0.1, 100, 2);
    _coverage = number("Minimum texture %", 1, 100, 60);
    _coverage->setToolTip("The picked texture must also be the strongest layer at each placement point.");
    _scaleMin = number("Minimum scale", 0.01, 63, 0.85);
    _scaleMax = number("Maximum scale", 0.01, 63, 1.15);
    _sinkDepth = number("Sink depth", 0, 10, 0);
    _sinkDepth->setSingleStep(0.05);
    _sinkDepth->setToolTip("Lower each model vertically below the terrain by this many world units. Zero keeps the origin on the ground. Try 0.20 for bushes; adjust in the preview.");
    _seed = new QSpinBox(this);
    _seed->setRange(0, 1000000000);
    _seed->setValue(1);
    form->addRow("Seed", _seed);
    _yaw = new QCheckBox("Random rotation (0–360°)", this);
    _yaw->setChecked(true);
    form->addRow(_yaw);
    _alignTerrain = new QCheckBox("Align to terrain", this);
    _alignTerrain->setToolTip("Tilt each M2 to the terrain slope at its origin. Rotates the whole model; does not bend it.");
    form->addRow(_alignTerrain);
    layout->addLayout(form);
    auto buttons = new QHBoxLayout;
    auto preview = new QPushButton("Preview", this);
    auto reroll = new QPushButton("Reroll", this);
    _place = new QPushButton("Place", this);
    _place->setEnabled(false);
    buttons->addWidget(preview);
    buttons->addWidget(reroll);
    buttons->addWidget(_place);
    layout->addLayout(buttons);
    auto cancelButtons = new QHBoxLayout;
    auto cancel = new QPushButton("Cancel preview", this);
    auto clear = new QPushButton("Clear area", this);
    cancelButtons->addWidget(cancel);
    cancelButtons->addWidget(clear);
    layout->addLayout(cancelButtons);
    _status = new QLabel("Highlight an area before generating a preview.", this);
    _status->setWordWrap(true);
    layout->addWidget(_status);
    _timer = new QTimer(this);
    _timer->setInterval(16);
    _attachmentTimer = new QTimer(this);
    _attachmentTimer->setInterval(100);
    connect(_attachmentTimer, &QTimer::timeout, this, [this] { repairPreviewAttachments(); });
    _refreshTimer = new QTimer(this);
    _refreshTimer->setSingleShot(true);
    _refreshTimer->setInterval(180);
    connect(_refreshTimer, &QTimer::timeout, this, [this] {
      if (!_timer->isActive()) generate();
    });
    auto refreshBrush = [this] {
      endStroke();
      _view->invalidate();
      _view->update();
    };
    connect(_radius, qOverload<double>(&QDoubleSpinBox::valueChanged), this, refreshBrush);
    connect(_shape, qOverload<int>(&QComboBox::currentIndexChanged), this, refreshBrush);
    connect(_pickTexture, &QPushButton::toggled, this, refreshBrush);
    connect(_timer, &QTimer::timeout, this, [this] { previewStep(); });
    connect(add, &QPushButton::clicked, this, [this] { addModels(); });
    connect(remove, &QPushButton::clicked, this, [this] {
      if (_models->currentRow() >= 0) { _models->removeRow(_models->currentRow()); schedulePreview(); }
    });
    connect(preview, &QPushButton::clicked, this, [this] { generate(); });
    connect(reroll, &QPushButton::clicked, this, [this] {
      _seed->setValue((_seed->value() + 1) % 1000000001); generate();
    });
    connect(_place, &QPushButton::clicked, this, [this] { place(); });
    connect(cancel, &QPushButton::clicked, this, [this] { invalidatePreview(); });
    connect(clear, &QPushButton::clicked, this, [this] {
      invalidatePreview(); _selection.clear(); endStroke(); status("Selection cleared.");
    });
    for (auto box : {_density, _spacing, _coverage, _scaleMin, _scaleMax, _sinkDepth})
      connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { schedulePreview(); });
    connect(_seed, qOverload<int>(&QSpinBox::valueChanged), this, [this] { schedulePreview(); });
    connect(_yaw, &QCheckBox::toggled, this, [this] { schedulePreview(); });
    connect(_alignTerrain, &QCheckBox::toggled, this, [this] { schedulePreview(); });
    connect(this, &QGroupBox::toggled, this, [this, body](bool enabled) {
      body->setVisible(enabled);
      if (!enabled) _pickTexture->setChecked(false);
      invalidatePreview();
      endStroke();
      if (enabled) { _view->getWorld()->reset_selection(); schedulePreview(); }
    });
    body->hide();
    connect(NOGGIT_ACTION_MGR, &ActionManager::historyNavigated, this, [this] {
      invalidatePreview();
    });
    connect(NOGGIT_ACTION_MGR, &ActionManager::onActionBegin, this, [this](Action*) {
      if (!_committing) invalidatePreview();
    });
  }

  bool TextureScatter::active() const { return isChecked(); }
  float TextureScatter::radius() const { return static_cast<float>(_radius->value()); }
  void TextureScatter::changeRadius(float delta) { _radius->setValue(_radius->value() + delta); }
  ScatterSelection::Shape TextureScatter::brushShape() const
  {
    return _shape->currentIndex() == 1 ? ScatterSelection::Shape::Square : ScatterSelection::Shape::Circle;
  }
  bool TextureScatter::pickingTexture() const { return _pickTexture->isChecked(); }
  ScatterSelection const& TextureScatter::selection() const { return _selection; }
  void TextureScatter::endStroke()
  {
    _lastPaint.reset();
    if (_selectionStatusDirty)
    {
      _selectionStatusDirty = false;
      schedulePreview(false);
    }
  }

  void TextureScatter::status(QString const& message)
  {
    _status->setText(message);
    _view->invalidate();
    _view->update();
  }

  void TextureScatter::clearPreview()
  {
    _timer->stop();
    _attachmentTimer->stop();
    _refreshTimer->stop();
    _refreshPending = false;
    _selectionStatusDirty = false;
    _sampling.reset();
    _loadingModel.reset();
    _previewUids.insert(_previewUids.end(), _retiredUids.begin(), _retiredUids.end());
    _retiredUids.clear();
    _place->setEnabled(false);
    if (!_previewUids.empty())
    {
      _view->makeCurrent();
      OpenGL::context::scoped_setter const context(::gl, _view->context());
      for (auto uid : _previewUids) if (uid) _view->getWorld()->deleteChunkMoverPreviewInstance(uid);
      _previewUids.clear();
    }
    _placements.clear();
  }

  void TextureScatter::schedulePreview(bool restart)
  {
    if (!active() || _committing) return;
    _selection.texture = _texture;
    _selection.coverage = static_cast<float>(_coverage->value());
    _view->invalidate();
    _view->update();
    _place->setEnabled(false);
    _refreshPending = true;
    if (restart)
    {
      _timer->stop();
      _sampling.reset();
    }
    // Do not restart the delay for every brush dab: continuous strokes must
    // produce previews too. Settings edits cancel obsolete sampling immediately.
    if (!_refreshTimer->isActive()) _refreshTimer->start();
  }

  void TextureScatter::invalidatePreview()
  {
    clearPreview();
    status(QString("%1 selected cells. Preview required; up to 5,000 M2s per batch.").arg(_selection.size()));
  }

  void TextureScatter::suspend() { setChecked(false); clearPreview(); }
  void TextureScatter::unload()
  {
    clearPreview();
    disconnect(NOGGIT_ACTION_MGR, nullptr, this, nullptr);
  }

  void TextureScatter::addModels()
  {
    for (auto const& entry : _editor->getClipboard())
    {
      if (entry.index() != eEntry_Object) continue;
      auto object = std::get<selected_object_type>(entry);
      if (object->which() != eMODEL) continue;
      QString const path = QString::fromStdString(object->instance_model()->file_key().filepath());
      bool exists = false;
      for (int row = 0; row < _models->rowCount(); ++row)
        exists |= _models->item(row, 0)->text() == path;
      if (exists) continue;
      int const row = _models->rowCount();
      _models->insertRow(row);
      auto item = new QTableWidgetItem(path);
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      item->setToolTip(path);
      _models->setItem(row, 0, item);
      auto weight = new QSpinBox(_models);
      weight->setRange(1, 1000);
      weight->setValue(1);
      _models->setCellWidget(row, 1, weight);
      connect(weight, qOverload<int>(&QSpinBox::valueChanged), this, [this] { schedulePreview(); });
    }
    schedulePreview();
    if (!_models->rowCount()) status("Choose an M2 in the Asset Browser or copy an existing M2 first.");
  }

  void TextureScatter::pickTexture(glm::vec3 const& position)
  {
    auto chunk = _view->getWorld()->getChunkAt(position);
    if (!chunk) return;
    auto layers = chunk->getTextureSet()->sampleTextureLayersAt(position.x, position.z);
    if (layers.empty()) return;
    auto strongest = std::max_element(layers.begin(), layers.end(), [](auto const& a, auto const& b) {
      return a.weight < b.weight;
    });
    _texture = strongest->texture->file_key().filepath();
    _textureLabel->setText(QString::fromStdString(_texture));
    _pickTexture->setChecked(false);
    schedulePreview();
  }

  void TextureScatter::paint(glm::vec3 const& position, bool erase)
  {
    float const r = radius();
    glm::vec3 const start = _lastPaint && _lastErase == erase && _lastRadius == r ? *_lastPaint : position;
    float const dx = position.x-start.x, dz = position.z-start.z;
    float const distance = std::sqrt(dx*dx+dz*dz);
    if (_lastPaint && distance == 0 && _lastErase == erase && _lastRadius == r) return;
    // Interpolate brush dabs so a quick drag leaves a continuous stroke.
    int const steps = std::max(1, static_cast<int>(std::ceil(distance / std::max(cellSize, r*0.25f))));
    bool changed = false;
    for (int step = 1; step <= steps; ++step)
    {
      float const t = static_cast<float>(step) / steps;
      changed |= _selection.paint((start.x + dx*t)/cellSize, (start.z + dz*t)/cellSize, r/cellSize, erase, brushShape());
    }
    _lastPaint = position;
    _lastErase = erase;
    _lastRadius = r;
    if (changed)
    {
      schedulePreview(false);
      // Changing a wrapping QLabel every frame can relayout the dock/viewport.
      // Refresh the count once at stroke end; the mask itself updates immediately.
      _selectionStatusDirty = true;
      // MapView::paintGL skips rendering without this flag. Scheduling only a
      // QWidget update can present an undrawn frame between timer redraws.
      _view->invalidate();
      _view->update();
    }
  }

  bool TextureScatter::eligible(float x, float z) const
  {
    auto chunk = _view->getWorld()->getChunkAt({x, 0, z});
    if (!chunk) return false;
    int const holeX = std::clamp(static_cast<int>((x - chunk->xbase) / (UNITSIZE * 2)), 0, 3);
    int const holeZ = std::clamp(static_cast<int>((z - chunk->zbase) / (UNITSIZE * 2)), 0, 3);
    if (chunk->isHole(holeX, holeZ)) return false;
    float target = 0, strongest = 0;
    for (auto const& layer : chunk->getTextureSet()->sampleTextureLayersAt(x, z))
    {
      strongest = std::max(strongest, layer.weight);
      if (layer.texture->file_key().filepath() == _texture) target = layer.weight;
    }
    return TextureScatterSampling::matchesTexture(target, strongest, _coverage->value());
  }

  void TextureScatter::generate()
  {
    _refreshTimer->stop();
    _timer->stop();
    _sampling.reset();
    _refreshPending = false;
    _place->setEnabled(false);
    if (!active() || NOGGIT_CUR_ACTION) return;
    if (_texture.empty() || !_models->rowCount() || _selection.empty())
    { clearPreview(); status("Pick a texture, add M2s, and highlight an area first."); return; }
    if (_scaleMin->value() > _scaleMax->value())
    { clearPreview(); status("Minimum scale must not exceed maximum scale."); return; }

    std::vector<std::string> models;
    std::vector<double> weights;
    for (int row = 0; row < _models->rowCount(); ++row)
    {
      models.push_back(_models->item(row, 0)->text().toStdString());
      weights.push_back(static_cast<QSpinBox*>(_models->cellWidget(row, 1))->value());
    }
    auto cells = _selection.cells();
    TextureScatterSampling::Settings const settings{
      cellSize, _density->value(), static_cast<float>(_spacing->value()),
      static_cast<float>(_scaleMin->value()), static_cast<float>(_scaleMax->value()),
      _yaw->isChecked(), static_cast<unsigned>(_seed->value()), maxObjects, weights};
    _loadingModel.reset();
    _attachmentTimer->start();
    _samplingModels = std::move(models);
    _samplingNormals.clear();
    _sampling = std::make_unique<TextureScatterSampling::Job>(std::move(cells), settings);
    status("Updating live preview...");
    _timer->start();
  }

  void TextureScatter::previewStep()
  {
    QElapsedTimer elapsed;
    elapsed.start();
    if (_sampling)
    {
      do
      {
        _sampling->step([this](float x, float z) -> std::optional<float> {
          if (!eligible(x, z)) return std::nullopt;
          glm::vec3 normal(0, 1, 0);
          auto ground = _view->getWorld()->try_get_ground_height({x, 0, z}, _alignTerrain->isChecked() ? &normal : nullptr);
          if (ground) _samplingNormals.push_back(normal);
          return ground ? std::optional<float>(ground->y) : std::nullopt;
        });
      } while (!_sampling->done() && elapsed.elapsed() < 4);
      if (!_sampling->done()) return;

      // Reuse unchanged instances instead of removing/recreating the whole forest.
      using Key = std::tuple<std::string, float, float, float, float, float, float, float>;
      auto key = [](Placement const& p) { return Key{p.model, p.position.x, p.position.y, p.position.z, p.scale, p.rotation.x, p.rotation.y, p.rotation.z}; };
      std::map<Key, std::uint32_t> previous;
      for (std::size_t i = 0; i < _previewUids.size(); ++i)
        if (_previewUids[i]) previous.emplace(key(_placements[i]), _previewUids[i]);
      _placements.clear();
      _previewUids.clear();
      for (auto const& p : _sampling->result())
      {
        auto const normal = _samplingNormals[_placements.size()];
        Placement placement{_samplingModels[p.model], {p.x, p.y - static_cast<float>(_sinkDepth->value()), p.z}, p.scale,
          _alignTerrain->isChecked() ? ScatterAlignment::rotation(normal, p.yaw) : glm::vec3(0, p.yaw, 0), normal, p.y};
        auto found = previous.find(key(placement));
        std::uint32_t uid = 0;
        if (found != previous.end())
        {
          if (_view->getWorld()->get_model(found->second)) uid = found->second;
          previous.erase(found);
        }
        _placements.push_back(std::move(placement));
        _previewUids.push_back(uid);
      }
      for (auto const& entry : previous) _retiredUids.push_back(entry.second);
      _sampling.reset();
      _previewIndex = 0;
      return;
    }

    _view->makeCurrent();
    OpenGL::context::scoped_setter const context(::gl, _view->context());
    try
    {
      while (!_retiredUids.empty() && elapsed.elapsed() < 4)
      {
        _view->getWorld()->deleteChunkMoverPreviewInstance(_retiredUids.back());
        _retiredUids.pop_back();
      }
      while (_retiredUids.empty() && _previewIndex < _placements.size() && elapsed.elapsed() < 4)
      {
        if (!_previewUids[_previewIndex])
        {
          auto const& p = _placements[_previewIndex];
          // Request loading first and yield instead of waiting on disk in World.
          if (!_loadingModel) _loadingModel = std::make_shared<ModelInstance>(
            BlizzardArchive::Listfile::FileKey(p.model), _view->getWorld()->getRenderContext());
          if (!_loadingModel->model->finishedLoading()) break;
          if (_loadingModel->model->loading_failed()) throw std::runtime_error("M2 could not be loaded");
          auto instance = _view->getWorld()->addChunkMoverPreviewM2(
            BlizzardArchive::Listfile::FileKey(p.model), p.position, p.scale, {p.rotation.x, p.rotation.y, p.rotation.z});
          _previewUids[_previewIndex] = instance->uid;
          _loadingModel.reset();
        }
        ++_previewIndex;
      }
    }
    catch (std::exception const& error)
    {
      clearPreview();
      status(QString("Preview failed: %1").arg(error.what()));
      return;
    }
    _view->invalidate();
    _view->update();
    if (_previewIndex == _placements.size() && _retiredUids.empty())
    {
      _timer->stop();
      if (_refreshPending) { _refreshTimer->start(); return; }
      _place->setEnabled(!_placements.empty());
      status(_placements.empty() ? "No placements fit. Check texture coverage, density, and the selected area."
        : QString("%1 preview M2s. %2").arg(_placements.size()).arg(_placements.size() == maxObjects
          ? "5,000-object limit: spread across the selection. Reduce density or clear old areas." : "Place commits this exact preview."));
    }
  }

  void TextureScatter::repairPreviewAttachments()
  {
    if (!active() || _committing || _previewUids.empty()) return;
    QElapsedTimer elapsed;
    elapsed.start();
    bool changed = false;
    std::size_t checked = 0;
    while (checked++ < _previewUids.size() && elapsed.elapsed() < 2)
    {
      _attachmentIndex %= _previewUids.size();
      auto uid = _previewUids[_attachmentIndex++];
      if (!uid) continue;
      auto object = _view->getWorld()->get_model(uid);
      if (!object) { schedulePreview(); return; }
      auto* instance = std::get<selected_object_type>(*object);
      if (!instance->chunk_mover_preview) continue;
      auto const& extents = instance->getExtents();
      TileIndex start(extents[0]), end(extents[1]);
      if (!start.is_valid() || !end.is_valid()) continue;
      for (std::size_t z = start.z; z <= end.z; ++z)
        for (std::size_t x = start.x; x <= end.x; ++x)
          if (auto* tile = _view->getWorld()->mapIndex.getTile(TileIndex{x, z}); tile && tile->finishedLoading())
          {
            auto const& attached = instance->getTiles();
            if (std::find(attached.begin(), attached.end(), tile) == attached.end())
            { tile->add_model(instance); changed = true; }
          }
    }
    if (changed) { _view->invalidate(); _view->update(); }
  }

  void TextureScatter::place()
  {
    if (_placements.empty() || _timer->isActive() || _refreshPending || _refreshTimer->isActive() || NOGGIT_CUR_ACTION || !active()) return;
    for (auto uid : _previewUids)
      if (!_view->getWorld()->get_model(uid))
      { invalidatePreview(); status("Preview objects unloaded. Generate a new preview before placing."); return; }
    // Refuse a stale or unloaded preview rather than silently placing a different batch.
    for (auto const& p : _placements)
    {
      glm::vec3 normal(0, 1, 0);
      auto ground = _view->getWorld()->try_get_ground_height(p.position, _alignTerrain->isChecked() ? &normal : nullptr);
      if (!eligible(p.position.x, p.position.z) || !ground || std::abs(ground->y - p.groundHeight) > 0.01f
          || (_alignTerrain->isChecked() && glm::length(normal - p.normal) > 0.001f))
      { invalidatePreview(); status("Terrain changed or unloaded. Generate a new preview before placing."); return; }
    }
    auto placements = _placements;
    clearPreview();
    _view->makeCurrent();
    OpenGL::context::scoped_setter const context(::gl, _view->context());
    _committing = true;
    auto action = NOGGIT_ACTION_MGR->beginAction(_view, ActionFlags::eOBJECTS_ADDED);
    std::size_t added = 0;
    try
    {
      for (auto const& p : placements)
      {
        // Register the actual stored instance and UID, not a temporary constructor copy.
        auto instance = _view->getWorld()->addM2AndGetInstance(
          BlizzardArchive::Listfile::FileKey(p.model), p.position, p.scale, {p.rotation.x, p.rotation.y, p.rotation.z}, nullptr, true, false);
        action->registerObjectAdded(instance);
        ++added;
      }
      NOGGIT_ACTION_MGR->endAction();
      _committing = false;
      status(QString("Placed %1 M2s. Undo removes this batch. Clear or repaint the area for the next batch.").arg(added));
      // Place disables itself when clearing the preview. Return keyboard focus
      // to the viewport so Ctrl+Z operates on map history instead of a panel editor.
      _view->setFocus(Qt::OtherFocusReason);
    }
    catch (std::exception const& error)
    {
      NOGGIT_ACTION_MGR->endAction();
      _committing = false;
      if (added) NOGGIT_ACTION_MGR->undo();
      status(QString("Placement failed; registered objects rolled back: %1").arg(error.what()));
    }
  }
}
