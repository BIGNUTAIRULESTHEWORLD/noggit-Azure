// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/tool_enums.hpp>

class Brush
{
private:
  float hardness;
  float iradius;
  float oradius;
  float radius;
  BrushShape shape;

public:
  void setHardness(float H);
  void setRadius(float R);
  void setShape(BrushShape shape_);
  float getHardness() const;
  float getRadius() const;
  BrushShape getShape() const;
  float getValue(float dist) const;
  float getValue(float x_dist, float z_dist) const;
  void init();
};
