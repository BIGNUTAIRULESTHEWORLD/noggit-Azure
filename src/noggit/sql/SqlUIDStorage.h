#pragma once
#include <noggit/sql/DatabaseManager.h>

namespace Noggit::Sql
{
  class SqlUIDStorage
  {
  public:

    static bool hasMaxUIDStoredDB(std::size_t mapID);
    static std::uint32_t getGUIDFromDB(std::size_t mapID);
    static void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID);
    static void updateUIDinDB(std::size_t mapID, std::uint32_t NewUID);
  };
}

