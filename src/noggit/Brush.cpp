// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Brush.h>

#include <algorithm>
#include <cmath>

void Brush::init()
{
  radius = 15;
  shape = BrushShape::CIRCLE;
  hardness = 0.5f;
  iradius = hardness * radius;
  oradius = radius - iradius;
}

void Brush::setHardness(float H)
{
  hardness = H;
  iradius = hardness * radius;
  oradius = radius - iradius;
}

void Brush::setRadius(float R)
{
  radius = R;
  iradius = hardness * radius;
  oradius = radius - iradius;
}

void Brush::setShape(BrushShape shape_)
{
  shape = shape_;
}

float Brush::getHardness() const
{
  return hardness;
}

float Brush::getRadius() const
{
  return radius;
}

BrushShape Brush::getShape() const
{
  return shape;
}

float Brush::getValue(float dist) const
{
  if (dist > radius)
    return 0.0f;
  if (dist < iradius)
    return 1.0f;
  return(1.0f - (dist - iradius) / oradius);
}

float Brush::getValue(float x_dist, float z_dist) const
{
  float const dist = shape == BrushShape::SQUARE
    ? std::max(std::abs(x_dist), std::abs(z_dist))
    : std::sqrt(x_dist * x_dist + z_dist * z_dist);
  return getValue(dist);
}
