#pragma once
#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <blizzard-database-library/include/structures/FileStructures.h>

#include <noggit/database/SqlDatabaseManager.h>

#include <optional>


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

  // interface table that gets data either from sql or raw dbc
  class ClientDatabaseTable
  {
  private:
    const std::string _tableName;

  public:
    unsigned int RecordCount() const;

    // column count from file header, not definition file
    int ColumnCount() const
    {
      return static_cast<int>(_tableReader->FieldCount());
    }

    std::string Name() const
    {
      return _tableName;
    }

    Structures::BlizzardDatabaseRow RecordById(unsigned int id) const
    {
      return _tableReader->RecordById(id);
    }

    Structures::BlizzardDatabaseRow RecordByPosition(unsigned int positionId) const
    {
      return _tableReader->Record(positionId);
    }

    BlizzardDatabaseRecordCollection Records() const
    {
      return BlizzardDatabaseRecordCollection(_tableReader);
    }

    Structures::BlizzardDatabaseRowDefinition GetRecordDefinition() const
    {
      return _tableReader->RecordDefinition();
    }


  };

  // calls client or server db adaptively. so /sql/ is not really a good location
  class ClientDatabase
  {
  public:
    // static ClientDatabase& instance()
    // {
    //   static ClientDatabase _instance;
    //   return _instance;
    // }
  
    static std::optional<Structures::BlizzardDatabaseRow> getRowById(const std::string& tableName, unsigned int id); // constructs a row either from db or client
  
    static bool testUploadDBCtoDB(const BlizzardDatabaseLib::BlizzardDatabaseTable& table);
  
    static void TODODeploySqlToClient();
  
  private:
    // ClientDatabase() = default;
    // ~ClientDatabase() = default;
    // ClientDatabase(const ClientDatabase&) = delete;
    // ClientDatabase& operator=(const ClientDatabase&) = delete;
  
  
    static Structures::BlizzardDatabaseRow clientRowById(const std::string& tableName, unsigned int id);
    static Structures::BlizzardDatabaseRow sqlRowById(const std::string& tableName, unsigned int id);
  
    static bool createSQLTableIfNotExist(const BlizzardDatabaseLib::BlizzardDatabaseTable& table);
  
    static std::string getSqlTableName(const std::string& db_name, unsigned int build_id = 0); // get automatically from project if default(0)
  
    static std::vector<DbColumnFormat> recordFormat(const std::string& table_name); // true record format for all columns, not array size/loc etc. eg returns all 17 columns for loc.
  
  };

}

