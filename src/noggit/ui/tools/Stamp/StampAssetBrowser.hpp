// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_STAMP_ASSET_BROWSER_HPP
#define NOGGIT_STAMP_ASSET_BROWSER_HPP

#include <noggit/ui/tools/Stamp/MapStampAsset.hpp>

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace Noggit::Ui::Tools::Stamp
{
  class StampAssetBrowser : public QDialog
  {
  public:
    StampAssetBrowser(QString directory_path, QString active_path, QWidget* parent = nullptr);

    [[nodiscard]] QString selectedPath() const;
    [[nodiscard]] QString activePath() const;

  private:
    struct Metadata
    {
      QImage preview;
      QString details;
      MapStampShape shape = MapStampShape::Circle;
      bool valid = false;
      QString error;
    };

    void populate(QString const& preferred_path = {}, int preferred_index = -1);
    void applyFilters();
    void updateSelection();
    void chooseCurrent();
    void renameCurrent();
    void deleteCurrent();

    QString _directory_path;
    QString _active_path;
    QString _selected_path;
    QLineEdit* _search = nullptr;
    QComboBox* _shape_filter = nullptr;
    QListWidget* _items = nullptr;
    QLabel* _preview = nullptr;
    QLabel* _name = nullptr;
    QLabel* _details = nullptr;
    QPushButton* _rename = nullptr;
    QPushButton* _remove = nullptr;
    QPushButton* _cancel = nullptr;
    QPushButton* _use = nullptr;
    QHash<QString, Metadata> _metadata;
  };
}

#endif // NOGGIT_STAMP_ASSET_BROWSER_HPP
