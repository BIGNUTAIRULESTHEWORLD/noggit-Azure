#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapBookmarkListItem.hpp>

#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace Noggit::Ui::Widget
{
  MapListBookmarkItem::MapListBookmarkItem(const MapListBookmarkData& data, QWidget* parent)
    : QWidget(parent)
  {
    setAttribute(Qt::WA_StyledBackground, false);
    setContextMenuPolicy(Qt::CustomContextMenu);

    map_icon = new QLabel(this);
    map_icon->setFixedSize(34, 34);
    map_icon->setAlignment(Qt::AlignCenter);
    map_icon->setPixmap(FontAwesomeIcon(FontAwesome::bookmark).pixmap(QSize(27, 27)));
    auto color = new QGraphicsColorizeEffect(map_icon);
    color->setColor(QColor(214, 183, 119));
    color->setStrength(1.0f);
    map_icon->setGraphicsEffect(color);

    QString const name = toCamelCase(data.MapName);
    map_name = new QLabel(name, this);
    map_name->setObjectName("mapListName");
    map_name->setToolTip(name);
    map_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    map_position = new QLabel(
        QString("%1, %2, %3").arg(static_cast<int>(data.Position.x))
            .arg(static_cast<int>(data.Position.y)).arg(static_cast<int>(data.Position.z)), this);
    map_position->setObjectName("mapListMeta");

    auto text_layout = new QVBoxLayout();
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(2);
    text_layout->addWidget(map_name);
    text_layout->addWidget(map_position);

    auto row_layout = new QHBoxLayout(this);
    row_layout->setContentsMargins(8, 5, 8, 5);
    row_layout->setSpacing(9);
    row_layout->addWidget(map_icon);
    row_layout->addLayout(text_layout, 1);
  }

  QSize MapListBookmarkItem::minimumSizeHint() const
  {
    return QSize(280, 52);
  }

  QString MapListBookmarkItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
