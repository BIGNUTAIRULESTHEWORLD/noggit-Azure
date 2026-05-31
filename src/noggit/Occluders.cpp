#include "Occluders.h"

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/Log.h>

#include <string>
#include <unordered_set>
#include <QFile>
#include <QTextStream>

namespace Noggit
{

  void OccluderStorage::loadFromCSV(int map_id)
  {
    if (loaded)
      return;

    loadOccludersFromCSV(map_id);
    loadOccluderNodesFromCSV(map_id);
    loadOccluderLocationsFromCSV(map_id);

    // optional, check for duplicates
    for (auto& occluder : occludersWotlk)
    {
      std::unordered_set<int> seqSet;
      bool hasDuplicates = false;
      for (const auto& node : occluder.nodes)
      {
        if (!seqSet.insert(node.Sequence).second)
          hasDuplicates = true;
      }
      assert(hasDuplicates == false);

      // Ensure sorting
      std::sort(occluder.nodes.begin(), occluder.nodes.end(),
        [](const OccluderNode& a, const OccluderNode& b)
        {
          return a.Sequence < b.Sequence;
        });

    }

    this->loaded = true;

  }

  void OccluderStorage::loadOccludersFromCSV(int map_id)
  {
    std::string occluder_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\Occluder.8.1.0.29139.csv";
    QString qPath = QString::fromStdString(occluder_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      // assert(headerLine == "ID, MapID, Type, SplineType, Red, Green, Blue, Alpha, Flags");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 9);

        bool ok;
        int occluder_map_id = fields[1].toInt(&ok);
        assert(ok);
        // only load this map
        if (map_id != occluder_map_id)
          continue;

        Occluder occluder_entry;
        occluder_entry.id = fields[0].toUInt(&ok);
        // occluder_entry.MapId = occluder_map_id;
        occluder_entry.Red = fields[4].toInt(&ok);
        occluder_entry.Green = fields[5].toInt(&ok);
        occluder_entry.Blue = fields[6].toInt(&ok);
        occluder_entry.Alpha = fields[7].toInt(&ok);

        occludersWotlk.push_back(occluder_entry);

      }
      file.close();
    }
    else
    {
      return;
    }
  }

  void OccluderStorage::loadOccluderNodesFromCSV(int map_id)
  {
    std::string occluder_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\OccluderNode.8.0.1.26970.csv";
    QString qPath = QString::fromStdString(occluder_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      // assert(headerLine == "ID, MapID, Type, SplineType, Red, Green, Blue, Alpha, Flags");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 4);

        bool ok;
        int occluder_id = fields[1].toInt(&ok);

        // find parent
        auto it = std::find_if(occludersWotlk.begin(), occludersWotlk.end(),
          [occluder_id](const Occluder& d) { return d.id == occluder_id; });
        if (it == occludersWotlk.end())
        { // not found
          // most likely simply not current map id and it wasn't loaded
          continue;
        }

        Occluder& parent = *it;

        assert(ok);
        // only load this map
        // if (map_id != parent.MapId)
        //   continue;

        OccluderNode occluder_node_entry;
        occluder_node_entry.Id = fields[0].toInt(&ok);
        occluder_node_entry.Sequence = fields[2].toInt(&ok);
        occluder_node_entry.LocationID = fields[3].toInt(&ok);

        parent.nodes.push_back(occluder_node_entry);

      }
      file.close();
    }
    else
    {
      return;
    }
  }

  void OccluderStorage::loadOccluderLocationsFromCSV(int map_id)
  {
    std::string occluder_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\OccluderLocation.8.0.1.27075.csv";
    QString qPath = QString::fromStdString(occluder_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      // assert(headerLine == "Pos_0, Pos_1, Pos_2, ID, MapID");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 5);

        bool ok;
        int occluder_map_id = fields[4].toInt(&ok);

        if (map_id != occluder_map_id)
          continue;

        // find parent
        // auto it = std::find_if(occludersWotlk.begin(), occludersWotlk.end(),
        //   [occluder_id](const Occluder& d) { return d.id == occluder_id; });
        // if (it == occludersWotlk.end())
        // { // not found
        //   assert(false);
        //   continue;
        // }
        assert(ok);

        // Remove surrounding quotes from each field
        for (QString& f : fields) {
          f = f.trimmed();  // remove spaces
          if (f.startsWith('"') && f.endsWith('"')) {
            f = f.mid(1, f.length() - 2); // remove quotes
          }
        }

        OccluderLocation occluder_node_entry;
        occluder_node_entry.initialized = true;
        occluder_node_entry.position.x = fields[0].toFloat(&ok);
        occluder_node_entry.position.y = fields[1].toFloat(&ok);
        occluder_node_entry.position.z = fields[2].toFloat(&ok);

        int loc_id = fields[3].toInt(&ok);

        OccluderNode* found_node = nullptr;
        auto it = std::find_if(
          occludersWotlk.begin(), occludersWotlk.end(),
          [loc_id, &found_node](Occluder& occluder)
          {
            for (auto& n : occluder.nodes)
              if (n.LocationID == loc_id)
                return (found_node = &n), true;

            return false;
          });

        if (it != occludersWotlk.end() && found_node)
        {
          found_node->location = occluder_node_entry;
        }
        else // not found
          assert(false);

      }
      file.close();
    }
    else
    {
      return;
    }
  }

}