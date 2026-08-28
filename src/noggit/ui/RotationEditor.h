// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <QtWidgets/QWidget>

class World;

class QDoubleSpinBox;

namespace Noggit
{
  struct BoolToggleProperty;
}

namespace Noggit
{
  namespace Ui
  {
    class rotation_editor : public QWidget
    {
    public:
      rotation_editor(QWidget* parent, World* world);

      BoolToggleProperty* use_median_pivot_point = nullptr;
      BoolToggleProperty* scale_around_pivot_point = nullptr;

      void updateValues(World* world);
    private:
      // for single selection
      void set_model_rotation(World* world);
      // for multi selection
      void change_models_rotation(World* world);

      QDoubleSpinBox* _rotation_x;
      QDoubleSpinBox* _rotation_z;
      QDoubleSpinBox* _rotation_y;
      QDoubleSpinBox* _position_x;
      QDoubleSpinBox* _position_z;
      QDoubleSpinBox* _position_y;
      QDoubleSpinBox* _scale;
    };
  }
}
