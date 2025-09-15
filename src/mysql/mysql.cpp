// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <mysql/mysql.h>
#include <noggit/world.h>

#include <QSettings>
#include <QMessageBox>

#include <cppconn/driver.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/exception.h>

namespace
{
  std::unique_ptr<sql::Connection> connect()
  {
		QSettings settings;

		// if using release SQL binaries in debug mode it will crash https://bugs.mysql.com/bug.php?id=91238 unless using sql strings
		// tcp://127.0.0.1:3306
		const sql::SQLString hostname = "tcp://" + settings.value("project/mysql/server").toString().toStdString() + ":" + settings.value("project/mysql/port", "3306").toString().toStdString();
		const sql::SQLString userName = settings.value("project/mysql/user").toString().toStdString();
		const sql::SQLString password = settings.value("project/mysql/pwd").toString().toStdString();
		const sql::SQLString schema = settings.value("project/mysql/db").toString().toStdString();

		try
		{
			std::unique_ptr<sql::Connection> Con(get_driver_instance()->connect(hostname, userName, password));

			// crete database if it doesn't exist
			std::string createdb_statement = "CREATE DATABASE IF NOT EXISTS " + schema;
			std::unique_ptr<sql::PreparedStatement> dbpstmt(Con->prepareStatement(createdb_statement));
			std::unique_ptr<sql::ResultSet> res(dbpstmt->executeQuery());
			
			Con->setSchema(schema);

			// create table if it doesn't exist, querries from src/sql
			std::unique_ptr<sql::PreparedStatement> tablepstmt(Con->prepareStatement("CREATE TABLE IF NOT EXISTS `UIDs` ("
												"`_map_id` int(11) NOT NULL,"
												"`UID` int(11) NOT NULL,"
												"PRIMARY KEY(`_map_id`)"
												") ENGINE = InnoDB DEFAULT CHARSET = latin1;"));
			std::unique_ptr<sql::ResultSet> tableres(tablepstmt->executeQuery());

			return Con;
		}
		catch (sql::SQLException& e)
		{

			return nullptr;
		}
		catch (std::exception& e)
		{
			std::cerr << "SQL Other exception: " << e.what() << std::endl;
			return nullptr;
		}
  }
}

namespace mysql
{
  bool testConnection(bool report_only_err)
  {
	  QSettings settings;
	  // if using release SQL binaries in debug mode it will crash https://bugs.mysql.com/bug.php?id=91238 unless using sql strings
	  const sql::SQLString hostname = "tcp://" + settings.value("project/mysql/server").toString().toStdString() + ":" + settings.value("project/mysql/port", "3306").toString().toStdString();
	  const sql::SQLString userName = settings.value("project/mysql/user").toString().toStdString();
	  const sql::SQLString password = settings.value("project/mysql/pwd").toString().toStdString();

	  QMessageBox prompt;
	  prompt.setWindowFlag(Qt::WindowStaysOnTopHint);
	  try
	  {
			sql::Driver* driver = get_driver_instance();
			sql::Connection* connection = driver->connect(hostname, userName, password);
			std::unique_ptr<sql::Connection> Con(connection);

		  prompt.setIcon(QMessageBox::Information);
		  prompt.setText("Succesfully connected to MySQL database.");
		  prompt.setWindowTitle("Success");

		  if (!report_only_err)
			prompt.exec();

		  return true;
	  }
	  catch (sql::SQLException& e)
	  {
		  
		  prompt.setIcon(QMessageBox::Warning);
		  prompt.setText("Failed to load MySQL database, check your settings. \nIf you did not intend to use this feature, disable it in Noggit->settings->MySQL");
		  prompt.setWindowTitle("Noggit Database Error");
		  // disable if connection is not valid
		  // settings.value("project/mysql/enabled") = false;
		  std::stringstream promptText;

		  promptText << "\n# ERR: " << e.what();
		  promptText << "\n (MySQL error code: " << e.getErrorCode() + ")";

		  prompt.setInformativeText(promptText.str().c_str());
		  prompt.exec();

		  return false;
	  }
		catch (std::exception& e)
		{
			std::cerr << "SQL Other exception: " << e.what() << std::endl;
			return false;
		}
  }

  bool hasMaxUIDStoredDB(std::size_t mapID)
  {
	  auto Con(connect());
	  if (Con == nullptr)
		  return false;
	  std::unique_ptr<sql::PreparedStatement> pstmt(Con->prepareStatement("SELECT * FROM `UIDs` WHERE `_map_id`=(?)"));
	  pstmt->setInt(1, mapID);
	  std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
	  return res->rowsCount();
  }

  std::uint32_t getGUIDFromDB(std::size_t mapID)
  {
	  auto Con(connect());
	  if (Con == nullptr)
		  return 0;
	  std::unique_ptr<sql::PreparedStatement> pstmt(Con->prepareStatement("SELECT `UID` FROM `UIDs` WHERE `_map_id`=(?)"));
	  pstmt->setInt(1, mapID);
	  std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

	  std::uint32_t highGUID(0);
	  if (res->rowsCount() == 0) 
    { 
      return 0; 
    }
	  while (res->next())
	  {
		  highGUID = res->getInt(1);
	  }

	  return highGUID;
  }

  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID)
  {
	  auto Con(connect());
	  if (Con == nullptr)
		  return;
	  std::unique_ptr<sql::PreparedStatement> pstmt(Con->prepareStatement("INSERT INTO `UIDs` SET `_map_id`=(?), `UID`=(?)"));
	  pstmt->setInt(1, mapID);
	  pstmt->setInt(2, NewUID);
	  pstmt->executeUpdate();
  }

  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID)
  {
	  auto Con(connect());
	  if (Con == nullptr)
		  return;
	  std::unique_ptr<sql::PreparedStatement> pstmt(Con->prepareStatement("UPDATE `UIDs` SET `UID`=(?) WHERE `_map_id`=(?)"));
	  pstmt->setInt(1, NewUID);
	  pstmt->setInt(2, mapID);
	  pstmt->executeUpdate();
  }
}
