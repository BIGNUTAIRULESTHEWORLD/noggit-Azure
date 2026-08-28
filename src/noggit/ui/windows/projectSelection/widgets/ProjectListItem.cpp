#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/ProjectListItem.hpp>

#include <QGraphicsColorizeEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>


namespace Noggit::Ui::Widget
{
  ProjectListItem::ProjectListItem(const ProjectListItemData& data, QWidget* parent = nullptr) : QWidget(parent)
  {
    setObjectName("project-list-item");
    setAttribute(Qt::WA_StyledBackground, false);

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(68, 15, 12, 10);
    layout->setSpacing(0);

    QIcon icon;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      icon = QIcon(":/icon-wrath");
    if (data.project_version == Project::ProjectVersion::SL)
      icon = QIcon(":/icon-shadow");
    _project_version_icon = new QLabel(this);
    _project_version_icon->setFixedSize(0, 0);
    _project_version_icon->setAlignment(Qt::AlignCenter);
    _project_version_icon->hide();

    auto project_name = toCamelCase(QString(data.project_name));
    _project_name_label = new QLabel(project_name, this);
    _project_name_label->setObjectName("project-title-label");
    _project_name_label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

    _project_directory_label = new QLabel(data.project_directory, this);
    _project_directory_label->setObjectName("project-information");
    _project_directory_label->setToolTip(data.project_directory);
    _project_directory_label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    _project_directory_label->setText(
        _project_directory_label->fontMetrics().elidedText(data.project_directory, Qt::ElideMiddle, 165));

    QString version;
    if (data.project_version == Project::ProjectVersion::WOTLK)
      version = "Wrath Of The Lich King";
    if (data.project_version == Project::ProjectVersion::SL)
      version = "Shadowlands";

    _project_version_label = new QLabel(version, this);
    _project_version_label->setObjectName("project-information");
    _project_version_label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

    auto information_layout = new QVBoxLayout();
    information_layout->setContentsMargins(0, 0, 0, 0);
    information_layout->setSpacing(2);
    information_layout->setAlignment(Qt::AlignVCenter);
    information_layout->addWidget(_project_name_label);
    information_layout->addWidget(_project_directory_label);
    information_layout->addWidget(_project_version_label);

    _project_last_edited_label = new QLabel(data.project_last_edited, this);
    _project_last_edited_label->setAlignment(Qt::AlignRight | Qt::AlignTop);
    _project_last_edited_label->setObjectName("project-last-edited");
    _project_last_edited_label->setToolTip(data.project_last_edited);
    _project_last_edited_label->hide();

    auto trailing_layout = new QVBoxLayout();
    trailing_layout->setContentsMargins(0, 0, 0, 0);
    trailing_layout->setSpacing(2);

    if (data.is_favorite)
    {
        _project_favorite_icon = new QLabel("", this);
        _project_favorite_icon->setPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(16, 16)));
        _project_favorite_icon->setAlignment(Qt::AlignRight | Qt::AlignTop);
        _project_favorite_icon->setObjectName("project-favorite");

        auto colour = new QGraphicsColorizeEffect(this);
        colour->setColor(QColor(255, 204, 0));
        colour->setStrength(1.0f);

        _project_favorite_icon->setGraphicsEffect(colour);
        trailing_layout->addWidget(_project_favorite_icon);
    }
    else
    {
        trailing_layout->addStretch();
    }

    trailing_layout->addWidget(_project_last_edited_label);

    setContextMenuPolicy(Qt::CustomContextMenu);

    layout->addLayout(information_layout, 1);
    layout->addLayout(trailing_layout, 0);
  }

  QSize ProjectListItem::minimumSizeHint() const
  {
    return QSize(250, 108);
  }

  QString ProjectListItem::toCamelCase(const QString& s)
  {
    QStringList parts = s.split(' ', Qt::SplitBehaviorFlags::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i)
      parts[i].replace(0, 1, parts[i][0].toUpper());

    return parts.join(" ");
  }
}
