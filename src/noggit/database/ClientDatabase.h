#pragma once
#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <blizzard-database-library/include/structures/FileStructures.h>

#include <noggit/database/SqlDatabaseManager.h>

#include <optional>

#include <QString>
#include <QSqlRecord>


constexpr const char* dbc_string_loc_names[16] = { "enUS", "koKR", "frFR", "deDE", "zhCN",
                              "zhTW", "esES", "esMX", "ruRU", "jaJP", "ptPT", "itIT",
                              "unk_12", "unk_13", "unk_14", "unk_15" };

using namespace BlizzardDatabaseLib;
namespace Noggit
{
  struct DbColumnFormat
  {
    std::string Type = "";
    std::string Name = "";
    // int size;
    bool isID = false;
    bool isRelation = false;
    bool isSigned = true;
  };

  enum class DatabaseMode
  {
    Sql,
    ClientStorage
  };

  struct SqlException : public std::runtime_error
  {
    SqlException(const QString& msg)
      : std::runtime_error(msg.toStdString()) {}
  };

  class Noggit::ClientDatabaseTable;
  // calls client or server db adaptively. so /sql/ is not really a good location
  class ClientDatabase
  {
    friend class ClientDatabaseTable;
  public:

    static void setDatabaseMode(DatabaseMode mode);
    static DatabaseMode databaseMode(); // sql or client storage

    static ClientDatabaseTable getTable(const std::string& tableName);

    static void saveTable(const std::string& tableName);
  
    // static std::optional<Structures::BlizzardDatabaseRow> getRowById(const std::string& tableName, unsigned int id); // constructs a row either from db or client
  
    static void TODODeploySqlToClient();

    static QSqlQuery executeQuery(const QString& sql, bool forward_only = true); // forward onl means can't browse query backward, but massively speeds up forward iteration
  
  private:
    static DatabaseMode _database_mode;
  
    static Structures::BlizzardDatabaseRow clientRowById(const std::string& tableName, unsigned int id);
    static Structures::BlizzardDatabaseRow sqlRowById(const std::string& tableName, unsigned int id);
  

  };

  // TODO can subclass BlizzardDatabaseRecordCollection instead
  class DatabaseRecordCollection
  {
  public:
    DatabaseRecordCollection(const ClientDatabaseTable& table);
    bool HasRecords();
    Structures::BlizzardDatabaseRow Next();
    // Structures::BlizzardDatabaseRow First();
    // Structures::BlizzardDatabaseRow Last();

  private:
    const ClientDatabaseTable& _table;

    // client
    BlizzardDatabaseRecordCollection _client_iterator;

    // sql stuff
    QSqlQuery _query;
    bool _querryHasStarted = false;
    bool _querry_valid = false;
    bool _hasNext = false;
    // QSqlRecord _nextRecord;
    Structures::BlizzardDatabaseRow _nextRecord;
    void querryAdvance();
  };

  // interface table that gets data either from sql or raw dbc
  // TODO : can just make BlizzardDatabaseTable subclass this.
  class ClientDatabaseTable
  {
    friend class DatabaseRecordCollection;

  private:
    const std::string _tableName;
    const QString _qtTableName;
    const Structures::BlizzardDatabaseRowDefinition _row_definition;

  public:
    ClientDatabaseTable(std::string tableName);

    // table info
    const std::string Name() const { return _tableName; };
    unsigned int RecordCount() const;
    int ColumnCount() const;
    int getRecordSize() const;
    Structures::BlizzardDatabaseRowDefinition& GetRecordDefinition() const;

    // get rows data
    std::optional<Structures::BlizzardDatabaseRow> RecordById(unsigned int id) const;
    // std::optional<Structures::BlizzardDatabaseRow> RecordByPosition(unsigned int positionId) const;
    // CheckIfIdExists
    Noggit::DatabaseRecordCollection Records() const;// { return Noggit::DatabaseRecordCollection(*this); };

    // modify data
    // Record addRecord(size_t id, size_t id_field = 0);
    // Record addRecordCopy(size_t id, size_t id_from, size_t id_field = 0);
    // void removeRecord(size_t id, size_t id_field = 0);
    // int getEmptyRecordID(size_t id_field = 0);


  private:
    // for internal use only, get the client storage
    BlizzardDatabaseLib::BlizzardDatabaseTable&  getClientTable() const;

    Structures::BlizzardDatabaseRow clientRowById(unsigned int id) const;
    Structures::BlizzardDatabaseRow sqlRowById(unsigned int id) const;

    // sql helpers
    bool UploadDBCtoDB();
    const std::string getSqlTableName(unsigned int build_id = 0) const; // get automatically from project if default(0)
    std::vector<DbColumnFormat> recordFormat() const; // true record format for all columns, not array size/loc etc. eg returns all 17 columns for loc.
    bool createSQLTableIfNotExist();
    bool verifySqlTableIntegrity();
    // Structures::BlizzardDatabaseRow sqlRecordToDatabaseRow(const QSqlRecord& record) const;
    Structures::BlizzardDatabaseRow sqlRecordToDatabaseRow(QSqlQuery& query) const;
  };
}

