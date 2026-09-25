#pragma once

#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace Noggit::Sql
{
  struct WorldDatabaseSchema
  {
    QString creature_entry_column;
    QString creature_radius_column;
    QString creature_spawn_display_column;
    bool uses_creature_template_model = false;

    QString creatureEntry(QString const& alias = {}) const
    {
      QString const prefix = alias.isEmpty() ? QString() : alias + ".";
      return prefix + "`" + creature_entry_column + "`";
    }

    QString creatureReferencesTemplate(QString const& alias = {}) const
    {
      if (creature_entry_column != "id1") return creatureEntry(alias) + " = ?";
      QString const prefix = alias.isEmpty() ? QString() : alias + ".";
      return QString("? IN (%1`id1`, %1`id2`, %1`id3`)").arg(prefix);
    }

    QString creatureRadius(QString const& alias = {}) const
    {
      QString const prefix = alias.isEmpty() ? QString() : alias + ".";
      return prefix + "`" + creature_radius_column + "`";
    }

    QString creatureSpawnDisplay(QString const& alias = {}) const
    {
      if (creature_spawn_display_column.isEmpty()) return {};
      QString const prefix = alias.isEmpty() ? QString() : alias + ".";
      return prefix + "`" + creature_spawn_display_column + "`";
    }

    QString templateDisplay(QString const& alias = "ct") const
    {
      if (uses_creature_template_model)
      {
        return QString("COALESCE((SELECT `CreatureDisplayID` "
                       "FROM `creature_template_model` ctm "
                       "WHERE ctm.`CreatureID` = %1.`entry` "
                       "AND ctm.`CreatureDisplayID` <> 0 "
                       "ORDER BY ctm.`Idx` LIMIT 1), 0)").arg(alias);
      }
      return QString("COALESCE(NULLIF(%1.`modelid1`, 0), NULLIF(%1.`modelid2`, 0), "
                     "NULLIF(%1.`modelid3`, 0), NULLIF(%1.`modelid4`, 0), 0)").arg(alias);
    }
  };

  inline bool worldTableExists(QSqlDatabase& db, QString const& table)
  {
    QSqlQuery query(db);
    query.prepare("SHOW TABLES LIKE ?");
    query.addBindValue(table);
    return query.exec() && query.next();
  }

  inline bool worldTableColumns(QSqlDatabase& db, QString const& table,
                                QSet<QString>& columns, QString& error)
  {
    columns.clear();
    QSqlQuery query(db);
    if (!query.exec(QString("SHOW COLUMNS FROM `%1`").arg(table)))
    {
      error = QString("Could not inspect %1: %2").arg(table, query.lastError().text());
      return false;
    }
    while (query.next()) columns.insert(query.value(0).toString().toLower());
    return true;
  }

  inline bool inspectWorldDatabaseSchema(QSqlDatabase& db, WorldDatabaseSchema& schema,
                                         QString& error)
  {
    QSet<QString> creature_columns;
    QSet<QString> template_columns;
    if (!worldTableColumns(db, "creature", creature_columns, error)
        || !worldTableColumns(db, "creature_template", template_columns, error))
      return false;

    if (creature_columns.contains("id1")) schema.creature_entry_column = "id1";
    else if (creature_columns.contains("id")) schema.creature_entry_column = "id";
    else
    {
      error = "The creature table has neither AzerothCore id1 nor legacy id.";
      return false;
    }

    if (creature_columns.contains("wander_distance"))
      schema.creature_radius_column = "wander_distance";
    else if (creature_columns.contains("spawndist"))
      schema.creature_radius_column = "spawndist";
    else
    {
      error = "The creature table has neither AzerothCore wander_distance nor legacy spawndist.";
      return false;
    }

    if (creature_columns.contains("modelid"))
      schema.creature_spawn_display_column = "modelid";

    if (template_columns.contains("modelid1"))
    {
      for (QString const& column : {"modelid1", "modelid2", "modelid3", "modelid4"})
        if (!template_columns.contains(column))
        {
          error = "The legacy creature_template model columns are incomplete.";
          return false;
        }
    }
    else
    {
      QSet<QString> model_columns;
      if (!worldTableColumns(db, "creature_template_model", model_columns, error))
      {
        error = "AzerothCore creature_template_model is required: " + error;
        return false;
      }
      for (QString const& column : {"creatureid", "idx", "creaturedisplayid"})
        if (!model_columns.contains(column))
        {
          error = "The creature_template_model table does not match AzerothCore.";
          return false;
        }
      schema.uses_creature_template_model = true;
    }
    return true;
  }

  inline bool loadTemplateDisplay(QSqlDatabase& db, WorldDatabaseSchema const& schema,
                                  unsigned entry, unsigned& display, QString& error,
                                  bool for_update = false)
  {
    QSqlQuery query(db);
    QString sql;
    if (schema.uses_creature_template_model)
      sql = "SELECT `CreatureDisplayID` FROM `creature_template_model` "
            "WHERE `CreatureID` = ? AND `CreatureDisplayID` <> 0 "
            "ORDER BY `Idx` LIMIT 1";
    else
      sql = "SELECT COALESCE(NULLIF(`modelid1`, 0), NULLIF(`modelid2`, 0), "
            "NULLIF(`modelid3`, 0), NULLIF(`modelid4`, 0), 0) "
            "FROM `creature_template` WHERE `entry` = ?";
    if (for_update) sql += " FOR UPDATE";
    query.prepare(sql);
    query.addBindValue(entry);
    if (!query.exec())
    {
      error = query.lastError().text();
      return false;
    }
    if (!query.next())
    {
      if (schema.uses_creature_template_model)
      {
        display = 0;
        return true;
      }
      error = QString("Creature template %1 no longer exists.").arg(entry);
      return false;
    }
    display = query.value(0).toUInt();
    return true;
  }

  inline bool replaceTemplateDisplay(QSqlDatabase& db, WorldDatabaseSchema const& schema,
                                     unsigned entry, unsigned display, QString& error)
  {
    if (schema.uses_creature_template_model)
    {
      QSqlQuery remove(db);
      remove.prepare("DELETE FROM `creature_template_model` WHERE `CreatureID` = ?");
      remove.addBindValue(entry);
      if (!remove.exec())
      {
        error = "Could not replace the AzerothCore creature model rows: "
          + remove.lastError().text();
        return false;
      }
      QSqlQuery insert(db);
      insert.prepare("INSERT INTO `creature_template_model` "
                     "(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`) "
                     "VALUES (?, 0, ?, 1, 1)");
      insert.addBindValue(entry);
      insert.addBindValue(display);
      if (!insert.exec())
      {
        error = "Could not save the AzerothCore creature model row: "
          + insert.lastError().text();
        return false;
      }
      return true;
    }

    QSqlQuery update(db);
    update.prepare("UPDATE `creature_template` SET `modelid1` = ?, `modelid2` = 0, "
                   "`modelid3` = 0, `modelid4` = 0 WHERE `entry` = ?");
    update.addBindValue(display);
    update.addBindValue(entry);
    if (!update.exec())
    {
      error = "Could not save the legacy creature template display: "
        + update.lastError().text();
      return false;
    }
    return true;
  }
}
