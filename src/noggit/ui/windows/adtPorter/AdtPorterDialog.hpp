#pragma once

#include <QDialog>
#include <memory>

class QComboBox;
class QCheckBox;
class QLabel;
class QSlider;
class World;

namespace Noggit::Project { class NoggitProject; }

namespace Noggit::Ui::Windows
{
  class AdtGridWidget;

  class AdtPorterDialog final : public QDialog
  {
    Q_OBJECT
  public:
    explicit AdtPorterDialog(std::shared_ptr<Project::NoggitProject> project,
                             QWidget* parent = nullptr);
    ~AdtPorterDialog() override;

  private:
    void portAdt();
    void updateSummary();
    void reloadSourceGrid();
    void reloadDestinationGrid();

    std::shared_ptr<Project::NoggitProject> _project;
    QComboBox* _source_map = nullptr;
    QComboBox* _destination_map = nullptr;
    QCheckBox* _same_map = nullptr;
    QSlider* _preview_opacity = nullptr;
    AdtGridWidget* _source_grid = nullptr;
    AdtGridWidget* _destination_grid = nullptr;
    QLabel* _summary = nullptr;
    QLabel* _status = nullptr;
    std::unique_ptr<World> _source_preview_world;
    std::unique_ptr<World> _destination_preview_world;
  };
}
