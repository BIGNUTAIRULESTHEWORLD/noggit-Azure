#include "SqlUIDStorage.h"

bool Noggit::Sql::SqlUIDStorage::hasMaxUIDStoredDB(std::size_t mapID)
{
	auto& db_mgr = SqlDatabaseManager::instance();

	bool valid_conn = db_mgr.testConnection(SQLDbType::Noggit);
	if (!valid_conn)
		return false;

	auto noggit_db = db_mgr.noggitDatabase();

	QSqlQuery query(noggit_db);
	query.prepare(QStringLiteral("SELECT * FROM `UIDs` WHERE `_map_id` = ?"));
	query.addBindValue(static_cast<int>(mapID));

	if (!query.exec())
	{
		qWarning() << "Failed to check UIDs:" << query.lastError().text();
		return false;
	}

	return query.next(); // true if at least one row exists
}

std::uint32_t Noggit::Sql::SqlUIDStorage::getGUIDFromDB(std::size_t mapID)
{
	auto& db_mgr = Sql::SqlDatabaseManager::instance();

	bool valid_conn = db_mgr.testConnection(SQLDbType::Noggit);
	if (!valid_conn)
		return 0;

	QSqlDatabase noggit_db = db_mgr.noggitDatabase();
	/////
	QSqlQuery query(noggit_db);
	query.prepare(QStringLiteral("SELECT `UID` FROM `UIDs` WHERE `_map_id` = ?"));
	query.addBindValue(static_cast<int>(mapID));

	if (!query.exec())
	{
		qWarning() << "Failed to fetch UID from db:" << query.lastError().text();
		return 0;
	}

	if (query.next())
		return query.value(0).toUInt();

	return 0; // no rows

	// Optional, find highest GUID of all maps.
	// "SELECT `UID` FROM `UIDs`"
	// std::uint32_t highGUID(0);
	// if (res->rowsCount() == 0)
	// {
	// 	return 0;
	// }
	// while (res->next())
	// {
	// 	highGUID = res->getInt(1);
	// }
	// 
	// 
	// return highGUID;
}

void Noggit::Sql::SqlUIDStorage::insertUIDinDB(std::size_t mapID, std::uint32_t NewUID)
{
	auto& db_mgr = Sql::SqlDatabaseManager::instance();
	bool valid_conn = db_mgr.testConnection(SQLDbType::Noggit);
	if (!valid_conn)
	{
			return;
	}

	QSqlDatabase noggit_db = db_mgr.noggitDatabase();

	QSqlQuery query(noggit_db);
	query.prepare(QStringLiteral("INSERT INTO `UIDs` (`_map_id`, `UID`) VALUES (?, ?)"));
	query.addBindValue(static_cast<int>(mapID));
	query.addBindValue(static_cast<int>(NewUID));

	if (!query.exec())
	{
		qWarning() << "Failed to insert UID in db: " << query.lastError().text();
	}
}

void Noggit::Sql::SqlUIDStorage::updateUIDinDB(std::size_t mapID, std::uint32_t NewUID)
{
	auto& db_mgr = Sql::SqlDatabaseManager::instance();

	if (!db_mgr.testConnection(SQLDbType::Noggit))
		return;

	QSqlDatabase noggit_db = db_mgr.noggitDatabase();

	QSqlQuery query(noggit_db);
	query.prepare(QStringLiteral("UPDATE `UIDs` SET `UID` = ? WHERE `_map_id` = ?"));
	query.addBindValue(static_cast<int>(NewUID));
	query.addBindValue(static_cast<int>(mapID));

	if (!query.exec())
	{
		qWarning() << "Failed to update UID:" << query.lastError().text();
	}
}
