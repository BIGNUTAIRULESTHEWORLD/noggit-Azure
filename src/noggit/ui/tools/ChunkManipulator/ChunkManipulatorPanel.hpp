// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ui/tools/ChunkManipulator/ChunkClipboard.hpp>

#include <QString>
#include <QWidget>
#include <map>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace Noggit::Ui::Tools::ChunkManipulator
{
  class ChunkManipulatorPanel : public QWidget
  {
    Q_OBJECT
  public:
    explicit ChunkManipulatorPanel(QWidget* parent = nullptr);

    [[nodiscard]] ChunkCopyFlags copyFlags() const;
    [[nodiscard]] ChunkPasteOptions pasteOptions() const;
    [[nodiscard]] ChunkPreviewOptions previewOptions() const;
    [[nodiscard]] float brushRadius() const;
    [[nodiscard]] bool squareBrush() const;
    void changeRadius(float amount);
    void rotateClockwise();
    void toggleMirrorHorizontal();
    void toggleMirrorVertical();
    void adjustHeightOffset(float amount);
    void setSelectionCount(std::size_t count);
    void setClipboardCount(std::size_t count, std::size_t m2_count, std::size_t wmo_count);
    void showPasteResult(ChunkPasteResult const& result);
    void refreshAssetLibrary(QString const& active_path = {});
    void showAssetStatus(QString const& status, bool error = false);
    [[nodiscard]] bool hasAssetSelection() const;
    void clearAssetSelection();

  signals:
    void pasteRequested();
    void clearSelectionRequested();
    void transformChanged();
    void previewChanged();
    void saveAssetRequested(QString const& name);
    void loadAssetRequested(QString const& path);
    void deleteAssetRequested(QString const& path);

  private:
    QCheckBox* addComponent(QString const& label, ChunkCopyFlags flag, bool checked);

    std::map<ChunkCopyFlags, QCheckBox*> _components;
    QComboBox* _rotation = nullptr;
    QComboBox* _height_mode = nullptr;
    QDoubleSpinBox* _height_offset = nullptr;
    QDoubleSpinBox* _radius = nullptr;
    QCheckBox* _mirror_horizontal = nullptr;
    QCheckBox* _mirror_vertical = nullptr;
    QCheckBox* _preview_enabled = nullptr;
    QCheckBox* _preview_m2s = nullptr;
    QCheckBox* _preview_wmos = nullptr;
    QCheckBox* _preview_heightmap = nullptr;
    QCheckBox* _preview_textures = nullptr;
    QLabel* _selection_status = nullptr;
    QLabel* _clipboard_status = nullptr;
    QLabel* _result_status = nullptr;
    QComboBox* _asset_library = nullptr;
    QLabel* _asset_status = nullptr;
  };
}
