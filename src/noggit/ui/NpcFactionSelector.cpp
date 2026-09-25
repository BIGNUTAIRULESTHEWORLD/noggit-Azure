#include <noggit/ui/NpcFactionSelector.hpp>

#include <noggit/DBC.h>

#include <QCompleter>
#include <QLineEdit>
#include <QRegularExpression>

namespace Noggit::Ui
{
namespace
{
  QString maskLabel(unsigned mask)
  {
    QStringList labels;
    if (mask & 1u) labels.push_back("players");
    if (mask & 2u) labels.push_back("Alliance");
    if (mask & 4u) labels.push_back("Horde");
    if (mask & 8u) labels.push_back("monsters");
    return labels.join("/");
  }

  QString factionName(DBCFile::Record const& templ)
  {
    unsigned const faction_id = templ.getUInt(FactionTemplateDB::Faction);
    if (faction_id && gFactionDB.CheckIfIdExists(faction_id))
    {
      QString const name = QString::fromUtf8(
        gFactionDB.getByID(faction_id).getLocalizedString(FactionDB::Name)).trimmed();
      if (!name.isEmpty()) return name;
    }

    unsigned const friends = templ.getUInt(FactionTemplateDB::FriendGroup);
    unsigned const enemies = templ.getUInt(FactionTemplateDB::EnemyGroup);
    QString const friend_label = maskLabel(friends);
    QString const enemy_label = maskLabel(enemies);
    if (!friend_label.isEmpty() && enemy_label.isEmpty())
      return "Friendly to " + friend_label;
    if (friend_label.isEmpty() && !enemy_label.isEmpty())
      return "Hostile to " + enemy_label;
    if (!friend_label.isEmpty() || !enemy_label.isEmpty())
      return QString("Friends: %1; enemies: %2")
        .arg(friend_label.isEmpty() ? "none" : friend_label,
             enemy_label.isEmpty() ? "none" : enemy_label);
    return faction_id ? QString("Unnamed faction %1").arg(faction_id)
                      : QString("Unnamed faction template");
  }
}

QString factionTemplateLabel(unsigned id)
{
  if (!gFactionTemplateDB.CheckIfIdExists(id))
    return QString("%1 — Unknown/custom faction template").arg(id);
  auto const record = gFactionTemplateDB.getByID(id);
  return QString("%1 — %2").arg(id).arg(factionName(record));
}

NpcFactionSelector::NpcFactionSelector(QWidget* parent)
  : QComboBox(parent)
{
  setEditable(true);
  setInsertPolicy(QComboBox::NoInsert);
  setMaxVisibleItems(24);
  setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  setMinimumContentsLength(24);
  lineEdit()->setPlaceholderText("Type an ID or faction name");

  for (auto record = gFactionTemplateDB.begin(); record != gFactionTemplateDB.end(); ++record)
  {
    unsigned const template_id = record->getUInt(FactionTemplateDB::ID);
    unsigned const faction_id = record->getUInt(FactionTemplateDB::Faction);
    QString const label = QString("%1 — %2").arg(template_id).arg(factionName(*record));
    addItem(label, template_id);
    setItemData(count() - 1,
      QString("FactionTemplate %1; Faction.dbc %2; flags 0x%3")
        .arg(template_id).arg(faction_id)
        .arg(record->getUInt(FactionTemplateDB::Flags), 0, 16),
      Qt::ToolTipRole);
  }

  completer()->setCaseSensitivity(Qt::CaseInsensitive);
  completer()->setFilterMode(Qt::MatchContains);
  completer()->setCompletionMode(QCompleter::PopupCompletion);
  connect(lineEdit(), &QLineEdit::editingFinished, this, [this]
  {
    QRegularExpression const leading_id("^\\s*(\\d+)");
    auto const match = leading_id.match(currentText());
    if (match.hasMatch()) setFactionId(match.captured(1).toUInt());
  });
}

unsigned NpcFactionSelector::factionId() const
{
  QRegularExpression const leading_id("^\\s*(\\d+)");
  auto const match = leading_id.match(currentText());
  if (match.hasMatch()) return match.captured(1).toUInt();
  return currentIndex() >= 0 ? currentData().toUInt() : 0u;
}

void NpcFactionSelector::setFactionId(unsigned id)
{
  int index = findData(id);
  if (index < 0)
  {
    addItem(factionTemplateLabel(id), id);
    index = count() - 1;
    setItemData(index, "This ID is not present in the configured FactionTemplate.dbc.",
                Qt::ToolTipRole);
  }
  setCurrentIndex(index);
}
}
