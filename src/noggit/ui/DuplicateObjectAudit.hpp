// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QWidget>

#include <memory>

class MapView;

namespace Noggit::Ui
{
  class DuplicateObjectAudit final : public QWidget
  {
  public:
    explicit DuplicateObjectAudit(MapView* map_view, QWidget* parent = nullptr);
    ~DuplicateObjectAudit() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
