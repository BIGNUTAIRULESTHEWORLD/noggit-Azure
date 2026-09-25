// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>
#include <noggit/Model.h> // Model
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/Sky.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <opengl/shader.hpp>
#include <glm/glm.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <math/trig.hpp>
#include <math/bounding_box.hpp>

#include <algorithm>
#include <string>
#include <array>
#include <unordered_map>
#include <QFile>
#include <QTextStream>
#include <QStringList>

const float skymul = 36.0f;

namespace skyparams
{
  // Map to store SkyParams globally
  std::unordered_map<unsigned int, SkyParam> skyParamMap;
  
  // get or create SkyParam from the global map
  SkyParam* getOrCreateParam(unsigned int id, Noggit::NoggitRenderContext context) {
  
    if (!id)
      return nullptr;
  
    auto it = skyParamMap.find(id);
    if (it != skyParamMap.end()) {
      return &(it->second);  // Return existing SkyParam
    }
  
    // Try to create new SkyParam and insert into map
    SkyParam newParam = SkyParam(id, context);
  
    // Dbc loading failed
    assert(newParam.Id != 0);
    if (newParam.Id == 0)
      return nullptr;
  
    auto [newIt, inserted] = skyParamMap.emplace(id, std::move(newParam));
    return &(newIt->second);  // Return newly created SkyParam
  }
}


SkyColor::SkyColor(int t, int col)
{
  time = t;
  color.z = ((col & 0x0000ff)) / 255.0f;
  color.y = ((col & 0x00ff00) >> 8) / 255.0f;
  color.x = ((col & 0xff0000) >> 16) / 255.0f;
}

SkyFloatParam::SkyFloatParam(int t, float val)
: time(t)
, value(val)
{
}

SkyParam::SkyParam(int paramId, Noggit::NoggitRenderContext context)
: _context(context)
{
  Id = paramId;

  if (Id == 0)
  {
    // shouldn't happen in the new system, we don't load params with no valid id.
    assert(false);

    return; // don't initialise entry
  }

  try
  {
    DBCFile::Record light_param = gLightParamsDB.getByID(paramId);
    int skybox_id = light_param.getInt(LightParamsDB::skybox);

    _highlight_sky = light_param.getInt(LightParamsDB::highlightSky);
    _river_shallow_alpha = light_param.getFloat(LightParamsDB::water_shallow_alpha);
    _river_deep_alpha = light_param.getFloat(LightParamsDB::water_deep_alpha);
    _ocean_shallow_alpha = light_param.getFloat(LightParamsDB::ocean_shallow_alpha);
    _ocean_deep_alpha = light_param.getFloat(LightParamsDB::ocean_deep_alpha);
    _glow = light_param.getFloat(LightParamsDB::glow);

    if (skybox_id)
    {
      try
      {
        auto skyboxRec = gLightSkyboxDB.getByID(skybox_id);
        skybox.emplace(skyboxRec.getString(LightSkyboxDB::filename), _context);
        skyboxFlags = skyboxRec.getInt(LightSkyboxDB::flags);
      }
      catch (...)
      {
        LogError << "When trying to get the skybox id " << skybox_id << "for the param " << paramId << " in LightSkybox.dbc." << std::endl;
      }

    }
  }
  catch (...)
  {
    LogError << "When trying to initialize Params the entry " << paramId << " in LightParams.dbc." << std::endl;
    Id = 0;
  }

  // initialize colors (lightIntBand.dbc)
  int light_int_start = (paramId * NUM_SkyColorNames) - (NUM_SkyColorNames - 1);
  for (int i = 0; i < NUM_SkyColorNames; ++i)
  {
    try
    {
      DBCFile::Record rec = gLightIntBandDB.getByID(light_int_start + i);
      int entries = rec.getInt(LightIntBandDB::Entries);

      if (entries == 0)
      {
        // mmin[i] = -1;
      }
      else
      {
        // smallest/first time value
        // mmin[i] = rec.getInt(LightIntBandDB::Times);
        for (int l = 0; l < entries; l++)
        {
            SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
            colorRows[i].push_back(sc);
        }
      }
    }
    catch (...)
    {
      assert(false);
      LogError << "When trying to intialize sky, there was an error with getting an entry in LightIntBand.dbc id (" << i << "). Lightparam id : " << paramId << std::endl;
      /*
      DBCFile::Record rec = gLightIntBandDB.getByID(i);
      int entries = rec.getInt(LightIntBandDB::Entries);

      if (entries == 0)
      {
          mmin[i] = -1;
      }
      else
      {
          mmin[i] = rec.getInt(LightIntBandDB::Times);
          for (int l = 0; l < entries; l++)
          {
              SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
              colorRows[i].push_back(sc);
          }
      }*/
    }
  }

  // initialize float params
  int light_float_start = (paramId * NUM_SkyFloatParamsNames) - (NUM_SkyFloatParamsNames - 1);
  for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
    {
        try
        {
            DBCFile::Record rec = gLightFloatBandDB.getByID(light_float_start + i);
            int entries = rec.getInt(LightFloatBandDB::Entries);

            if (entries == 0)
            {
                // mmin_float[i] = -1;
            }
            else
            {
                // mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
                for (int l = 0; l < entries; l++)
                {
                    SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
                    floatParams[i].push_back(sc);
                }
            }
        }
        catch (...)
        {
            assert(false);
            LogError << "When trying to intialize sky, there was an error with getting an entry in LightFloatBand.dbc id (" << i << "). Lightparam id : " << paramId << std::endl;
            /*
            DBCFile::Record rec = gLightFloatBandDB.getByID(i + 1);
            int entries = rec.getInt(LightFloatBandDB::Entries);

            if (entries == 0)
            {
                mmin_float[i] = -1;
            }
            else
            {
                mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
                for (int l = 0; l < entries; l++)
                {
                    SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
                    floatParams[i].push_back(sc);
                }
            }*/
        }
    }

}

bool SkyParam::highlight_sky() const
{
  return _highlight_sky;
}

float SkyParam::river_shallow_alpha() const
{
  return _river_shallow_alpha;
}

float SkyParam::river_deep_alpha() const
{
  return _river_deep_alpha;
}

float SkyParam::ocean_shallow_alpha() const
{
  return _ocean_shallow_alpha;
}

float SkyParam::ocean_deep_alpha() const
{
  return _ocean_deep_alpha;
}

float SkyParam::glow() const
{
  return _glow;
}

void SkyParam::set_glow(float glow)
{
  _glow = glow;
}

void SkyParam::set_highlight_sky(bool state)
{
  _highlight_sky = state;
}

void SkyParam::set_river_shallow_alpha(float alpha)
{
  _river_shallow_alpha = alpha;
}

void SkyParam::set_river_deep_alpha(float alpha)
{
  _river_deep_alpha = alpha;
}

 void SkyParam::set_ocean_shallow_alpha(float alpha)
{
  _ocean_shallow_alpha = alpha;
}

void SkyParam::set_ocean_deep_alpha(float alpha)
{
  _ocean_deep_alpha = alpha;
}


Sky::Sky(DBCFile::Iterator data, Noggit::NoggitRenderContext context)
: _context(context)
, _selected(false)
{
  Id = data->getInt(LightDB::ID);
  mapId = data->getInt(LightDB::Map);
  pos = glm::vec3(data->getFloat(LightDB::PositionX) / skymul, data->getFloat(LightDB::PositionY) / skymul, data->getFloat(LightDB::PositionZ) / skymul);
  r1 = data->getFloat(LightDB::RadiusInner) / skymul;
  r2 = data->getFloat(LightDB::RadiusOuter) / skymul;

  global = (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f);

  for (int i = 0; i < NUM_SkyParamsNames; ++i)
  {
      int sky_param_id = data->getInt(LightDB::DataIDs + i);

      skyParams[i] = sky_param_id;

      // initialize param to map, shouldn't even be needed
      // if (sky_param_id > 0)
      // {
      //   getOrCreateParam(sky_param_id, _context);
      // }
  }
}

int Sky::getId() const
{
  return Id;
}

std::optional<SkyParam*> Sky::getParam(int param_index) const
{
  unsigned int param_id = skyParams[param_index];
  if (param_id == 0)
    return std::nullopt;
  
  if (cachedCurrentParam && cachedCurrentParam->Id == param_id) {
    return cachedCurrentParam;
  }
  
  SkyParam* param_ptr = skyparams::getOrCreateParam(param_id, _context);

  if (param_ptr)
  {
    cachedCurrentParam = param_ptr;
    return cachedCurrentParam;
  }
  else
  {
    cachedCurrentParam = nullptr;
    return std::nullopt;
  }
}

std::optional<SkyParam*> Sky::getCurrentParam() const
{
  auto current = getParam(curr_sky_param);
  if (current.has_value() || curr_sky_param == SKY_PARAM_CLEAR)
    return current;

  // Some lights omit one or more weather variants. The client retains the
  // light's clear profile in that case instead of dropping all lighting data.
  return getParam(SKY_PARAM_CLEAR);
}

float Sky::floatParamFor(int r, int t) const
{
  auto param_opt = getCurrentParam();
  if (!param_opt.has_value())
    return 0.0f;

  SkyParam* const sky_param = param_opt.value();

  if (sky_param->floatParams[r].empty())
  {
    return 0.0f;
  }
  float c1, c2;
  int t1, t2;
  size_t last = sky_param->floatParams[r].size() - 1;

  if (t < sky_param->floatParams[r].front().time)
  {
    // reverse interpolate
    c1 = sky_param->floatParams[r][last].value;
    c2 = sky_param->floatParams[r][0].value;
    t1 = sky_param->floatParams[r][last].time;
    t2 = sky_param->floatParams[r][0].time + DAY_DURATION;
    t += DAY_DURATION;
  }
  else
  {
    for (size_t i = last; true; i--)
    { //! \todo iterator this.
      if (sky_param->floatParams[r][i].time <= t)
      {
        c1 = sky_param->floatParams[r][i].value;
        t1 = sky_param->floatParams[r][i].time;

        if (i == last)
        {
          c2 = sky_param->floatParams[r][0].value;
          t2 = sky_param->floatParams[r][0].time + DAY_DURATION;
        }
        else
        {
          c2 = sky_param->floatParams[r][i + 1].value;
          t2 = sky_param->floatParams[r][i + 1].time;
        }
        break;
      }
    }
  }

  float tt = static_cast<float>(t - t1) / static_cast<float>(t2 - t1);
  return c1 + ((c2 - c1) * tt);
}

glm::vec3 Sky::colorFor(int r, int t) const
{
  auto param_opt = getCurrentParam();
  if (!param_opt.has_value())
    return glm::vec3(0.0f, 0.0f, 0.0f);

  SkyParam* const sky_param = param_opt.value();

  if (sky_param->colorRows[r].empty())
  {
    return glm::vec3(0.0f, 0.0f, 0.0f);
  }
  glm::vec3 c1, c2;
  int t1, t2;
  int last = static_cast<int>(sky_param->colorRows[r].size()) - 1;

  if (last == 0)
  {
      c1 = sky_param->colorRows[r][last].color;
      c2 = sky_param->colorRows[r][0].color;
      t1 = sky_param->colorRows[r][last].time;
      t2 = sky_param->colorRows[r][0].time + DAY_DURATION;
      t += DAY_DURATION;
  }
  else
  {

      // if (t < sky_param->mmin[r])
      if (t < sky_param->colorRows[r].front().time)
      {
          // reverse interpolate
          c1 = sky_param->colorRows[r][last].color;
          c2 = sky_param->colorRows[r][0].color;
          t1 = sky_param->colorRows[r][last].time;
          t2 = sky_param->colorRows[r][0].time + DAY_DURATION;
          t += DAY_DURATION;
      }
      else
      {
          for (int i = last; true; i--)
          { //! \todo iterator this.
              if (sky_param->colorRows[r][i].time <= t)
              {
                  c1 = sky_param->colorRows[r][i].color;
                  t1 = sky_param->colorRows[r][i].time;

                  if (i == last)
                  {
                      c2 = sky_param->colorRows[r][0].color;
                      t2 = sky_param->colorRows[r][0].time + DAY_DURATION;
                  }
                  else
                  {
                      c2 = sky_param->colorRows[r][i + 1].color;
                      t2 = sky_param->colorRows[r][i + 1].time;
                  }
                  break;
              }
          }
      }
  }

  float tt = static_cast<float>(t - t1) / static_cast<float>(t2 - t1);
  return c1*(1.0f - tt) + c2*tt;
}

const float rad = 400.0f;

//...............................top....med....medh........horiz..........bottom
const math::degrees angles[] = { math::degrees (90.0f)
                               , math::degrees (18.0f)
                               , math::degrees (10.0f)
                               , math::degrees (3.0f)
                               , math::degrees (0.0f)
                               , math::degrees (-30.0f)
                               , math::degrees (-90.0f)
                               };
const int cnum = 7;
const int skycolors[cnum] = { SKY_COLOR_TOP, SKY_COLOR_MIDDLE, SKY_COLOR_BAND1, SKY_COLOR_BAND2, SKY_COLOR_SMOG, SKY_FOG_COLOR, SKY_FOG_COLOR };
const int hseg = 32;


void Skies::loadZoneLights(int map_id)
{
    // read zone lights from csv file
  {
    std::string zonelight_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\ZoneLight.3.4.3.56262.csv";
    QString qPath = QString::fromStdString(zonelight_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      assert(headerLine == "ID,Name,MapID,LightID");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 4);
        if (fields.size() != 4)
        {
          continue;
        }

        bool ok;
        int light_map_id = fields[2].toInt(&ok);
        assert(ok);
        // only load this map
        if (map_id != light_map_id)
          continue;

        ZoneLight zone_light_entry;
        zone_light_entry.id = fields[0].toInt(&ok);
        zone_light_entry.name = fields[1].toStdString();
        // zone_light_entry.mapId = light_map_id;
        zone_light_entry.lightId = fields[3].toInt(&ok);

        // zoneLightsWotlk[zone_light_entry.id] = zone_light_entry;
        zoneLightsWotlk.push_back(zone_light_entry);

        // get the light reference
        Sky* light_ptr = findSkyById(zone_light_entry.lightId);
        // if light was not loaded, most likely missing or not in map.
        assert(light_ptr != nullptr);
        if (!light_ptr)
          continue;
        // zoneLightsWotlk[zone_light_entry.id].light = findSkyById(zone_light_entry.lightId);
        light_ptr->zone_light = true;
      }
      file.close();
    }
    else
    {
      LogError << "Failed loading Zone Lights. Can't open " << zonelight_db_path << std::endl;
      return;
    }
  }

  // load zone light points to temporary object
  std::unordered_map<int, std::vector<ZoneLightPoint>> zoneLightPoints;
  {
    std::string zonelightpoints_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\ZoneLightPoint.3.4.3.56262.csv";
    QString qPath = QString::fromStdString(zonelightpoints_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      assert(headerLine == "ID,Pos_0,Pos_1,PointOrder,ZoneLightID");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 5);
        if (fields.size() != 5)
        {
          continue;
        }

        bool ok;
        int zone_light_id = fields[4].toUInt(&ok);
        assert(ok);

        // Check if the vector contains the zone_light_id
        // if (!zoneLightsWotlk.contains(zone_light_id))
        auto it = std::find_if(zoneLightsWotlk.begin(), zoneLightsWotlk.end(),
          [zone_light_id](const ZoneLight& zoneLight) {
            return zoneLight.id == zone_light_id;
          });
        if (it == zoneLightsWotlk.end())
          continue;

        ZoneLightPoint zone_light_point_entry;
        zone_light_point_entry.id = fields[0].toUInt(&ok);
        zone_light_point_entry.zoneLightId = zone_light_id;

        // convert server to client coords
        // need to swap X and Y
        zone_light_point_entry.posX = -fields[2].toFloat(&ok) + ZEROPOINT;
        zone_light_point_entry.posY = -fields[1].toFloat(&ok) + ZEROPOINT;
        zone_light_point_entry.pointOrder = fields[3].toUInt(&ok);

        // Automatically create a vector if the key doesn't exist, and add the entry
        zoneLightPoints[zone_light_id].push_back(zone_light_point_entry);

        // bad idea, this blindly trusts the ordering
        // zoneLightsWotlk[zone_light_id].points.push_back(glm::vec2(zone_light_point_entry.posX, zone_light_point_entry.posY));

      }
      file.close();
    }
    else
    {
      LogError << "Failed loading Zone Light Points. Can't open " << zonelightpoints_db_path << std::endl;
      return;
    }
  }

  // re order points, because we don't trust the storage order
  for (auto& points_list : zoneLightPoints)
  {
    // if (!zoneLightsWotlk.contains(points_list.first))
    //   continue;

    // polygon must have at least 3 points
    assert(points_list.second.size() > 2);

    // Check for duplicate pointOrder values
    for (int i = 1; i < points_list.second.size(); ++i) {
      if (points_list.second[i].pointOrder == points_list.second[i - 1].pointOrder)
      {
        assert(false);
        continue;
      }
    }

    // Sort the vector based on pointOrder (ascending)
    std::sort(points_list.second.begin(), points_list.second.end(), [](const ZoneLightPoint& a, const ZoneLightPoint& b) {
      return a.pointOrder < b.pointOrder;
      });

  }


  for (auto& zone_light : zoneLightsWotlk)
  {
    auto& list = zoneLightPoints[zone_light.id];
    // insert reordered points
    for (auto& point : list)
    {
      zone_light.points.push_back(glm::vec2(point.posX, point.posY));
    }

    // calculate 2d extents
    math::aabb_2d const bounds (zone_light.points);

    zone_light._extents[0] = bounds.min;
    zone_light._extents[1] = bounds.max;
  }

}

Sky* Skies::findSkyById(int sky_id)
{
  for (auto& sky : skies)
  {
    if (sky.getId() == sky_id)
    {
      return &sky;
    }
  }
  return nullptr;
}

Skies::Skies(unsigned int mapid, Noggit::NoggitRenderContext context)
  : stars (ModelInstance("Environments\\Stars\\Stars.mdx", context))
  , _context(context)
  , _indices_count(0)
  , _last_pos(glm::vec3(0.0f, 0.0f, 0.0f))
{
  bool has_global = false;
  for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
  {
    if (mapid == i->getUInt(LightDB::Map))
    {
      Sky s(i, _context);
      skies.push_back(s);
      numSkies++;

      if (s.pos == glm::vec3(0, 0, 0))
        has_global = true;
    }
  }

  if (!has_global)
  {
    LogDebug << "No global light data found for the current map (id :" << mapid
        << ") using light id 1 as a fallback" << std::endl;
    for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
    {
      if (1 == i->getUInt(LightDB::ID))
      {
        Sky s(i, _context);
        s.global = true;
        skies.push_back(s);
        numSkies++;
        break;
      }
    }
    using_fallback_global = true;
  }

  // sort skies from smallest to largest; global last.
  // smaller skies will have precedence when calculating weights to achieve smooth transitions etc.
  std::sort(skies.begin(), skies.end());

  // load Zone Lights data, was hardcoded in 3.3.5 and moved to a dbc in Cata. Noggit stores them in a csv
  if (Noggit::Project::CurrentProject::get()->projectVersion == Noggit::Project::ProjectVersion::WOTLK)
  {
    loadZoneLights(mapid);
  }
}

Sky* Skies::createNewSky(Sky*  old_sky, unsigned int new_id, glm::vec3& pos)
{
  Sky new_sky_copy = *old_sky;
  new_sky_copy.Id = new_id;
  new_sky_copy.pos = pos;

  new_sky_copy.weight = 0.f;
  new_sky_copy.is_new_record = true;
  new_sky_copy.setSelected(false);

  // A pasted light reuses the source lighting profiles, just as a pasted M2
  // reuses its model asset. Keep its spatial Light.dbc row in memory so Ctrl+S
  // can persist it, but do not write the DBC from a placement operation.
  new_sky_copy.save_light_record();

  skies.push_back(new_sky_copy);

  numSkies++;

  // refresh rendering & weights
  std::sort(skies.begin(), skies.end());
  force_update();

  for (Sky& sky : skies)
  {
    if (sky.Id == new_id)
    {
        return &sky;
    }
  }
  return nullptr;
}

Sky* Skies::restoreSky(Sky const& sky)
{
  if (Sky* existing = findSkyById(sky.Id))
    return existing;

  skies.push_back(sky);
  skies.back().setSelected(false);
  skies.back().save_light_record();
  ++numSkies;
  std::sort(skies.begin(), skies.end());
  force_update();
  return findSkyById(sky.Id);
}

bool Skies::deleteSkyById(int sky_id)
{
  auto const found = std::find_if(skies.begin(), skies.end(), [sky_id](Sky const& sky)
  {
    return sky.Id == sky_id;
  });
  if (found == skies.end() || found->global || found->zone_light)
    return false;

  gLightDB.removeRecord(static_cast<std::size_t>(sky_id), LightDB::ID);
  skies.erase(found);
  if (numSkies > 0)
    --numSkies;
  force_update();
  return true;
}

void Skies::selectSkyById(int sky_id)
{
  for (Sky& sky : skies)
  {
    sky.setSelected(!sky.global && !sky.zone_light && sky.Id == sky_id);
  }
}

// returns the global light, not the highest weight
Sky* Skies::findSkyWeights(glm::vec3 pos)
{
  int default_sky_id = 0;

  for (auto& sky : skies)
  {
    if (sky.global)
    {
      default_sky_id = sky.Id;
      break;
    }
  }

  std::sort(skies.begin(), skies.end(), [=](Sky& a, Sky& b)
  {
    return glm::distance(pos, a.pos) > glm::distance(pos, b.pos);
  });

  for (auto& sky : skies)
  {
    float distance_to_light = glm::distance(pos, sky.pos);

    if (sky.global || distance_to_light > sky.r2)
    {
      sky.weight = 0.f;
      continue;
    }

    float length_of_falloff = sky.r2 - sky.r1;
    if (distance_to_light <= sky.r1 || length_of_falloff <= 0.0f)
    {
      sky.weight = 1.0f;
    }
    else
    {
      sky.weight = glm::clamp((sky.r2 - distance_to_light) / length_of_falloff,
                              0.0f, 1.0f);
    }

  }

  // Light zones
  glm::vec2 const pos_2d = glm::vec2(pos.x, pos.z);
  for (auto& lightzone : zoneLightsWotlk)
  {
    if (math::is_inside_of_aabb_2d(pos_2d, lightzone._extents[0], lightzone._extents[1]))
    {
      bool inside = math::is_inside_of_polygon(pos_2d, lightzone.points);

      if (inside)
      {
        Sky* sky = findSkyById(lightzone.lightId);
        if (sky)
          sky->weight = 1.0f;
      }
    }
  }

  return default_sky_id ? findSkyById(default_sky_id) : nullptr;
}

Sky* Skies::findClosestSkyByWeight()
{
    // gets the highest weight sky
    if (skies.size() == 0)
        return nullptr;

    Sky* closest_sky = &skies[0];
    for (auto& sky : skies)
    {
        // use >= to make sure when we have multiple with the same weight, 
        // last one has priority, because it is the closest
        // skies is sorted by distance to center
        if (sky.weight > 0.0f && sky.weight >= closest_sky->weight)
            closest_sky = &sky;
    }
    return closest_sky;
}

Sky* Skies::findClosestSkyByDistance(glm::vec3 pos)
{
    if (skies.size() == 0)
        return nullptr;

    Sky* closest = &skies[0];
    float distance = 1000000.f;
    for (auto& sky : skies)
    {
        float distanceToCenter = glm::distance(pos, sky.pos);

        if (distanceToCenter <= sky.r2 && distanceToCenter < distance)
        {
            distance = distanceToCenter;
            closest = &sky;
        }
    }

    return closest;
}

void Skies::setCurrentParam(int param_id)
{
  assert(param_id >= 0 && param_id < NUM_SkyParamsNames);

  active_param = static_cast<SkyParamsNames>(param_id);

  for (auto& sky : skies)
  {
      Sky* skyptr = &sky;
      skyptr->curr_sky_param = param_id;
  }

  force_update();
}

void Skies::update_sky_colors(glm::vec3 pos, int time, bool global_only)
{
  if (numSkies == 0 || (_last_time == time && _last_pos == pos && !_force_update))
  {
    return;
  }
  _force_update = false;

  Sky* default_sky = findSkyWeights(pos);

  // The global-lighting preference also governs model skyboxes. Previously it
  // skipped local color blending but left local skybox weights active.
  if (global_only)
  {
    for (Sky& sky : skies)
      if (!sky.global)
        sky.weight = 0.0f;
  }

  // initialize lightning with default(global) light
  if (default_sky && default_sky->getCurrentParam().has_value())
  {
    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      color_set[i] = default_sky->colorFor(i, time);
    }

    // float values
    float fog_distance = default_sky->floatParamFor(SKY_FOG_DISTANCE, time);
    _fog_distance = fog_distance == 0.0f ? 6500.0f : fog_distance;

    float fog_multiplier = default_sky->floatParamFor(SKY_FOG_MULTIPLIER, time);
    _fog_multiplier = fog_multiplier == 0.0f ? 0.1f : fog_multiplier;

    _celestial_glow = default_sky->floatParamFor(SKY_CELESTIAL_GLOW, time);
    _cloud_density = default_sky->floatParamFor(SKY_CLOUD_DENSITY, time);
    _unknown_float_param4 = default_sky->floatParamFor(SKY_UNK_FLOAT_PARAM_4, time);
    _unknown_float_param5 = default_sky->floatParamFor(SKY_UNK_FLOAT_PARAM_5, time);

    // param values
    auto param_opt = default_sky->getCurrentParam();
    if (param_opt.has_value())
    {
      SkyParam* const default_sky_param = param_opt.value();

      _river_shallow_alpha = default_sky_param->river_shallow_alpha();
      _river_deep_alpha = default_sky_param->river_deep_alpha();
      _ocean_shallow_alpha = default_sky_param->ocean_shallow_alpha();
      _ocean_deep_alpha = default_sky_param->ocean_deep_alpha();
      _glow = default_sky_param->glow();
    }
    else
    {
      // if no param data, use some default. TODO : check how client does it.
      _river_shallow_alpha = 0.5f;
      _river_deep_alpha = 1.0f;
      _ocean_shallow_alpha = 0.75f;
      _ocean_deep_alpha = 1.0f;
      _glow = 0.5f;
    }
  }
  else
  {
    LogError << "Failed to load default light. Something went seriously wrong. Potentially corrupt Light.dbc" << std::endl;

    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      color_set[i] = glm::vec3(1.0f, 1.0f, 1.0f);
    }

    _fog_multiplier = 0.1f;
    _fog_distance = 6500.0f;
    _celestial_glow = 1.0f;
    _cloud_density = 1.0f;
    _unknown_float_param4 = 1.0f;
    _unknown_float_param5 = 1.0f;

    _river_shallow_alpha = 0.5f;
    _river_deep_alpha = 1.0f;
    _ocean_shallow_alpha = 0.75f;
    _ocean_deep_alpha = 1.0f;
    _glow = 0.5f;

  }

  if (!global_only)
  {
    // Blending interpolation with local lights
    for (size_t j = 0; j<skies.size(); j++) 
    {
      Sky const& sky = skies[j];

      if (sky.weight > 0.f)
      {
        // now calculate the color rows
        for (int i = 0; i < NUM_SkyColorNames; ++i) 
        {
          if ((sky.colorFor(i, time).x>1.0f) || (sky.colorFor(i, time).y>1.0f) || (sky.colorFor(i, time).z>1.0f))
          {
            LogDebug << "Sky " << j << " " << i << " is out of bounds!" << std::endl;
            continue;
          }
          auto timed_color = sky.colorFor(i, time);
          color_set[i] = glm::mix(color_set[i], timed_color, sky.weight);
        }

        auto param_opt = sky.getCurrentParam();
        if (param_opt.has_value())
        {
          SkyParam* default_sky_param = param_opt.value();

          float sky_weight_remain = (1.0f - sky.weight);

          float fog_distance = sky.floatParamFor(SKY_FOG_DISTANCE, time);
          if (fog_distance != 0.0f)
            _fog_distance = (_fog_distance * sky_weight_remain) + (fog_distance * sky.weight);

          float fog_multiplier = sky.floatParamFor(SKY_FOG_MULTIPLIER, time);
          if (fog_multiplier != 0.0f)
            _fog_multiplier = (_fog_multiplier * sky_weight_remain) + (fog_multiplier * sky.weight);

          _celestial_glow = (_celestial_glow * sky_weight_remain) + (sky.floatParamFor(SKY_CELESTIAL_GLOW, time) * sky.weight);
          _cloud_density = (_cloud_density * sky_weight_remain) + (sky.floatParamFor(SKY_CLOUD_DENSITY, time) * sky.weight);
          _unknown_float_param4 = (_unknown_float_param4 * sky_weight_remain) + (sky.floatParamFor(SKY_UNK_FLOAT_PARAM_4, time) * sky.weight);
          _unknown_float_param5 = (_unknown_float_param5 * sky_weight_remain) + (sky.floatParamFor(SKY_UNK_FLOAT_PARAM_5, time) * sky.weight);

          _river_shallow_alpha = (_river_shallow_alpha * sky_weight_remain) + (default_sky_param->river_shallow_alpha() * sky.weight);
          _river_deep_alpha = (_river_deep_alpha * sky_weight_remain) + (default_sky_param->river_deep_alpha() * sky.weight);
          _ocean_shallow_alpha = (_ocean_shallow_alpha * sky_weight_remain) + (default_sky_param->ocean_shallow_alpha() * sky.weight);
          _ocean_deep_alpha = (_ocean_deep_alpha * sky_weight_remain) + (default_sky_param->ocean_deep_alpha() * sky.weight);

          _glow = (_glow * sky_weight_remain) + (default_sky_param->glow() * sky.weight);
        }
        else
        {
          // if no data for param index, it just uses default values from global light, no blending
        }

      }

    }
  }

  // LightFloatBand defines the fog start and end distances. Applying the old
  // distance-dependent exponent on top made short-range zone lights nearly
  // opaque well before their authored fog end (notably Arathi and Wetlands).
  _fog_rate = 1.0f;

  _last_pos = pos;
  _last_time = time;

  _need_color_buffer_update = true;  
}

bool Skies::draw(glm::mat4x4 const& model_view
                , glm::mat4x4 const& projection
                , glm::vec3 const& camera_pos
                , OpenGL::Scoped::use_program& m2_shader
                , math::frustum const& frustum
                , const float& cull_distance
                , int animtime
                , int time
                /*, bool draw_particles*/
                , bool draw_skybox
                , OutdoorLightStats const& light_stats
                )
{
  if (numSkies == 0)
  {
    return false;
  }

  if (!_uploaded)
  {
    upload();
  }

  if (_need_color_buffer_update)
  {
    update_color_buffer();
  }

  {
    OpenGL::Scoped::use_program shader {*_program.get()};

    if(_need_vao_update)
    {
      update_vao(shader);
    }

    {
      OpenGL::Scoped::vao_binder const _ (_vao);
       
      shader.uniform("model_view_projection", projection * model_view);
      shader.uniform("camera_pos", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));

      int wrapped_time = time % DAY_DURATION;
      if (wrapped_time < 0)
        wrapped_time += DAY_DURATION;
      float const day_fraction = static_cast<float>(wrapped_time) / DAY_DURATION;
      float const solar_angle = day_fraction * (glm::pi<float>() * 2.0f)
        - glm::pi<float>() * 0.5f;
      float const solar_azimuth = glm::radians(-45.0f);
      glm::vec3 const sun_direction = glm::normalize(glm::vec3(
        glm::cos(solar_angle) * glm::cos(solar_azimuth),
        glm::sin(solar_angle),
        glm::cos(solar_angle) * glm::sin(solar_azimuth)));

      shader.uniform("sun_direction", sun_direction);
      shader.uniform("sun_color", color_set[SUN_COLOR]);
      shader.uniform("sun_halo_color", color_set[SUN_CLOUD_COLOR]);
      shader.uniform("cloud_emissive_color", color_set[CLOUD_EMISSIVE_COLOR]);
      shader.uniform("cloud_layer1_color", color_set[CLOUD_LAYER1_AMBIENT_COLOR]);
      shader.uniform("cloud_layer2_color", color_set[CLOUD_LAYER2_AMBIENT_COLOR]);
      shader.uniform("celestial_glow", _celestial_glow);
      shader.uniform("cloud_density", _cloud_density);
      shader.uniform("night_intensity", light_stats.nightIntensity);
      shader.uniform("day_fraction", day_fraction);

      gl.drawElements(GL_TRIANGLES, _indices_count, GL_UNSIGNED_SHORT, nullptr);
    }
  }

  if (draw_skybox)
  {
    struct SkyboxLayer
    {
      SkyParam* param = nullptr;
      float contribution = 0.0f;
      float opacity = 0.0f;
    };

    // update_sky_colors applies local lights from lowest to highest priority.
    // Convert those sequential blends into final contributions so missing
    // skyboxes reveal the procedural sky and overlapping skyboxes sum to one.
    std::vector<float> contributions(skies.size(), 0.0f);
    float remaining = 1.0f;
    for (std::size_t reverse_index = skies.size(); reverse_index > 0; --reverse_index)
    {
      std::size_t const index = reverse_index - 1;
      if (skies[index].global)
        continue;

      float const weight = glm::clamp(skies[index].weight, 0.0f, 1.0f);
      contributions[index] = weight * remaining;
      remaining *= 1.0f - weight;
    }

    std::vector<SkyboxLayer> layers;
    float procedural_celestial_contribution = 0.0f;
    auto add_layer = [&](Sky& sky, float contribution)
    {
      if (contribution <= 0.0001f)
        return;

      auto const param_opt = sky.getCurrentParam();
      SkyParam* const param = param_opt.has_value() ? param_opt.value() : nullptr;
      bool const has_skybox = param && param->skybox.has_value();
      bool const combines_procedural = has_skybox
        && (param->skyboxFlags & LIGHT_SKYBOX_COMBINE);

      if (!has_skybox || combines_procedural)
        procedural_celestial_contribution += contribution;
      if (has_skybox)
        layers.push_back({param, contribution, 0.0f});
    };

    Sky* global_sky = nullptr;
    for (Sky& sky : skies)
      if (sky.global)
      {
        global_sky = &sky;
        break;
      }

    if (global_sky)
      add_layer(*global_sky, remaining);
    else
      procedural_celestial_contribution += remaining;

    for (std::size_t index = 0; index < skies.size(); ++index)
      if (!skies[index].global)
        add_layer(skies[index], contributions[index]);

    // Convert final contributions to source-alpha values for ordered
    // low-to-high-priority compositing over the procedural sky dome.
    float later_skybox_contribution = 0.0f;
    for (std::size_t reverse_index = layers.size(); reverse_index > 0; --reverse_index)
    {
      SkyboxLayer& layer = layers[reverse_index - 1];
      float const available = std::max(1.0f - later_skybox_contribution, 0.0001f);
      layer.opacity = glm::clamp(layer.contribution / available, 0.0f, 1.0f);
      later_skybox_contribution += layer.contribution;
    }

    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const no_depth_test;
    for (SkyboxLayer const& layer : layers)
    {
      auto& model = layer.param->skybox.value();
      model.pos = camera_pos;
      model.scale = 0.1f;
      model.recalcExtents();

      OpenGL::M2RenderState model_render_state;
      model_render_state.tex_arrays = {0, 0};
      model_render_state.tex_indices = {0, 0};
      model_render_state.tex_unit_lookups = {-1, -1};
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      m2_shader.uniform("blend_mode", 0);
      m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
      m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
      m2_shader.uniform("tex_unit_lookup_1", 0);
      m2_shader.uniform("tex_unit_lookup_2", 0);
      m2_shader.uniform("pixel_shader", 0);

      int skybox_time = animtime;
      if ((layer.param->skyboxFlags & LIGHT_SKYBOX_FULL_DAY)
          && model.model->finishedLoading())
      {
        unsigned int const animation_length = model.model->animationLength(0);
        int wrapped_time = time % DAY_DURATION;
        if (wrapped_time < 0)
          wrapped_time += DAY_DURATION;
        float const day_fraction = static_cast<float>(wrapped_time) / DAY_DURATION;
        skybox_time = static_cast<int>(day_fraction * animation_length);
      }

      model.model->renderer()->draw(model_view
                                   , model
                                   , m2_shader
                                   , model_render_state
                                   , frustum
                                   , 1000000
                                   , camera_pos
                                   , skybox_time
                                   , display_mode::in_3D
                                   , true
                                   , true
                                   , nullptr
                                   , true
                                   , layer.opacity
                                   , layer.opacity < 0.9999f
                                   , true);
    }

    // LIGHT_SKYBOX_COMBINE retains procedural celestial elements. Blend stars
    // by the same effective contributions so boundaries do not pop.
    float const star_opacity = light_stats.nightIntensity
      * glm::clamp(procedural_celestial_contribution, 0.0f, 1.0f);
    if (star_opacity > 0.0001f)
    {
      stars.pos = camera_pos;
      stars.scale = 0.1f;
      stars.recalcExtents();
    
      OpenGL::M2RenderState model_render_state;
      model_render_state.tex_arrays = {0, 0};
      model_render_state.tex_indices = {0, 0};
      model_render_state.tex_unit_lookups = {-1, -1};
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      gl.depthMask(GL_TRUE);
      m2_shader.uniform("blend_mode", 0);
      m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
      m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
      m2_shader.uniform("tex_unit_lookup_1", 0);
      m2_shader.uniform("tex_unit_lookup_2", 0);
      m2_shader.uniform("pixel_shader", 0);
    
      stars.model->renderer()->draw(model_view
                                   , stars
                                   , m2_shader
                                   , model_render_state
                                   , frustum
                                   , 1000000
                                   , camera_pos
                                   , animtime
                                   , display_mode::in_3D
                                   , true
                                   , true
                                   , nullptr
                                   , true
                                   , star_opacity
                                   , star_opacity < 0.9999f
                                   , true);
    }
  }


  return true;
}

void Skies::drawLightingSpheres (glm::mat4x4 const& model_view
  , glm::mat4x4 const& projection
  , glm::vec3 const& camera_pos
  , math::frustum const& frustum
  , const float& cull_distance
)
{
  for (Sky& sky : skies)
  {
    if (glm::distance(sky.pos, camera_pos) <= cull_distance) // TODO: frustum cull here
    {
        glm::vec4 diffuse = { color_set[LIGHT_GLOBAL_DIFFUSE], 1.f };
        glm::vec4 ambient = { color_set[LIGHT_GLOBAL_AMBIENT], 1.f };

        Log << sky.getId() << " <=> (x,y,z) : " << sky.pos.x << "," << sky.pos.y << "," << sky.pos.z << " -- r1 : " << sky.r1 << " -- r2 : " << sky.r2 << std::endl;

        _sphere_render.draw(model_view * projection, sky.pos, ambient, sky.r1, 32, 18, 1.f);
        _sphere_render.draw(model_view * projection, sky.pos, diffuse, sky.r2, 32, 18, 1.f);
    }
  }
}

void Skies::drawLightingSphereHandles (glm::mat4x4 const& model_view
  , glm::mat4x4 const& projection
  , glm::vec3 const& camera_pos
  , math::frustum const& frustum
  , const float& cull_distance
  , bool draw_spheres)
{
  for (Sky& sky : skies)
  {
    if (glm::distance(sky.pos, camera_pos) - sky.r2 <= cull_distance) // TODO: frustum cull here
    {

      _sphere_render.draw(model_view * projection, sky.pos, {1.f, 0.f, 0.f, 1.f}, 5.f);

      if (sky.selected())
      {
        glm::vec3 diffuse = color_set[LIGHT_GLOBAL_DIFFUSE];
        glm::vec3 ambient = color_set[LIGHT_GLOBAL_AMBIENT];
        _sphere_render.draw(model_view * projection, sky.pos, {ambient.x, ambient.y, ambient.z, 0.3}, sky.r1);
        _sphere_render.draw(model_view * projection, sky.pos, {diffuse.x, diffuse.y, diffuse.z, 0.3}, sky.r2);
      }
    }
  }
}

bool Skies::hasSkies() const
{
  return numSkies > 0;
}

float Skies::river_shallow_alpha() const
{
  return _river_shallow_alpha;
}

float Skies::river_deep_alpha() const
{
  return _river_deep_alpha;
}

float Skies::ocean_shallow_alpha() const
{
  return _ocean_shallow_alpha;
}

float Skies::ocean_deep_alpha() const
{
  return _ocean_deep_alpha;
}

float Skies::fog_distance_end() const
{
  return _fog_distance / 36.f;
}

float Skies::fog_distance_start() const
{
  return (_fog_distance / 36.f) * _fog_multiplier;
}

float Skies::fog_distance_multiplier() const
{
  return _fog_multiplier;
}

float Skies::celestial_glow() const
{
  return _celestial_glow;
}

float Skies::cloud_density() const
{
  return _cloud_density;
}

float Skies::unknown_float_param4() const
{
  return _unknown_float_param4;
}

float Skies::unknown_float_param5() const
{
  return _unknown_float_param5;
}

float Skies::glow() const
{
  return _glow;
}

float Skies::fogRate() const
{
  return _fog_rate;
}

void Skies::unload()
{
  _program.reset();
  _vertex_array.unload();
  _buffers.unload();
  _sphere_render.unload();

  _uploaded = false;
  _need_vao_update = true;

}

void Skies::force_update()
{
  _force_update = true;
}

void Skies::upload()
{
  _program.reset(new OpenGL::program(
    {
        {GL_VERTEX_SHADER, R"code(
#version 330 core

uniform mat4 model_view_projection;
uniform vec3 camera_pos;

in vec3 position;
in vec3 color;

out vec3 f_color;
out vec3 f_direction;

void main()
{
  vec4 pos = vec4(position + camera_pos, 1.f);
  gl_Position = model_view_projection * pos;
  f_color = color;
  f_direction = normalize(position);
}
)code" }
        , {GL_FRAGMENT_SHADER, R"code(
#version 330 core

in vec3 f_color;
in vec3 f_direction;

uniform vec3 sun_direction;
uniform vec3 sun_color;
uniform vec3 sun_halo_color;
uniform vec3 cloud_emissive_color;
uniform vec3 cloud_layer1_color;
uniform vec3 cloud_layer2_color;
uniform float celestial_glow;
uniform float cloud_density;
uniform float night_intensity;
uniform float day_fraction;

out vec4 out_color;

float hash31(vec3 p)
{
  p = fract(p * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yzx + 33.33);
  return fract((p.x + p.y) * p.z);
}

float value_noise(vec3 p)
{
  vec3 cell = floor(p);
  vec3 local = fract(p);
  local = local * local * (3.0 - 2.0 * local);

  float n000 = hash31(cell + vec3(0.0, 0.0, 0.0));
  float n100 = hash31(cell + vec3(1.0, 0.0, 0.0));
  float n010 = hash31(cell + vec3(0.0, 1.0, 0.0));
  float n110 = hash31(cell + vec3(1.0, 1.0, 0.0));
  float n001 = hash31(cell + vec3(0.0, 0.0, 1.0));
  float n101 = hash31(cell + vec3(1.0, 0.0, 1.0));
  float n011 = hash31(cell + vec3(0.0, 1.0, 1.0));
  float n111 = hash31(cell + vec3(1.0, 1.0, 1.0));

  float z0 = mix(mix(n000, n100, local.x),
                 mix(n010, n110, local.x), local.y);
  float z1 = mix(mix(n001, n101, local.x),
                 mix(n011, n111, local.x), local.y);
  return mix(z0, z1, local.z);
}

float cloud_noise(vec3 p)
{
  float result = 0.0;
  float amplitude = 0.55;
  for (int octave = 0; octave < 4; ++octave)
  {
    result += value_noise(p) * amplitude;
    p = p * 2.03 + vec3(19.1, 7.7, 13.4);
    amplitude *= 0.5;
  }
  return result;
}

void main()
{
  const float PI = 3.14159265358979323846;
  vec3 direction = normalize(f_direction);
  vec3 color = f_color;
  float glow = clamp(celestial_glow, 0.0, 4.0);
  float daylight = 1.0 - clamp(night_intensity, 0.0, 1.0);

  float upper_sky = smoothstep(-0.08, 0.22, direction.y);
  // Sample clouds from the normalized dome direction rather than spherical
  // longitude/latitude. Longitude has a wrap seam and is undefined at the
  // pole, which creates full-height triangular discontinuities on the dome.
  // A full rotation per day also keeps the animation continuous at midnight.
  float cloud_angle = day_fraction * 2.0 * PI;
  float cloud_cos = cos(cloud_angle);
  float cloud_sin = sin(cloud_angle);
  vec3 cloud_direction = vec3(
    direction.x * cloud_cos - direction.z * cloud_sin,
    direction.y,
    direction.x * cloud_sin + direction.z * cloud_cos);

  float density = clamp(cloud_density, 0.0, 1.0);
  float cloud_value = cloud_noise(cloud_direction * vec3(3.8, 2.4, 3.8));
  float cloud_threshold = mix(0.78, 0.34, density);
  float cloud_alpha = smoothstep(cloud_threshold, cloud_threshold + 0.18,
                                 cloud_value) * upper_sky;
  float cloud_edge = smoothstep(cloud_threshold, cloud_threshold + 0.055,
                                cloud_value) * (1.0 - smoothstep(
                                  cloud_threshold + 0.055,
                                  cloud_threshold + 0.18, cloud_value));
  vec3 cloud_tint = max(
    mix(cloud_layer2_color, cloud_layer1_color, cloud_value), vec3(0.0));
  float tint_luminance = dot(cloud_tint, vec3(0.2126, 0.7152, 0.0722));
  // Light*Band cloud colors are ambient tints, not final opaque cloud RGB.
  // Preserve their hue while deriving visible brightness from the local sky.
  vec3 cloud_chroma = tint_luminance > 0.01
    ? cloud_tint / tint_luminance : vec3(1.0);
  cloud_chroma = mix(vec3(1.0), cloud_chroma, 0.45);

  float sky_luminance = dot(max(color, vec3(0.0)),
                            vec3(0.2126, 0.7152, 0.0722));
  float shaded_luminance = max(sky_luminance * mix(0.62, 0.92, cloud_value),
                               mix(0.035, 0.10, daylight));
  vec3 cloud_body = cloud_chroma * shaded_luminance;
  cloud_body += sun_color * daylight * max(sun_direction.y, 0.0) * 0.16;
  cloud_body += cloud_emissive_color * cloud_edge * (0.20 + glow * 0.08);
  cloud_body = max(cloud_body, color * mix(0.20, 0.48, daylight));

  float visible_cloud_alpha = cloud_alpha * mix(0.48, 0.72, density);
  color = mix(color, cloud_body, visible_cloud_alpha);

  float sun_dot = dot(direction, normalize(sun_direction));
  float sun_disc = smoothstep(0.99935, 0.99978, sun_dot);
  float sun_halo = pow(max(sun_dot, 0.0), 180.0) * 0.8
                 + pow(max(sun_dot, 0.0), 28.0) * 0.18;
  color += sun_halo_color * sun_halo * glow * daylight * upper_sky;
  color = mix(color, sun_color * (1.0 + glow * 0.35),
              sun_disc * daylight * upper_sky);

  vec3 moon_direction = -normalize(sun_direction);
  float moon_dot = dot(direction, moon_direction);
  float moon_disc = smoothstep(0.99915, 0.99972, moon_dot);
  float moon_halo = pow(max(moon_dot, 0.0), 48.0) * 0.22;
  vec3 moon_color = mix(cloud_layer2_color, vec3(0.72, 0.80, 1.0), 0.7);
  color += moon_color * moon_halo * glow * night_intensity * upper_sky;
  color = mix(color, moon_color * (0.75 + glow * 0.2),
              moon_disc * night_intensity * upper_sky);

  out_color = vec4(max(color, vec3(0.0)), 1.0);
}
)code" }
    }
  ));

  _vertex_array.upload();
  _buffers.upload();

  std::vector<glm::vec3> vertices;
  std::vector<std::uint16_t> indices;

  glm::vec3 basepos1[cnum], basepos2[cnum];

  for (int h = 0; h < hseg; h++)
  {
    for (int i = 0; i < cnum; ++i)
    {
      basepos1[i] = basepos2[i] = glm::vec3(glm::cos(math::radians(angles[i])._) * rad, glm::sin(math::radians(angles[i])._)*rad, 0);

      math::rotate(0, 0, &basepos1[i].x, &basepos1[i].z, math::radians(glm::pi<float>() *2.0f / hseg * h));
      math::rotate(0, 0, &basepos2[i].x, &basepos2[i].z, math::radians(glm::pi<float>() *2.0f / hseg * (h + 1)));
    }

    for (int v = 0; v < cnum - 1; v++)
    {
      int start = static_cast<int>(vertices.size());

      vertices.push_back(basepos2[v]);
      vertices.push_back(basepos1[v]);
      vertices.push_back(basepos1[v + 1]);
      vertices.push_back(basepos2[v + 1]);

      indices.push_back(start+0);
      indices.push_back(start+1);
      indices.push_back(start+2);

      indices.push_back(start+2);
      indices.push_back(start+3);
      indices.push_back(start+0);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STATIC_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STATIC_DRAW);

  _indices_count = static_cast<int>(indices.size());

  _uploaded = true;
  _need_vao_update = true;
}

void Skies::update_vao(OpenGL::Scoped::use_program& shader)
{
  OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_vbo);

  {
    OpenGL::Scoped::vao_binder const _ (_vao);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> vertices_buffer (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> colors_buffer (_colors_vbo);
    shader.attrib("color", 3, GL_FLOAT, GL_FALSE, 0, 0);

    indices_binder.bind();
  }

  _need_vao_update = false;
}

void Skies::update_color_buffer()
{
  std::vector<glm::vec3> colors;

  for (int h = 0; h < hseg; h++)
  {
    for (int v = 0; v < cnum - 1; v++)
    {
      colors.push_back(color_set[skycolors[v]]);
      colors.push_back(color_set[skycolors[v]]);
      colors.push_back(color_set[skycolors[v + 1]]);
      colors.push_back(color_set[skycolors[v + 1]]);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_colors_vbo, colors, GL_STATIC_DRAW);

  _need_vao_update = true;
}


void OutdoorLightStats::interpolate(OutdoorLightStats *a, OutdoorLightStats *b, float r)
{
  static constexpr unsigned DayNight_SecondsPerDay = 86400;

  float progressDayAndNight = r / DayNight_SecondsPerDay;

  float phiValue = 0;
  const float thetaValue = 3.926991f;
  const float phiTable[4] =
    {
      2.2165682f,
      1.9198623f,
      2.2165682f,
      1.9198623f
    };

  unsigned currentPhiIndex = static_cast<unsigned>(progressDayAndNight / 0.25f);
  unsigned nextPhiIndex = 0;

  if (currentPhiIndex < 3)
    nextPhiIndex = currentPhiIndex + 1;

  // Lerp between the current value of phi and the next value of phi
  {
    float transitionProgress = (progressDayAndNight / 0.25f) - currentPhiIndex;

    float currentPhiValue = phiTable[currentPhiIndex];
    float nextPhiValue = phiTable[nextPhiIndex];

    phiValue = glm::mix(currentPhiValue, nextPhiValue, transitionProgress);
  }

  // Convert from Spherical Position to Cartesian coordinates
  float sinPhi = glm::sin(phiValue);
  float cosPhi = glm::cos(phiValue);

  float sinTheta = glm::sin(thetaValue);
  float cosTheta = glm::cos(thetaValue);

  dayDir.x = sinPhi * cosTheta;
  dayDir.y = sinPhi * sinTheta;
  dayDir.z = cosPhi;

  float ir = 1.0f - progressDayAndNight;
  nightIntensity = a->nightIntensity * ir + b->nightIntensity * progressDayAndNight;
}

OutdoorLighting::OutdoorLighting()
{

  static constexpr std::array<int, 24> night_hours =
    {1, 1, 1, 1, 1, 1,
     0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 1, 1};

  for (int i = 0; i < 24; ++i)
  {
    OutdoorLightStats ols;
    ols.nightIntensity = night_hours[i];
    lightStats.push_back(ols);
  }
}

OutdoorLightStats OutdoorLighting::getLightStats(int time)
{
  // ASSUME: only 24 light info records, one for each whole hour
  //! \todo  generalize this if the data file changes in the future

  int normalized_time ((static_cast<int>(time) % DAY_DURATION) / 2);

  static constexpr unsigned DayNight_SecondsPerDay = 86400;

  long progressDayAndNight = (static_cast<float>(normalized_time) * 120);

  while (progressDayAndNight < 0 || progressDayAndNight > DayNight_SecondsPerDay)
  {
    if (progressDayAndNight > DayNight_SecondsPerDay)
      progressDayAndNight -= DayNight_SecondsPerDay;

    if (progressDayAndNight < 0)
      progressDayAndNight += DayNight_SecondsPerDay;
  }

  OutdoorLightStats out;

  OutdoorLightStats *a, *b;
  int ta = normalized_time / 60;
  int tb = (ta + 1) % 24;

  a = &lightStats[ta];
  b = &lightStats[tb];

  out.interpolate(a, b, progressDayAndNight);

  return out;
}

bool Sky::operator<(const Sky& s) const
{
  if (global) return false;
  else if (s.global) return true;
  else return r2 < s.r2;
}

bool Sky::selected() const
{
  return _selected;
}

void Sky::setSelected(bool selected)
{
  _selected = selected;
}

void Sky::setMapId(int map_id)
{
  mapId = map_id;
}

void Sky::save_light_record()
{
  bool const create_record = !gLightDB.CheckIfIdExists(Id);
  DBCFile::Record lightDbRecord = create_record ? gLightDB.addRecord(Id) : gLightDB.getByID(Id);

  if (create_record)
    lightDbRecord.write(LightDB::Map, mapId);

  lightDbRecord.write(LightDB::PositionX, pos.x * skymul);
  lightDbRecord.write(LightDB::PositionY, pos.y * skymul);
  lightDbRecord.write(LightDB::PositionZ, pos.z * skymul);
  lightDbRecord.write(LightDB::RadiusInner, r1 * skymul);
  lightDbRecord.write(LightDB::RadiusOuter, r2 * skymul);

  for (int param_id = 0; param_id < NUM_SkyParamsNames; ++param_id)
    lightDbRecord.write(LightDB::DataIDs + param_id, skyParams[param_id]);

  is_new_record = false;
}

void Sky::save_to_dbc()
{
  // Save Light.dbc record
  // find new empty ID : gLightDB.getEmptyRecordID(); .prob do it when creating new light instead.
  try 
  {
    // assuming a new unused id is already set with is_new_record
    DBCFile::Record lightDbRecord = is_new_record ? gLightDB.addRecord(Id) : gLightDB.getByID(Id);

    if (is_new_record)
        lightDbRecord.write(LightDB::Map, mapId);

    lightDbRecord.write(LightDB::PositionX, pos.x * skymul);
    lightDbRecord.write(LightDB::PositionY, pos.y * skymul);
    lightDbRecord.write(LightDB::PositionZ, pos.z * skymul);
    lightDbRecord.write(LightDB::RadiusInner, r1 * skymul);
    lightDbRecord.write(LightDB::RadiusOuter,r2 * skymul);

    bool save_param_dbc = false;
    bool save_colors_dbc = false;
    bool save_floats_dbc = false;
    bool save_skybox_dbc = false;

    for (int param_id = 0; param_id < NUM_SkyParamsNames; param_id++)
    {
      auto param_opt = getParam(param_id);
      if (!param_opt.has_value())
        continue;

      SkyParam* sky_param = param_opt.value();
      if (sky_param == nullptr)
        continue;

      assert(sky_param->Id > 0);

      // This is weird. Id assignation for new records need to be done either now or before.
      // int lightParam_dbc_id = sky_param->_is_new_param_record ? gLightParamsDB.getEmptyRecordID() 
      //                                                         : sky_param->Id;
      // assuming Id was properly set
      int lightParam_dbc_id = sky_param->Id;

      lightDbRecord.write(LightDB::DataIDs + param_id, lightParam_dbc_id);

      if (lightParam_dbc_id == 0)
      {
        continue;
      }

      // save lightparams.dbc
      if (sky_param->_need_save || sky_param->_is_new_param_record)
      {
          save_param_dbc = true;
          try
                {
                    DBCFile::Record light_param = sky_param->_is_new_param_record ? gLightParamsDB.addRecord(lightParam_dbc_id)
                        : gLightParamsDB.getByID(lightParam_dbc_id);

                    light_param.write(LightParamsDB::highlightSky, int(sky_param->highlight_sky()));
                    light_param.write(LightParamsDB::water_shallow_alpha, sky_param->river_shallow_alpha());
                    light_param.write(LightParamsDB::water_deep_alpha, sky_param->river_deep_alpha());
                    light_param.write(LightParamsDB::ocean_shallow_alpha, sky_param->ocean_shallow_alpha());
                    light_param.write(LightParamsDB::ocean_deep_alpha, sky_param->ocean_deep_alpha());
                    light_param.write(LightParamsDB::glow, sky_param->glow());

                    if (sky_param->skybox.has_value()) // TODO skybox dbc
                    {
                        // try to find an existing record with those params
                        bool exists = false;
                        for (DBCFile::Iterator i = gLightSkyboxDB.begin(); i != gLightSkyboxDB.end(); ++i)
                        {
                            if (i->getString(LightSkyboxDB::filename) == sky_param->skybox.value().model->file_key().filepath()
                                && i->getInt(LightSkyboxDB::flags) == sky_param->skyboxFlags)
                            {
                                int id = i->getInt(LightSkyboxDB::ID);
                                light_param.write(LightParamsDB::skybox, id);
                                exists = true;
                                break;
                            }
                        }

                        if (!exists) // doesn't exist, create a new record
                        {
                          int new_skybox_dbc_id = gLightSkyboxDB.getEmptyRecordID();
                          DBCFile::Record rec = gLightSkyboxDB.addRecord(new_skybox_dbc_id);
                          rec.writeString(LightSkyboxDB::filename, sky_param->skybox.value().model->file_key().filepath());
                          rec.write(LightSkyboxDB::flags, sky_param->skyboxFlags);

                          gLightSkyboxDB.save();
                          
                          light_param.write(LightParamsDB::skybox, new_skybox_dbc_id);
                        }
                    }
                    else
                        light_param.write(LightParamsDB::skybox, 0);
                }
          catch (DBCFile::NotFound)
          {
              assert(false);
              LogError << "When trying to get the lightparams for the entry " << lightParam_dbc_id << " in LightParams.dbc" << std::endl;

              // failsafe, don't point to new id that couldn't be created
              if (sky_param->_is_new_param_record)
                  lightDbRecord.write(LightDB::DataIDs + param_id, 0);
          }
      }

      // save LightIntBand.dbc
      if (sky_param->_colors_need_save || sky_param->_is_new_param_record)
      {
        save_colors_dbc = true;
        int light_int_start = (lightParam_dbc_id * NUM_SkyColorNames) - (NUM_SkyColorNames - 1);

        for (int i = 0; i < NUM_SkyColorNames; ++i)
        {
          try
          {
            DBCFile::Record rec = sky_param->_is_new_param_record ? gLightIntBandDB.addRecord(light_int_start + i) 
                                                                  : gLightIntBandDB.getByID(light_int_start + i);

            int entries = static_cast<int>(sky_param->colorRows[i].size());

            rec.write(LightIntBandDB::Entries, entries); // nb of entries

            // write all entries to cleanup data
            for (int l = 0; l < 16; l++)
            {
              if (l >= entries)
              {
                rec.write(LightIntBandDB::Times + l, 0);
                rec.write(LightIntBandDB::Values + l, 0);
              }
              else
              {
                rec.write(LightIntBandDB::Times + l, sky_param->colorRows[i][l].time);
                
                int rebuilt_color_int = static_cast<int>(sky_param->colorRows[i][l].color.z * 255.0f)
                    + (static_cast<int>(sky_param->colorRows[i][l].color.y * 255.0f) << 8)
                    + (static_cast<int>(sky_param->colorRows[i][l].color.x * 255.0f) << 16);
                rec.write(LightIntBandDB::Values + l, rebuilt_color_int);
              }
            }
            // sky_param->_colors_need_save = false;
          }
          catch (...)
          {
              assert(false);
              LogError << "When trying to save sky colors, sky id : " << lightDbRecord.getInt(LightDB::ID) << std::endl;
          }
        }
      }

      // save LightFloatBand.dbc
      if (sky_param->_floats_need_save || sky_param->_is_new_param_record)
      {
        save_floats_dbc = true;
        int light_float_start = (lightParam_dbc_id * NUM_SkyFloatParamsNames) - (NUM_SkyFloatParamsNames - 1);

        for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
        {
          try
          {
            DBCFile::Record rec = sky_param->_is_new_param_record ? gLightFloatBandDB.addRecord(light_float_start + i) 
                                                                  : gLightFloatBandDB.getByID(light_float_start + i);
            int entries = static_cast<int>(sky_param->floatParams[i].size());

            rec.write(LightFloatBandDB::Entries, entries); // nb of entries

            // write all entries to cleanup data
            for (int l = 0; l < 16; l++)
            {
              if (l >= entries)
              {
                rec.write(LightFloatBandDB::Times + l, 0);
                rec.write(LightFloatBandDB::Values + l, 0.0f);
              }
              else
              {
                rec.write(LightFloatBandDB::Times + l, sky_param->floatParams[i][l].time);
                rec.write(LightFloatBandDB::Values + l, sky_param->floatParams[i][l].value);
              }
            }
            // sky_param->_floats_need_save = false;
          }
          catch (...)
          {
            LogError << "Error when trying to save sky float params, sky id : " << lightDbRecord.getInt(LightDB::ID) << std::endl;
          }
        }
      }

      // sky_param->_need_save = false;
      sky_param->_is_new_param_record = false;
    }

    gLightDB.save();
    if (save_colors_dbc)
        gLightIntBandDB.save();
    if (save_floats_dbc)
        gLightFloatBandDB.save();
    if (save_param_dbc)
        gLightParamsDB.save();

    is_new_record = false;
  }
  catch (DBCFile::AlreadyExists)
  {
    LogError << "DBCFile::AlreadyExists When trying to add light.dbc record for the entry " << Id << std::endl;
    assert(false);
  }
  catch (DBCFile::NotFound)
  {
    LogError << "DBCFile::NotFound When trying to add light.dbc record for the entry " << Id << std::endl;
    assert(false);
  }
  catch (...)
  {
    LogError << "Unknown exception When trying to add light.dbc record for the entry " << Id << std::endl;
    assert(false);
  }

}
