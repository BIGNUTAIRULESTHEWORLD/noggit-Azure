// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "object_palette.hpp"

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/MapView.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>
#include <noggit/World.h>
#include <noggit/ModelInstance.h>

#include <QDockWidget>
#include <QDialog>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSaveFile>
#include <QTimer>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <exception>
#include <limits>
#include <string>

namespace
{
  QString groupLibraryPath(Noggit::Project::NoggitProject const& project)
  {
    return QDir(QString::fromStdString(project.ProjectPath)).filePath("noggit_m2_groups.json");
  }

  bool loadGroupLibrary(QString const& path, QJsonArray& groups, QString& error)
  {
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly))
    {
      error = file.errorString();
      return false;
    }
    QJsonParseError parse_error;
    auto const document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()
        || !document.object().value("groups").isArray())
    {
      error = "The saved groups file is invalid.";
      return false;
    }
    groups = document.object().value("groups").toArray();
    return true;
  }

  bool writeGroupLibrary(QString const& path, QJsonArray const& groups, QString& error)
  {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
      error = file.errorString();
      return false;
    }
    QJsonObject root;
    root.insert("version", 1);
    root.insert("groups", groups);
    auto const data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
    {
      error = file.errorString();
      return false;
    }
    return true;
  }
}


namespace Noggit
{
  namespace Ui
  {

    ObjectPalette::ObjectPalette(MapView* map_view, std::shared_ptr<Noggit::Project::NoggitProject> Project, QWidget* parent)
        : widget(parent), _map_view(map_view), _project(Project)
    {
      setWindowTitle("Object Group Browser");
      setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
      setMinimumSize(180, 80);

      auto* button_layout = new QVBoxLayout(this);

      _save_group_button = new QPushButton("Save selected group", this);
      _save_group_button->setToolTip("Save the selected M2s to this project's group browser");
      button_layout->addWidget(_save_group_button);
      connect(_save_group_button, &QAbstractButton::clicked, this, &ObjectPalette::saveSelectedGroup);

      _browse_groups_button = new QPushButton("Browse groups", this);
      button_layout->addWidget(_browse_groups_button);
      connect(_browse_groups_button, &QAbstractButton::clicked, this, &ObjectPalette::browseSavedGroups);

      button_layout->addStretch();

    }

    void ObjectPalette::saveSelectedGroup()
    {
      World* const world = _map_view->getWorld();
      selection_group const* selected_group = nullptr;
      for (auto const& group : world->_selection_groups)
      {
        if (!group.isSelected()) continue;
        if (selected_group)
        {
          QMessageBox::information(this, "Save M2 group", "Select one group to save.");
          return;
        }
        selected_group = &group;
      }
      std::vector<ModelInstance*> models;
      glm::vec3 pivot(0.0f);
      if (selected_group)
      {
        for (auto const uid : selected_group->getMembers())
        {
          auto const selection = world->get_model(uid);
          if (!selection || selection->index() != eEntry_Object
              || std::get<selected_object_type>(*selection)->which() != eMODEL)
          {
            QMessageBox::warning(this, "Save M2 group", "All group members must be loaded M2s.");
            return;
          }
          models.push_back(static_cast<ModelInstance*>(std::get<selected_object_type>(*selection)));
        }
      }
      else
      {
        // Saving an arrangement does not require creating a persistent scene group.
        for (auto* object : world->get_selected_objects())
        {
          if (!object || object->which() != eMODEL)
          {
            QMessageBox::warning(this, "Save M2 group", "Select only M2s to save a group.");
            return;
          }
          models.push_back(static_cast<ModelInstance*>(object));
        }
      }
      if (models.size() < 2)
      {
        QMessageBox::information(this, "Save M2 group", "Select at least two M2s in the viewport.");
        return;
      }
      for (auto const* model : models)
        pivot += model->pos;
      pivot /= static_cast<float>(models.size());

      QJsonArray objects;
      for (auto const* model : models)
      {
        QJsonObject object;
        object.insert("path", QString::fromStdString(model->instance_model()->file_key().filepath()));
        object.insert("offset", QJsonArray{model->pos.x - pivot.x, model->pos.y - pivot.y, model->pos.z - pivot.z});
        object.insert("rotation", QJsonArray{float(model->dir.x), float(model->dir.y), float(model->dir.z)});
        object.insert("scale", model->scale);
        objects.append(object);
      }

      QString error;
      QJsonArray groups;
      auto const path = groupLibraryPath(*_project);
      if (!loadGroupLibrary(path, groups, error))
      {
        QMessageBox::warning(this, "Save M2 group", error);
        return;
      }
      bool accepted = false;
      auto const name = QInputDialog::getText(this, "Save M2 group", "Group name:",
                                             QLineEdit::Normal, QString(), &accepted).trimmed();
      if (!accepted || name.isEmpty()) return;
      for (auto const& entry : groups)
      {
        if (entry.toObject().value("name").toString().compare(name, Qt::CaseInsensitive) == 0)
        {
          QMessageBox::warning(this, "Save M2 group", "A saved group already has that name.");
          return;
        }
      }
      QJsonObject group;
      group.insert("name", name);
      group.insert("objects", objects);
      groups.append(group);
      if (!writeGroupLibrary(path, groups, error))
        QMessageBox::warning(this, "Save M2 group", error);
    }

    void ObjectPalette::browseSavedGroups()
    {
      if (_saved_groups_dialog)
      {
        if (_saved_groups_dialog->isMinimized()) _saved_groups_dialog->showNormal();
        _saved_groups_dialog->raise();
        _saved_groups_dialog->activateWindow();
        return;
      }

      QString error;
      auto groups = std::make_shared<QJsonArray>();
      auto const path = groupLibraryPath(*_project);
      if (!loadGroupLibrary(path, *groups, error))
      {
        QMessageBox::warning(this, "Saved M2 groups", error);
        return;
      }

      auto* dialog = new QDialog(_map_view, Qt::Window | Qt::WindowTitleHint
          | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint
          | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
      _saved_groups_dialog = dialog;
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->setWindowTitle("Saved M2 Groups");
      dialog->resize(780, 500);
      auto* layout = new QVBoxLayout(dialog);
      auto* search = new QLineEdit(dialog);
      search->setPlaceholderText("Search saved groups or model paths");
      layout->addWidget(search);
      auto* browser_content = new QHBoxLayout();
      auto* list = new QListWidget(dialog);
      browser_content->addWidget(list, 1);
      auto* preview = new QLabel("Select a group to preview", dialog);
      preview->setAlignment(Qt::AlignCenter);
      preview->setMinimumSize(400, 320);
      preview->setFrameShape(QFrame::StyledPanel);
      browser_content->addWidget(preview);
      layout->addLayout(browser_content, 1);
      auto* keep_grouped = new QCheckBox("Keep objects grouped after placement", dialog);
      keep_grouped->setChecked(true);
      keep_grouped->setToolTip("Uncheck to place the preset as separate M2s that can be moved individually.");
      layout->addWidget(keep_grouped);
      auto* buttons = new QHBoxLayout();
      auto* load = new QPushButton("Load for placement", dialog);
      auto* rename = new QPushButton("Rename", dialog);
      auto* remove = new QPushButton("Delete", dialog);
      buttons->addWidget(load);
      buttons->addWidget(rename);
      buttons->addWidget(remove);
      layout->addLayout(buttons);

      // Model VAOs belong to their offscreen GL context, so this browser cannot
      // share the palette thumbnail renderer's model cache.
      auto* preview_renderer = new Noggit::Ui::Tools::PreviewRenderer(
        400, 320, Noggit::NoggitRenderContext::M2_GROUP_PREVIEW, dialog);
      preview_renderer->hide();
      auto* preview_timer = new QTimer(dialog);
      preview_timer->setSingleShot(true);
      preview_timer->setInterval(100);
      connect(preview_timer, &QTimer::timeout, dialog, [groups, list, preview, preview_renderer]()
      {
        auto* item = list->currentItem();
        if (!item || item->isHidden())
        {
          preview->setText("Select a group to preview");
          return;
        }

        auto const objects = groups->at(item->data(Qt::UserRole).toInt())
                                  .toObject().value("objects").toArray();
        std::vector<Noggit::Ui::Tools::M2GroupPreviewInstance> entries;
        entries.reserve(objects.size());
        for (auto const& value : objects)
        {
          auto const object = value.toObject();
          auto const path = object.value("path").toString();
          auto const offset = object.value("offset").toArray();
          auto const rotation = object.value("rotation").toArray();
          double const scale = object.value("scale").toDouble();
          if (!path.endsWith(".m2", Qt::CaseInsensitive)
              || !Noggit::Application::NoggitApplication::instance()->clientData()->exists(path.toStdString())
              || offset.size() != 3 || rotation.size() != 3
              || !std::isfinite(scale) || scale <= 0.0
              || scale > std::numeric_limits<float>::max()
              || !offset.at(0).isDouble() || !offset.at(1).isDouble() || !offset.at(2).isDouble()
              || !rotation.at(0).isDouble() || !rotation.at(1).isDouble() || !rotation.at(2).isDouble())
          {
            preview->setText("Preview unavailable: missing or invalid M2 data");
            return;
          }
          auto const position = glm::vec3(offset.at(0).toDouble(), offset.at(1).toDouble(),
                                          offset.at(2).toDouble());
          auto const direction = glm::vec3(rotation.at(0).toDouble(), rotation.at(1).toDouble(),
                                           rotation.at(2).toDouble());
          if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
              || !std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
          {
            preview->setText("Preview unavailable: invalid M2 transform");
            return;
          }
          entries.push_back({path.toStdString(), position, direction, static_cast<float>(scale)});
        }
        if (entries.empty())
        {
          preview->setText("Preview unavailable: empty group");
          return;
        }
        try
        {
          preview_renderer->setM2GroupOffscreen(entries);
          preview->setPixmap(*preview_renderer->renderToPixmap());
        }
        catch (std::exception const&)
        {
          preview->setText("Preview unavailable: could not render this group");
        }
      });
      connect(list, &QListWidget::currentItemChanged, dialog,
              [=](QListWidgetItem*, QListWidgetItem*)
      {
        preview->setText("Loading preview...");
        preview_timer->start();
      });

      auto populate = [groups, list, search, preview](int selected_index = -1)
      {
        list->clear();
        for (int i = 0; i < groups->size(); ++i)
        {
          auto const group = groups->at(i).toObject();
          auto const objects = group.value("objects").toArray();
          auto* item = new QListWidgetItem(
            QString("%1 (%2 M2s)").arg(group.value("name").toString()).arg(objects.size()), list);
          item->setData(Qt::UserRole, i);
          QStringList paths;
          for (auto const& object : objects) paths.append(object.toObject().value("path").toString());
          item->setToolTip(paths.join('\n'));
          item->setHidden(!item->text().contains(search->text(), Qt::CaseInsensitive)
                          && !item->toolTip().contains(search->text(), Qt::CaseInsensitive));
        }
        for (int i = 0; i < list->count(); ++i)
          if (!list->item(i)->isHidden()
              && list->item(i)->data(Qt::UserRole).toInt() == selected_index)
          {
            list->setCurrentItem(list->item(i));
            break;
          }
        for (int i = 0; !list->currentItem() && i < list->count(); ++i)
          if (!list->item(i)->isHidden())
          {
            list->setCurrentItem(list->item(i));
            break;
          }
        if (!list->currentItem()) preview->setText("No matching groups");
      };
      connect(search, &QLineEdit::textChanged, dialog, [populate](QString const&) { populate(); });
      auto load_group = [this, groups, list, keep_grouped]()
      {
        auto* item = list->currentItem();
        if (!item || item->isHidden()) return;
        emit savedGroupSelected(groups->at(item->data(Qt::UserRole).toInt())
                                    .toObject().value("objects").toArray(),
                                keep_grouped->isChecked());
      };
      connect(load, &QPushButton::clicked, this, load_group);
      connect(list, &QListWidget::itemDoubleClicked, this, [load_group](QListWidgetItem* item)
      {
        if (item && !item->isHidden()) load_group();
      });
      connect(rename, &QPushButton::clicked, dialog, [groups, list, dialog, path, populate]()
      {
        auto* item = list->currentItem();
        if (!item || item->isHidden()) return;
        auto const index = item->data(Qt::UserRole).toInt();
        auto group = groups->at(index).toObject();
        bool accepted = false;
        auto const name = QInputDialog::getText(dialog, "Rename group", "Group name:",
                                               QLineEdit::Normal,
                                               group.value("name").toString(), &accepted).trimmed();
        if (!accepted) return;
        if (name.isEmpty())
        {
          QMessageBox::warning(dialog, "Rename group", "Enter a group name.");
          return;
        }
        for (int i = 0; i < groups->size(); ++i)
        {
          if (i != index && groups->at(i).toObject().value("name").toString()
                                .compare(name, Qt::CaseInsensitive) == 0)
          {
            QMessageBox::warning(dialog, "Rename group", "A saved group already has that name.");
            return;
          }
        }
        if (group.value("name").toString() == name) return;
        group.insert("name", name);
        auto updated = *groups;
        updated.replace(index, group);
        QString save_error;
        if (!writeGroupLibrary(path, updated, save_error))
        {
          QMessageBox::warning(dialog, "Rename group", save_error);
          return;
        }
        *groups = updated;
        populate(index);
      });
      connect(remove, &QPushButton::clicked, dialog, [groups, list, dialog, path, populate]()
      {
        auto* item = list->currentItem();
        if (!item || item->isHidden()) return;
        auto const index = item->data(Qt::UserRole).toInt();
        auto updated = *groups;
        updated.removeAt(index);
        QString save_error;
        if (!writeGroupLibrary(path, updated, save_error))
        {
          QMessageBox::warning(dialog, "Saved M2 groups", save_error);
          return;
        }
        *groups = updated;
        populate();
      });
      populate();
      dialog->show();
    }

  }

}
