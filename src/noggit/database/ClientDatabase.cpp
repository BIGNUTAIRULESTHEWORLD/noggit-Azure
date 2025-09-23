#include "ClientDatabase.h"
#include <noggit/project/CurrentProject.hpp>
#include <noggit/application/Utils.hpp>
#include <noggit/Log.h>

#include <QString>
#include <QSqlRecord>
#include <QSqlField>
#include <QElapsedTimer>
#include <QSettings>

namespace Noggit
{
	// namespace Sql
		auto escapeSqlString = [](const QString& str) -> QString {
			QString result = str;
			result.replace("'", "''"); // double single quotes for SQL
			return "'" + result + "'";
			};


		void ClientDatabase::setDatabaseMode(DatabaseMode mode)
		{
			_database_mode = mode;
		}

		DatabaseMode ClientDatabase::_database_mode = DatabaseMode::ClientStorage;
		DatabaseMode ClientDatabase::databaseMode()
		{
			return _database_mode;
		}

		ClientDatabaseTable ClientDatabase::getTable(const std::string& tableName)
		{
			return ClientDatabaseTable(tableName);
		}

	bool ClientDatabaseTable::UploadDBCtoDB()
	{

		auto sql_table_name = getSqlTableName();

		/*
		if (!verifySqlTableIntegrity())
		{
			Log << "Table " << sql_table_name << "does not exist or has wrong structure.";
			qDebug() << "Table " << sql_table_name.c_str() << "does not exist or has wrong structure.";
			return false;
		}*/
		qDebug() << "Populating empty Table " << sql_table_name.c_str();

		// insert if fresh_table, otherwise replace?

		auto& row_definition = GetRecordDefinition();
		auto sql_record_format = recordFormat();

		auto client_table_iterator = getClientTable().Records();

		// empty table, nothing to insert
		if (!client_table_iterator.HasRecords())
			return false;

		QStringList column_names;
		for (auto& sql_column_format : sql_record_format)
		{
			column_names.append(sql_column_format.Name.c_str());
		}
		int colCount = column_names.size();

		auto& db_mgr = Noggit::Sql::SqlDatabaseManager::instance();
		auto noggit_db = db_mgr.noggitDatabase();
		QSqlQuery query(noggit_db);

		// Start bulk insert ///////////////////////////////
		QElapsedTimer timer;
		timer.start();

		const int batchSize = 2000;
		int rowCount = 0;

		noggit_db.transaction();

		// query.exec("SET UNIQUE_CHECKS=0;");

		QStringList rowBuffer; // holds each row as a string
		rowBuffer.reserve(batchSize);

		while (client_table_iterator.HasRecords())
		{
			auto& record = client_table_iterator.Next();
			QStringList colValues;
			colValues.reserve(column_names.size());

			for (auto& column_def : row_definition.ColumnDefinitions)
			{
				if (column_def.Type == "int" && column_def.isID)
				{
					colValues.append(QString::number(record.RecordId));
					continue;
				}
				auto& rowColumn = record.Columns.at(column_def.Name);
				if (column_def.Type == "locstring")
				{
					for (int i = 0; i < 16; ++i)
						colValues.append(escapeSqlString(QString::fromStdString(rowColumn.Values[i])));

					auto& flagValue = record.Columns.at(column_def.Name + "_flags").Value;
					colValues.append(QString::fromStdString(flagValue));
				}
				else
				{
					int len = (column_def.arrLength > 1) ? column_def.arrLength : 1;
					for (int i = 0; i < len; ++i)
					{
						if (column_def.Type == "string")
							colValues.append(escapeSqlString((len > 1) ? QString::fromStdString(rowColumn.Values[i])
								: QString::fromStdString(rowColumn.Value)));
						else // int/float
							colValues.append((len > 1) ? QString::fromStdString(rowColumn.Values[i])
								: QString::fromStdString(rowColumn.Value));
					}
				}
			}

			// append row as a single string
			rowBuffer.append("(" + colValues.join(",") + ")");
			rowCount++;

			// flush batch
			if (rowCount % batchSize == 0)
			{
				int min_size = rowCount * ((column_names.size()*2) + 2); // 2 chars minimum per column (value and comma)
				// qDebug() << "min size" << min_size;
				QString sql;
				sql.reserve(min_size);
				sql = QString("INSERT INTO `%1` (%2) VALUES ")
					.arg(sql_table_name.c_str())
					.arg(column_names.join(", "));
				sql += rowBuffer.join(",");

				if (!query.exec(sql))
				{
					qWarning() << "Batch insert failed:" << query.lastError().text();
					// query.exec("SET UNIQUE_CHECKS=1;");
					noggit_db.rollback();
					return false;
				}
				rowBuffer.clear();
				rowCount = 0;
			}
		}

		// flush remaining rows
		if (!rowBuffer.isEmpty())
		{
			int min_size = rowCount * ((column_names.size() * 2) + 2);
			QString sql;
			sql.reserve(min_size);
			sql = QString("INSERT INTO `%1` (%2) VALUES ")
				.arg(sql_table_name.c_str())
				.arg(column_names.join(", "));
			sql += rowBuffer.join(",");

			if (!query.exec(sql))
			{
				qWarning() << "Final batch insert failed:" << query.lastError().text();
				// query.exec("SET UNIQUE_CHECKS=1;");
				noggit_db.rollback();
				return false;
			}
		}
		// query.exec("SET UNIQUE_CHECKS=1;");

		noggit_db.commit();

		// benchmark
		qint64 elapsedMs = timer.elapsed();
		qDebug() << "Inserted" << getClientTable().RecordCount() << "rows in" << elapsedMs << "ms ("
			<< (getClientTable().RecordCount() * 1000.0 / elapsedMs) << " rows/sec)";

		Log << "Inserted " << getClientTable().RecordCount() << " rows in " << elapsedMs << "ms ("
			<< (getClientTable().RecordCount() * 1000.0 / elapsedMs) << " rows/sec)" << std::endl;

		return true;
	}

	// executes query in client db and checks errors
	// use isActive to check if it properly ran, not isValid.
	QSqlQuery ClientDatabase::executeQuery(const QString& sql, bool forward_only)
	{
		auto db_mgr = Noggit::Sql::SqlDatabaseManager::instance().noggitDatabase();
		QSqlQuery query(Noggit::Sql::SqlDatabaseManager::instance().noggitDatabase());

		qDebug() << "Executing query : " << sql;
		QElapsedTimer timer;
		timer.start();
		if (forward_only) // call this for browsing large data sets
			query.setForwardOnly(true);
		if (!query.exec(sql))
		{
			LogError << "SQL query failed:" << query.lastError().text().toStdString();
			LogError << "Query:" << sql.toStdString();
			// throw SqlException("Query failed: " + query.lastError().text() + "\nQuery: " + sql);
			assert(false);
			return QSqlQuery(); // invalid query
		}

		qint64 elapsedMs = timer.elapsed();
		qDebug() << "Executed query in " << elapsedMs << "ms";

		return query;
	}

	bool ClientDatabaseTable::createSQLTableIfNotExist()
	{
		auto row_definition = GetRecordDefinition();

		const std::string sql_table_name = getSqlTableName();

		auto db_record_format = recordFormat();
		assert(db_record_format.size() == getClientTable().ColumnCount());

		std::string statement = std::format("CREATE TABLE IF NOT EXISTS `{}` (", sql_table_name);

		std::string primary_key_name;
		for (auto& db_column_format : db_record_format)
		{
			statement += std::format("`{}` {}", db_column_format.Name, db_column_format.Type);

			if (db_column_format.Type == "TEXT")
			{
				statement += " NULL"; // allow NULL by default
			}
			else
			{
				if (!db_column_format.isSigned && db_column_format.Type == "INT")
				{
					// assert(db_column_format.Type == "INT");
					statement += " UNSIGNED";		// only allow int to be unsigned?
				}
				statement += " NOT NULL"; // allow text to be nulled
				statement += " DEFAULT 0";
			}

			statement += ",\n";

			if (db_column_format.isID)
			{
				assert(primary_key_name.empty()); // more than one key ? TODO
				primary_key_name = db_column_format.Name;
			}
		}

		if (!primary_key_name.empty())
			statement += std::format("PRIMARY KEY (`{}`)", primary_key_name);

		// Add indexes for relations
		for (auto& db_column_format : db_record_format)
		{
			if (db_column_format.isRelation && !db_column_format.isID) {
				statement += std::format(",\nINDEX (`{}`)", db_column_format.Name);
			}
		}

		// statement += ")\n ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 DEFAULT COLLATE='utf8mb4_general_ci';";
		statement += ")\n ENGINE = InnoDB;";

		auto& db_mgr = Noggit::Sql::SqlDatabaseManager::instance();
		bool valid_conn = db_mgr.testConnection(Noggit::Sql::SQLDbType::Noggit);
		if (!valid_conn)
			return false;
		auto noggit_db = db_mgr.noggitDatabase();

		QSqlQuery query(noggit_db);
		bool success = query.exec(QString::fromStdString(statement));

		if (!success)
		{
			qDebug() << "Failed to create table:" << query.lastError().text();
		}
		else
		{
			qDebug() << "Table " << sql_table_name.c_str() << " created.";

			UploadDBCtoDB();
		}

		return success;
	}

	std::vector<DbColumnFormat> ClientDatabaseTable::recordFormat() const
	{
		auto record_format = std::vector<DbColumnFormat>();

		auto& row_definition = GetRecordDefinition();
		for (int col_idx = 0; col_idx < row_definition.ColumnDefinitions.size(); col_idx++)
		{
			auto& column_def = row_definition.ColumnDefinitions[col_idx];

			bool is_locstring = false;

			// convert dbd definition type names to real format
			// TODO : map types
			std::string sql_data_type = "INT";
			if (BlizzardDatabaseLib::Extension::String::Compare(column_def.Type, "int"))
			{
				sql_data_type = "INT";
			}
			else if (BlizzardDatabaseLib::Extension::String::Compare(column_def.Type, "float"))
			{
				sql_data_type = "FLOAT";
			}
			else if (BlizzardDatabaseLib::Extension::String::Compare(column_def.Type, "string"))
			{
				sql_data_type = "TEXT";
			}
			else if (BlizzardDatabaseLib::Extension::String::Compare(column_def.Type, "locstring"))
			{
				sql_data_type = "TEXT";
				is_locstring = true;
			}
			else
				assert(false);

			int array_size = 1;
			if (column_def.arrLength > 1)
			{
				array_size = column_def.arrLength;
			}
			if (is_locstring)
				array_size = 16;

			for (int i = 0; i < array_size; i++)
			{
				DbColumnFormat db_col_format;
				std::string col_name = "";

				if (array_size == 1)
				{
					col_name = column_def.Name;
				}
				else if (is_locstring)
				{
					col_name = std::format("{}_{}", column_def.Name, dbc_string_loc_names[i]); // {MapName_lang}_{enUS}
				}
				else if (array_size > 1)
				{
					col_name = std::format("{}_{}", column_def.Name, i); // {MapName}_{0}
				}
				db_col_format.Name = col_name;
				db_col_format.Type = sql_data_type;

				assert(!(column_def.isID && array_size > 1));
				db_col_format.isID = column_def.isID;
				db_col_format.isRelation = column_def.isRelation;
				db_col_format.isSigned = column_def.isSigned;

				record_format.push_back(db_col_format);
			}
			if (is_locstring) // add lang mask column
			{
				DbColumnFormat db_col_format;
				db_col_format.Name = std::format("{}_flags", column_def.Name);
				db_col_format.Type = "INT";
				db_col_format.isSigned = false;
				db_col_format.isID = false;
				db_col_format.isRelation = false;
				record_format.push_back(db_col_format);
			}
		}

		return record_format;
	}

	bool ClientDatabaseTable::verifySqlTableIntegrity()
	{
		auto& db_mgr = Noggit::Sql::SqlDatabaseManager::instance();
		bool valid_conn = db_mgr.testConnection(Noggit::Sql::SQLDbType::Noggit);
		if (!valid_conn)
			return false;

		// check if table exists
		QString sql_table_name = getSqlTableName().c_str();

		auto noggit_db = db_mgr.noggitDatabase();

		// table integrity check
		bool table_is_valid = true;
		bool fresh_table = false;

		// noggit_db.tables().contains(sql_table_name) is bugged with current qt version and mysql 8
		QSqlQuery query_show(noggit_db);
		if (!query_show.exec("SHOW TABLES"))
		{
			qWarning() << "Failed to list tables:" << query_show.lastError().text();
			return false;
		}
		QStringList tables;
		while (query_show.next())
		{
			tables << query_show.value(0).toString();
		}

		if (tables.contains(sql_table_name))
		{
			// this is also bugged...
			// QSqlRecord sql_rec = noggit_db.record(sql_table_name);
			// if (sql_rec.isEmpty())
			// {
			// 	table_is_valid = false;
			// }
			// else
			// {
			// 	// TODO verify db structure, just column count for now
			// 	if (table.ColumnCount() != sql_rec.count())
			// 	{
			// 		assert(false);
			// 		table_is_valid = false;
			// 	}
			// }
		}
		else // table doesn't exist
		{
			// create table
			table_is_valid = createSQLTableIfNotExist();
			fresh_table = true;
		}


		return table_is_valid;
	}

	// Structures::BlizzardDatabaseRow ClientDatabaseTable::sqlRecordToDatabaseRow(const QSqlRecord& record) const
	Structures::BlizzardDatabaseRow ClientDatabaseTable::sqlRecordToDatabaseRow(QSqlQuery& record) const

	{
		auto& row_definition = GetRecordDefinition();

		auto database_row = Structures::BlizzardDatabaseRow(-1);

		int Id = -1;
		int field_idx = 0;
		for (int column_def_idx = 0; column_def_idx < row_definition.ColumnDefinitions.size(); ++column_def_idx)
		{
			auto& column_def = row_definition.ColumnDefinitions[column_def_idx];
			auto database_column = Structures::BlizzardDatabaseColumn();

			if (column_def.Type == "locstring")
			{
				database_column.Values.resize(16);
				for (int loc_idx = 0; loc_idx < 16; loc_idx++)
				{
					database_column.Values[loc_idx] = (record.value(field_idx++).toString().toStdString());
				}
				// currently loc mask is set to a separate column because wdbc reader does it.
				auto loc_mask_column = Structures::BlizzardDatabaseColumn();
				loc_mask_column.Value = record.value(field_idx++).toString().toStdString();
				database_row.Columns[column_def.Name + "_flags"] = loc_mask_column;
			}
			else // every other type than locstring
			{
				if (column_def.arrLength > 1) // array
				{
					database_column.Values.resize(column_def.arrLength);
					for (int i = 0; i < column_def.arrLength; i++)
					{
						database_column.Values[i] = (record.value(field_idx++).toString().toStdString());
					}
				}
				else // single value
				{
					database_column.Value = record.value(field_idx++).toString().toStdString();

					if (column_def.isID)
						Id = std::stoi(database_column.Value);
				}
			}
			database_row.Columns[column_def.Name] = std::move(database_column);
		}
		assert(Id != -1); // no id found
		database_row.RecordId = Id;

		return database_row;
	}

	ClientDatabaseTable::ClientDatabaseTable(std::string tableName)
		: _tableName(tableName), _qtTableName(tableName.c_str())
	{
		if (ClientDatabase::databaseMode() == DatabaseMode::Sql)
			verifySqlTableIntegrity(); // verifySqlTableIntegrity()->createtableifnotexists()->UploadDBCtoDB()
	};

	unsigned int ClientDatabaseTable::RecordCount() const
	{
		unsigned int client_count = getClientTable().RecordCount();

		if (ClientDatabase::databaseMode() == DatabaseMode::Sql)
		{
			QString sql = QString("SELECT COUNT(*) FROM `%1`").arg(getSqlTableName().c_str());
			QSqlQuery query = ClientDatabase::executeQuery(sql);

			if (query.isActive())
			{
				if (query.next())
					assert(query.value(0).toUInt() == client_count);
			}
		}
		return client_count;
	}

	int ClientDatabaseTable::ColumnCount() const
	{
		// get from parsed definition
		int def_column_count = recordFormat().size();

		int client_count = getClientTable().ColumnCount();
		assert(def_column_count == client_count);

		if (ClientDatabase::databaseMode() == DatabaseMode::Sql)
		{
			auto db = Noggit::Sql::SqlDatabaseManager::instance().noggitDatabase();
			QSqlRecord rec = db.record(QString::fromStdString(getSqlTableName()));
			int db_count = rec.count();
			assert(db_count == def_column_count);
		}

		return def_column_count;
	}

	std::optional<Structures::BlizzardDatabaseRow> ClientDatabaseTable::RecordById(unsigned int id) const
	{
		auto row = Structures::BlizzardDatabaseRow(-1);

		if (ClientDatabase::databaseMode() == DatabaseMode::Sql)
			row = sqlRowById(id);
		else
			row = clientRowById(id);

		if (row.RecordId == -1)
			return std::nullopt;
		else
			return row;
	}

	Noggit::DatabaseRecordCollection ClientDatabaseTable::Records() const
	{
		return Noggit::DatabaseRecordCollection(*this);
	};

	/*
	std::optional<Structures::BlizzardDatabaseRow> ClientDatabaseTable::RecordByPosition(unsigned int positionId) const
	{
		// TODO
		auto row = Structures::BlizzardDatabaseRow(-1);
		if (ClientDatabase::databaseMode() == DatabaseMode::Sql)
		{
			// We shouldn't do this with SQL table
		}
		else
			row = getClientTable().RecordByPosition(positionId);

		return std::optional<Structures::BlizzardDatabaseRow>();
	}*/

	Structures::BlizzardDatabaseRowDefinition& ClientDatabaseTable::GetRecordDefinition() const
	{
		return Noggit::Project::CurrentProject::get()->ClientDatabase->TableRecordDefinition(_tableName);
	}

	BlizzardDatabaseLib::BlizzardDatabaseTable& ClientDatabaseTable::getClientTable() const
	{
		return Noggit::Project::CurrentProject::get()->ClientDatabase->LoadTable(_tableName, readFileAsIMemStream);
	}

	// get from local dbc data memory stream in BlizzardDatabaseLib::BlizzardDatabase
	Structures::BlizzardDatabaseRow ClientDatabaseTable::clientRowById(unsigned int id) const
	{
		auto record = getClientTable().RecordById(id);
		return record;
	}

	// get from SQL request to noggit db
	// never use this function for more than 1 rows, implement a new bulk function
	Structures::BlizzardDatabaseRow ClientDatabaseTable::sqlRowById(unsigned int id) const
	{
		QString sql_table_name = getSqlTableName().c_str();
		QString sql = QString("SELECT * FROM `%1` WHERE ID = %2").arg(sql_table_name).arg(id);

		auto query = ClientDatabase::executeQuery(sql);

		if (!query.isActive())
			return BlizzardDatabaseLib::Structures::BlizzardDatabaseRow(-1);

		// auto row_definition = GetRecordDefinition();

		if (query.next())
		{
			// QSqlRecord record = query.record(); // slow af
			auto database_row = sqlRecordToDatabaseRow(query);

			return database_row;
		}
		else
		{
			LogError << "SQL : No row found in" << sql_table_name.toStdString() << "for ID =" << id;
			qWarning() << "SQL : No row found in" << sql_table_name << "for ID =" << id;
			return BlizzardDatabaseLib::Structures::BlizzardDatabaseRow();
		}

	}
	
	const std::string ClientDatabaseTable::getSqlTableName(unsigned int build_id) const
	{
		if (build_id == 0)
			build_id = Noggit::Project::CurrentProject::get()->buildId();

		std::string table = std::format("db_{}_{}", _tableName, build_id);

		// convert to lowercase for compatibility with SQL
		std::transform(table.begin(), table.end(), table.begin(),
			[](unsigned char c) { return std::tolower(c); });

		return table;
	}

	DatabaseRecordCollection::DatabaseRecordCollection(const ClientDatabaseTable& table)
		:_table(table), /*_mode(mode),*/ _client_iterator(_table.getClientTable().Records())
	{
		if (ClientDatabase::databaseMode() == DatabaseMode::Sql)
		{
			QString sql = QString("SELECT * FROM `%1`").arg(_table.getSqlTableName().c_str()); //  ORDER BY ID ?

			_query = ClientDatabase::executeQuery(sql, true);
			_querry_valid = _query.isActive(); // if query.exec ran properly

			querryAdvance();
		}
	}

	bool DatabaseRecordCollection::HasRecords()
	{
		if (ClientDatabase::databaseMode() == DatabaseMode::ClientStorage)
			return _client_iterator.HasRecords();
		else
		{
			return _querry_valid && _hasNext;
		}
	}

	Structures::BlizzardDatabaseRow DatabaseRecordCollection::Next()
	{
		if (ClientDatabase::databaseMode() == DatabaseMode::ClientStorage)
			return _client_iterator.Next();
		else
		{
			if (!_querry_valid || !_hasNext)
			{
				assert(false);
				return Structures::BlizzardDatabaseRow(); // empty
			}

			// always store one row in advance to know if it's the last one
			// auto row = _table.sqlRecordToDatabaseRow(_nextRecord);
			assert(_nextRecord.RecordId != -1);

			querryAdvance();

			return _nextRecord;
		}

	}

	void DatabaseRecordCollection::querryAdvance()
	{
		if (_query.next())
		{
			// _nextRecord = _query.record();
			_nextRecord = _table.sqlRecordToDatabaseRow(_query);
			_hasNext = true;
		}
		else
		{
			_hasNext = false;
		}
	}

}