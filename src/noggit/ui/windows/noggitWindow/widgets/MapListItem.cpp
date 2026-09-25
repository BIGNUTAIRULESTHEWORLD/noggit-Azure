#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>

#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace Noggit::Ui::Widget
{
  MapListItem::MapListItem(const MapListData& data, QWidget* parent)
    : QWidget(parent)
    , _map_pinned_label(nullptr)
    , _map_data(data)
  {
    setAttribute(Qt::WA_StyledBackground, false);
    setContextMenuPolicy(Qt::CustomContextMenu);

    QIcon icon;
    switch (_map_data.expansion_id)
    {
      case 0: icon = QIcon(":/icon-classic"); break;
      case 1: icon = QIcon(":/icon-burning"); break;
      case 2: icon = QIcon(":/icon-wrath"); break;
      case 3: icon = QIcon(":/icon-cata"); break;
      case 4: icon = QIcon(":/icon-panda"); break;
      case 5: icon = QIcon(":/icon-warlords"); break;
      case 6: icon = QIcon(":/icon-legion"); break;
      case 7: icon = QIcon(":/icon-battle"); break;
      case 8: icon = QIcon(":/icon-shadow"); break;
      default: break;
    }

    _map_icon = new QLabel(this);
    _map_icon->setObjectName("mapListIcon");
    _map_icon->setFixedSize(34, 34);
    _map_icon->setAlignment(Qt::AlignCenter);
    _map_icon->setPixmap(icon.pixmap(QSize(32, 32)));

    QString const map_name = toCamelCase(_map_data.map_name);
    _map_name = new QLabel(map_name, this);
    _map_name->setObjectName("mapListName");
    _map_name->setToolTip(map_name);
    _map_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QString instance_type;
    switch (_map_data.map_type_id)
    {
      case 0: instance_type = "Continent"; break;
      case 1: instance_type = "Dungeon"; break;
      case 2: instance_type = "Raid"; break;
      case 3: instance_type = "Battleground"; break;
      case 4: instance_type = "Arena"; break;
      case 5: instance_type = "Scenario"; break;
      default: instance_type = "Unknown"; break;
    }

    _map_id = new QLabel(QString("Map %1 ·").arg(_map_data.map_id), this);
    _map_id->setObjectName("mapListMeta");
    _map_instance_type = new QLabel(instance_type, this);
    _map_instance_type->setObjectName("mapListMeta");

    auto meta_layout = new QHBoxLayout();
    meta_layout->setContentsMargins(0, 0, 0, 0);
    meta_layout->setSpacing(4);
    meta_layout->addWidget(_map_id);
    meta_layout->addWidget(_map_instance_type);
    meta_layout->addStretch();

    auto text_layout = new QVBoxLayout();
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(2);
    text_layout->addWidget(_map_name);
    text_layout->addLayout(meta_layout);

    auto row_layout = new QHBoxLayout(this);
    row_layout->setContentsMargins(8, 5, 8, 5);
    row_layout->setSpacing(9);
    row_layout->addWidget(_map_icon);
    row_layout->addLayout(text_layout, 1);

    if (_map_data.pinned)
    {
      _map_pinned_label = new QLabel(this);
      _map_pinned_label->setObjectName("mapListPinned");
      _map_pinned_label->setPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(14, 14)));
      _map_pinned_label->setFixedSize(18, 18);
      _map_pinned_label->setAlignment(Qt::AlignCenter);
      auto color = new QGraphicsColorizeEffect(_map_pinned_label);
      color->setColor(QColor(214, 183, 119));
      color->setStrength(1.0f);
      _map_pinned_label->setGraphicsEffect(color);
      row_layout->addWidget(_map_pinned_label);
    }
  }

  QSize MapListItem::minimumSizeHint() const
  {
    return QSize(280, 52);
  }

  const QString MapListItem::name() const
  {
    return _map_data.map_name;
  }

  int MapListItem::id() const
  {
    return _map_data.map_id;
  }

  int MapListItem::type() const
  {
    return _map_data.map_type_id;
  }

  int MapListItem::expansion() const
  {
    return _map_data.expansion_id;
  }

  bool MapListItem::wmo_map() const
  {
    return _map_data.wmo_map;
  }

  QString MapListItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
