// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_STAMP_ASSET_BROWSER_HPP
#define NOGGIT_STAMP_ASSET_BROWSER_HPP

#include <noggit/ui/tools/Stamp/MapStampAsset.hpp>

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QBoxLayout;
class QFileInfo;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QResizeEvent;
class QSplitter;

namespace Noggit::Ui::Tools::Stamp
{
  class StampAssetBrowser : public QWidget
  {
    Q_OBJECT

  public:
    StampAssetBrowser(QString directory_path, QString active_path, QWidget* parent = nullptr);
    ~StampAssetBrowser() override;

    void refresh(QString const& preferred_path = {});
    void pauseLoading();
    void invalidate(QString const& path);
    void setActivePath(QString const& path);

  signals:
    void stampChosen(QString const& path);
    void libraryChanged(QString const& active_path);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    class PreviewLoader;

    struct Metadata
    {
      QImage preview;
      QString details;
      MapStampShape shape = MapStampShape::Circle;
      bool valid = false;
      bool loading = false;
      QString error;
      QDateTime modified;
      qint64 size = -1;
    };

    void populate(QString const& preferred_path = {}, int preferred_index = -1);
    static Metadata readMetadata(QFileInfo const& asset_info);
    void applyMetadata(QListWidgetItem* item, QString const& path, Metadata const& metadata);
    void previewLoaded(QString const& path, Metadata const& metadata, quint64 generation);
    void selectPreferred(QString const& preferred_path, int preferred_index);
    [[nodiscard]] bool matchesFilters(QListWidgetItem const* item) const;
    void applyFilters();
    void updateResponsiveLayout();
    void updateSelection();
    void chooseCurrent();
    void renameCurrent();
    void deleteCurrent();

    QString _directory_path;
    QString _active_path;
    QLineEdit* _search = nullptr;
    QComboBox* _shape_filter = nullptr;
    QSplitter* _splitter = nullptr;
    QListWidget* _items = nullptr;
    QFrame* _details_panel = nullptr;
    QBoxLayout* _details_layout = nullptr;
    QLabel* _preview = nullptr;
    QLabel* _name = nullptr;
    QLabel* _details = nullptr;
    QPushButton* _rename = nullptr;
    QPushButton* _remove = nullptr;
    QPushButton* _use = nullptr;
    PreviewLoader* _preview_loader = nullptr;
    QHash<QString, Metadata> _metadata;
    QHash<QString, Metadata> _cache;
    QHash<QString, QPair<QDateTime, qint64>> _listed_files;
    QHash<QString, QListWidgetItem*> _item_by_path;
    QStringList _pending_paths;
    bool _populated = false;
    bool _needs_refresh = false;
    bool _compact_layout = false;
    quint64 _generation = 0;
  };
}

#endif // NOGGIT_STAMP_ASSET_BROWSER_HPP
