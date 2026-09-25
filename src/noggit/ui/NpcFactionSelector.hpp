#pragma once

#include <QComboBox>
#include <QString>

namespace Noggit::Ui
{
  QString factionTemplateLabel(unsigned id);

  class NpcFactionSelector : public QComboBox
  {
  public:
    explicit NpcFactionSelector(QWidget* parent = nullptr);

    unsigned factionId() const;
    void setFactionId(unsigned id);
  };
}
