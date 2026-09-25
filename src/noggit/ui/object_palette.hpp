// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_OBJECT_PALETTE_HPP
#define NOGGIT_OBJECT_PALETTE_HPP

#include <noggit/ui/widget.hpp>

#include <QJsonArray>
#include <QPointer>

#include <memory>

class QPushButton;
class QDialog;
class MapView;

namespace Noggit::Project
{
  class NoggitProject;
}

namespace Noggit
{
  namespace Ui
  {
    class ObjectPalette : public widget
    {
      Q_OBJECT

    public:
      ObjectPalette(MapView* map_view, std::shared_ptr<Noggit::Project::NoggitProject> Project, QWidget* parent);

      void saveSelectedGroup();
      void browseSavedGroups();

    signals:
      void savedGroupSelected(QJsonArray const& objects, bool keep_grouped);

    private:
      QPushButton* _save_group_button;
      QPushButton* _browse_groups_button;
      MapView* _map_view;
      std::shared_ptr<Noggit::Project::NoggitProject> _project;
      QPointer<QDialog> _saved_groups_dialog;

    };
  }
}

#endif //NOGGIT_OBJECT_PALETTE_HPP
