#include "ClientDatabase.h"
#include <noggit/project/CurrentProject.hpp>
#include <noggit/application/Utils.hpp>
#include <noggit/Log.h>

#include <QString>
#include <QSqlRecord>
#include <QSqlField>
#include <QElapsedTimer>

namespace Noggit
{
	// namespace Sql
		auto escapeSqlString = [](const QString& str) -> QString {
			QString result = str;
			result.replace("'", "''"); // double single quotes for SQL
			return "'" + result + "'";
			};


	std::optional<Structures::BlizzardDatabaseRow> ClientDatabase::getRowById(const std::string& tableName, unsigned int id)
	{
		bool setting_use_sql_db = false; // todo QSETTING

		auto row = Structures::BlizzardDatabaseRow(-1);

		if (setting_use_sql_db)
			row = sqlRowById(tableName, id);
		else
			row = clientRowById(tableName, id);

		if (row.RecordId == -1)
			return std::nullopt;
		else
			return row;
	}

	bool ClientDatabase::testUploadDBCtoDB(const BlizzardDatabaseLib::BlizzardDatabaseTable& table)
	{
		auto& db_mgr = Noggit::Sql::SqlDatabaseManager::instance();
		bool valid_conn = db_mgr.testConnection(Noggit::Sql::SQLDbType::Noggit);
		if (!valid_conn)
			return false;

		auto table_name = table.Name();
		// Noggit::Project::CurrentProject::get()->projectVersion; // expension, not exact build id
		unsigned int build_id = Noggit::Project::CurrentProject::get()->buildId();

		// check if table exists
		QString sql_table_name = getSqlTableName(table_name, build_id).c_str();

		auto noggit_db = db_mgr.noggitDatabase();

		// table integrity check
		bool table_is_valid = true;
		bool fresh_table = false;
		QSqlRecord sql_rec = noggit_db.record(sql_table_name);

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
			table_is_valid = createSQLTableIfNotExist(table);
			fresh_table = true;
		}

		if (!table_is_valid)
		{
			Log << "Table " << sql_table_name.toStdString() << "does not exist or has wrong structure.";
			qDebug() << "Table " << sql_table_name << "does not exist or has wrong structure.";
			return false;
		}

		// insert if fresh_table, otherwise replace?

		auto row_definition = table.GetRecordDefinition();
		auto sql_record_format = recordFormat(table_name);

		auto client_table_iterator = table.Records();

		// empty table, nothing to insert
		if (!client_table_iterator.HasRecords())
			return false;

		QStringList column_names;
		for (auto& sql_column_format : sql_record_format)
		{
			column_names.append(sql_column_format.Name.c_str());
		}
		int colCount = column_names.size();


		// Start bulk insert ///////////////////////////////
		QElapsedTimer timer;
		timer.start();

		const int batchSize = 2000;
		int rowCount = 0;

		QSqlQuery query(noggit_db);

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
					.arg(sql_table_name)
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
				.arg(sql_table_name)
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
		qDebug() << "Inserted" << table.RecordCount() << "rows in"
			<< elapsedMs << "ms ("
			<< (table.RecordCount() * 1000.0 / elapsedMs) << " rows/sec)";

		Log << "Inserted " << table.RecordCount() << " rows in "
			<< elapsedMs << "ms ("
			<< (table.RecordCount() * 1000.0 / elapsedMs) << " rows/sec)" << std::endl;

		return true;
	}

	// get from local dbc data memory stream in BlizzardDatabaseLib::BlizzardDatabase
	Structures::BlizzardDatabaseRow ClientDatabase::clientRowById(const std::string& tableName, unsigned int id)
	{
		auto& table = Noggit::Project::CurrentProject::get()->ClientDatabase->LoadTable(tableName, readFileAsIMemStream);
		auto record = table.RecordById(id);

		return record;
	}

	// get from SQL request to noggit db
	// never use this function for more than 1 rows, implement a new bulk function
	Structures::BlizzardDatabaseRow ClientDatabase::sqlRowById(const std::string& tableName, unsigned int id)
	{
		auto& db_mgr = Noggit::Sql::SqlDatabaseManager::instance();
		// Test connection ?

		auto noggit_db = db_mgr.noggitDatabase();

		 auto row_definition = Noggit::Project::CurrentProject::get()->ClientDatabase->TableRecordDefinition(tableName);

		QString sql_table_name = getSqlTableName(tableName).c_str();
		QSqlQuery query(noggit_db);
		QString sql = QString("SELECT * FROM %1 WHERE ID = :id").arg(sql_table_name);

		query.prepare(sql);
		query.bindValue(":id", id);

		if (!query.exec())
		{
			qWarning() << "Query exec failed:" << query.lastError().text();
			return BlizzardDatabaseLib::Structures::BlizzardDatabaseRow();
		}
		
		if (query.next())
			{					
				QSqlRecord record = query.record();

				auto database_row = Structures::BlizzardDatabaseRow(id);

				// test db def/////////////////////////
				{
						auto record_db_def = recordFormat(tableName);
						if (record.count() != record_db_def.size())
						{
							// error : definition deosn't match db structure
							assert(false);
							return Structures::BlizzardDatabaseRow(-1);
						}

						// Tests construct row from query 
						// TODOOOOOOOOOOOOOOO

						for (int i = 0; i < record_db_def.size(); ++i)
						{
							auto& column_db_def = record_db_def[i];
							QSqlField db_field = record.field(i);

							assert(column_db_def.Name == record.fieldName(i).toStdString());
							// assert(column_db_def.Type == db_field.type());
						}
				}/////////////////////////////////////////

				int field_idx = 0;
				for (int column_def_idx = 0; column_def_idx < row_definition.ColumnDefinitions.size(); ++column_def_idx)
				{
					auto& column_def = row_definition.ColumnDefinitions[column_def_idx];
					auto database_column = Structures::BlizzardDatabaseColumn();

					auto value = std::string();
					if (column_def.Type == "locstring")
					{
						std::vector<std::string> localizedValues = std::vector<std::string>();
						for (int loc_idx = 0; loc_idx < 16; loc_idx++)
						{
							localizedValues.push_back(query.value(field_idx++).toString().toStdString());
						}

						database_column.Values = localizedValues;
						database_row.Columns[column_def.Name] = database_column;

						// currently loc mask is set to a separate column because wdbc reader does it.
						auto loc_mask_column = Structures::BlizzardDatabaseColumn();
						loc_mask_column.Value = query.value(field_idx++).toString().toStdString();
						database_row.Columns[column_def.Name + "_flags"] = loc_mask_column;
					}
					else // every other type than locstring
					{
						if (column_def.arrLength > 1) // array
						{
							for (int i = 0; i < column_def.arrLength; i++)
							{
								auto Value = query.value(field_idx++);
								database_column.Values.push_back(Value.toString().toStdString());
							}
						}
						else // single value
						{
							auto Value = query.value(field_idx++);
							value = Value.toString().toStdString();
						}
					}
					database_column.Value = value;
					database_row.Columns[column_def.Name] = database_column;
				}

				return database_row;
			}
		else
		{
			qWarning() << "No row found in" << tableName.c_str() << "for ID =" << id;
			return BlizzardDatabaseLib::Structures::BlizzardDatabaseRow();
		}
		
	}

	bool ClientDatabase::createSQLTableIfNotExist(const BlizzardDatabaseLib::BlizzardDatabaseTable& table)
	{
		const std::string table_name = table.Name();
		auto row_definition = table.GetRecordDefinition();

		const std::string sql_table_name = getSqlTableName(table_name);
		std::string statement = std::format("CREATE TABLE IF NOT EXISTS `{}` (", sql_table_name);

		std::string primary_key_name;

		auto db_record_format = recordFormat(table_name);

		assert(db_record_format.size() == table.ColumnCount());

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
			qDebug() << "Table " << table_name.c_str() << " created.";
		}

		return success;
	}

	std::string ClientDatabase::getSqlTableName(const std::string& db_name, unsigned int build_id)
	{
		if (build_id == 0)
			build_id = Noggit::Project::CurrentProject::get()->buildId();

		std::string table = std::format("db_{}_{}", db_name, build_id);

		// convert to lowercase for compatibility with SQL
		std::transform(table.begin(), table.end(), table.begin(),
			[](unsigned char c) { return std::tolower(c); });

		return table;
	}

	std::vector<DbColumnFormat> ClientDatabase::recordFormat(const std::string& table_name)
	{
		auto record_format = std::vector<DbColumnFormat>();

		auto row_definition = Noggit::Project::CurrentProject::get()->ClientDatabase->TableRecordDefinition(table_name);

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

}