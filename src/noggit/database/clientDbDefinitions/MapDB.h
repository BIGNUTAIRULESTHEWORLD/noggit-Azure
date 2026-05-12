#pragma once
#include <noggit/database/ClientDatabase.h>

// experimental


struct MapDbRow : BlizzardDatabaseLib::Structures::BlizzardDatabaseRow
{
  uint AreaType() { return getUInt("InstanceType"); };        // uint
};

class MapDb : public Noggit::ClientDatabaseTable
{
public:
  MapDb() :
    ClientDatabaseTable("Map")
  {}
    uint MapID() ;        // uint

    std::optional<MapDbRow> mapRecordById(unsigned int id) { return static_cast<MapDbRow>(RecordById(id).value()); };

  /// Fields
  static const size_t MapID = 0;        // uint
  static const size_t InternalName = 1;    // string
  static const size_t AreaType = 2;      // uint
  static const size_t Flags = 3;      // uint
  static const size_t IsBattleground = 4;    // uint 
  static const size_t Name = 5;        // loc
  static const size_t AreaTableID = 22;    // uint 
  static const size_t MapDescriptionAlliance = 23;    // loc 
  static const size_t MapDescriptionHorde = 40;    // loc 
  static const size_t LoadingScreen = 57;    // uint [LoadingScreen]
  static const size_t minimapIconScale = 58;    // uint [LoadingScreen]
  static const size_t corpseMapID = 59; //	iRefID	Points to column 1, -1 if none
  static const size_t corpseX = 60; //	Float	The X - Coord of the instance entrance
  static const size_t corpseY = 61; //	Float	The Y - Coord of the instance entrance
  static const size_t TimeOfDayOverride = 62; //	Integer	Set to - 1 for everything but Orgrimmar and Dalaran arena.For those, the time of day will change to this.
  static const size_t ExpansionID = 63; //	Integer	Vanilla : 0, BC : 1, WotLK : 2
  static const size_t RaidOffset = 64; //	Integer
  static const size_t NumberOfPlayers = 65; //	Integer	Used for reset time?

  static std::string getMapName(int pMapID);
  static int findMapName(const std::string& map_name);
};