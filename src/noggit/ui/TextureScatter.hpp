// Texture-aware placement of ordinary, undoable M2 instances.
#pragma once

#include <QGroupBox>
#include <glm/vec3.hpp>
#include <noggit/tools/ScatterSelection.hpp>
#include <noggit/tools/TextureScatterSampling.hpp>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class MapView;
class ModelInstance;
class QDoubleSpinBox;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QTableWidget;
class QTimer;

namespace Noggit::Ui
{
  class object_editor;

  class TextureScatter : public QGroupBox
  {
  public:
    TextureScatter(MapView* view, object_editor* editor);
    bool active() const;
    float radius() const;
    void changeRadius(float delta);
    ScatterSelection::Shape brushShape() const;
    bool pickingTexture() const;
    void paint(glm::vec3 const& position, bool erase);
    void endStroke();
    void pickTexture(glm::vec3 const& position);
    void suspend();
    void unload();
    ScatterSelection const& selection() const;

  private:
    struct Placement
    {
      std::string model;
      glm::vec3 position;
      float scale;
      glm::vec3 rotation;
      glm::vec3 normal;
      float groundHeight;
    };
    void schedulePreview(bool restart = true);
    void invalidatePreview();
    void clearPreview();
    void generate();
    void previewStep();
    void repairPreviewAttachments();
    void place();
    void addModels();
    bool eligible(float x, float z) const;
    void status(QString const& message);

    MapView* _view;
    object_editor* _editor;
    QTableWidget* _models;
    QLabel* _textureLabel;
    QLabel* _status;
    QDoubleSpinBox* _radius;
    QComboBox* _shape;
    QPushButton* _pickTexture;
    QDoubleSpinBox* _density;
    QDoubleSpinBox* _spacing;
    QDoubleSpinBox* _coverage;
    QDoubleSpinBox* _scaleMin;
    QDoubleSpinBox* _scaleMax;
    QDoubleSpinBox* _sinkDepth;
    QSpinBox* _seed;
    QCheckBox* _yaw;
    QCheckBox* _alignTerrain;
    QPushButton* _place;
    QTimer* _timer;
    QTimer* _refreshTimer;
    QTimer* _attachmentTimer;
    std::size_t _attachmentIndex = 0;
    bool _refreshPending = false;
    std::unique_ptr<TextureScatterSampling::Job> _sampling;
    std::vector<std::string> _samplingModels;
    std::vector<glm::vec3> _samplingNormals;
    std::vector<std::uint32_t> _retiredUids;
    std::size_t _previewIndex = 0;
    std::shared_ptr<ModelInstance> _loadingModel;
    std::string _texture;
    ScatterSelection _selection;
    std::optional<glm::vec3> _lastPaint;
    bool _lastErase = false;
    float _lastRadius = 0;
    bool _selectionStatusDirty = false;
    std::vector<Placement> _placements;
    std::vector<std::uint32_t> _previewUids;
    bool _committing = false;
  };
}
