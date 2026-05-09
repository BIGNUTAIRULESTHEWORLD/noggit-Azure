#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QSqlError>
#include <QDebug>
#include <QSqlQuery>
#include <QMap>
#include <QStringList>
#include <QMessageBox>

// Connections are currently not thread safe ! would need temporary connection copies per thread.
namespace Noggit::Sql
{
  enum class SQLDbType
  {
    Noggit,
    World
  };
  
  enum class SqlDriver
  {
    MySQL,
    // SQLite,
    // PostgreSQL
  };

  struct DbConfig
  {
    QString host;
    int port = 3306;
    QString dbName;
    QString user;
    QString password;
  };

  class SqlDatabaseManager
  {
  public:
    static SqlDatabaseManager& instance()
    {
      static SqlDatabaseManager _instance;
      return _instance;
    }
  
    bool initializeDbConnection(SQLDbType type,
      const QString& host, int port,
      const QString& dbName, const QString& user, const QString& password)
    {
      DbConfig newConfig{ host, port, dbName, user, password };

      // If config is unchanged and connection is alive, nothing to do
      if (configs[type].host == host &&
        configs[type].port == port &&
        configs[type].dbName == dbName &&
        configs[type].user == user &&
        configs[type].password == password &&
        testConnection(type))
      {
        return true;
      }
      configs[type] = newConfig;

      QString connName = connectionName(type);

      // remove old connection if it exists
      if (QSqlDatabase::contains(connName))
      {
        QSqlDatabase oldDb = QSqlDatabase::database(connName);
        if (oldDb.isOpen())
          oldDb.close();
        QSqlDatabase::removeDatabase(connName);
      }

      // Create new connection
      QSqlDatabase db = QSqlDatabase::addDatabase(driverToString(SqlDriver::MySQL), connName);
      db.setHostName(host);
      db.setPort(port);
      // db.setDatabaseName(dbName);
      db.setUserName(user);
      db.setPassword(password);

      // connect to my sql without a database
      if (!db.open())
      {
        qWarning() << "Failed to connect to database " << dbName << "on host" << host << ":" << db.lastError().text();
        return false;
      }

      // Create database if it doesn't exist
      QSqlQuery query(db);
      if (!query.exec(QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1`").arg(dbName)))
      {
        qWarning() << "Failed to open database " << dbName << ":" << db.lastError().text();
        return false;
      }

      // now attempt to connect to db
      // Need to close connection first because it needs to create the connection with a databasename.
      db.close();
      db.setDatabaseName(dbName);
      if (!db.open())
      {
        qDebug() << "Failed to open database:" << db.lastError().text();
        return false;
      }
      qDebug() << db.tables();

      return true;
    }
  
    // requires database to be initialized
    bool testConnection(SQLDbType type) const
    {
      if (!configs.contains(type))
      {
        qWarning() << "No configuration for database type" << static_cast<int>(type);
      }

      QSqlDatabase db = databaseConnection(type);
      if (db.isValid() && db.isOpen())
        return true;

      // If the connection is invalid or closed, attempt to open it
      if (!db.open())
      {
        qWarning() << "Connection test failed for" << db.connectionName() << ":" << db.lastError().text();
        qDebug() << db.lastError().nativeErrorCode();
        qDebug() << db.lastError().databaseText();
        return false;
      }
      else
      {
        qDebug() << "Reconnected to database" << db.connectionName();
      }

      return true;
    }
  
    bool testConnectionWithPopup(SQLDbType type, bool report_only_error) const
    {
      QMessageBox prompt;
      prompt.setWindowFlag(Qt::WindowStaysOnTopHint);
      bool success = false;
      std::stringstream promptText;
      try
      {
        success = testConnection(type);
      }
      catch (std::exception& e)
      {
        success = false;

        promptText << "\n# ERR: " << e.what();
      }

      if (success && !report_only_error)
      {
        prompt.setIcon(QMessageBox::Information);
        prompt.setText("Succesfully connected to MySQL database.");
        prompt.setWindowTitle("Success");

        if (!report_only_error)
          prompt.exec();
      }
      else if (!success)
      {
        prompt.setIcon(QMessageBox::Warning);
        prompt.setText("Failed to load MySQL database, check your settings. \nIf you did not intend to use this feature, disable it in Noggit->settings->MySQL");
        prompt.setWindowTitle("Noggit Database Error");
        // disable if connection is not valid
        // settings.value("project/mysql/client_db_enabled") = false;

        promptText << databaseConnection(type).lastError().text().toStdString() << std::endl;
        promptText << databaseConnection(type).lastError().nativeErrorCode().toStdString() << std::endl;
        promptText << databaseConnection(type).lastError().databaseText().toStdString() << std::endl;

        prompt.setInformativeText(promptText.str().c_str());
        prompt.exec();
      }

      return success;
    }

    bool isDriverAvailable()
    {
      QStringList availableDrivers = QSqlDatabase::drivers();
      return availableDrivers.contains(driverToString(_sql_driver), Qt::CaseInsensitive);
    }

    QSqlDatabase databaseConnection(SQLDbType type) const
    {
      return QSqlDatabase::database(connectionName(type));
    }
  
    QSqlDatabase noggitDatabase() const
    {
      return SqlDatabaseManager::databaseConnection(SQLDbType::Noggit);
    }
  
    QSqlDatabase worldDatabase() const
    {
      return SqlDatabaseManager::databaseConnection(SQLDbType::World);
    }
  
  private:
    SqlDatabaseManager() = default;
    ~SqlDatabaseManager() = default;
    SqlDatabaseManager(const SqlDatabaseManager&) = delete;
    SqlDatabaseManager& operator=(const SqlDatabaseManager&) = delete;

    SqlDriver _sql_driver = SqlDriver::MySQL;

    mutable QMap<SQLDbType, DbConfig> configs; // stores current configuration per DB type
  
    QString connectionName(SQLDbType type) const
    {
      switch (type)
      {
      case SQLDbType::Noggit: return "noggit";
      case SQLDbType::World: return "world";
      }
      return {};
    }
  
    QString driverToString(SqlDriver driver)
    {
      switch (driver)
      {
        case SqlDriver::MySQL: return "QMYSQL";
        // case SqlDriver::SQLite: return "QSQLITE";
        // case SqlDriver::PostgreSQL: return "QPSQL";
      }
      return "";
    }
  };
}
