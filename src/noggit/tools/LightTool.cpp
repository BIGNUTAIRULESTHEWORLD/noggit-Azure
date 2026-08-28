// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LightTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/MapView.h>
#include <noggit/Input.hpp>
#include <noggit/ui/tools/LightEditor/LightEditor.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/World.h>
#include <noggit/DBC.h>
#include <noggit/rendering/WorldRender.hpp>

#include <math/ray.hpp>
#include <math/sphere.hpp>

#include <external/glm/gtc/type_ptr.hpp>
#include <external/glm/gtx/matrix_decompose.hpp>
#include <external/glm/gtx/transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Noggit
{
    LightTool::LightTool(MapView* mapView)
        : Tool{ mapView }
    {
        setupHotkeys();
    }

    LightTool::~LightTool()
    {
        delete _lightEditor;
    }

    char const* LightTool::name() const
    {
        return "Lightning Editor";
    }

    editing_mode LightTool::editingMode() const
    {
        return editing_mode::light;
    }

    Ui::FontNoggit::Icons LightTool::icon() const
    {
        return Ui::FontNoggit::TOOL_LIGHT;
    }

    unsigned int LightTool::actionModality() const
    {
      unsigned int modality = 0;
      if (_moveSky)
        modality |= ActionModalityControllers::eMMB;
      if (_keyX != 0.0f || _keyZ != 0.0f)
        modality |= ActionModalityControllers::eNUM | ActionModalityControllers::eTRANSLATE;
      if (_keyScale != 0.0f)
        modality |= ActionModalityControllers::eNUM | ActionModalityControllers::eSCALE;
      if (_gizmoWasUsing)
        modality |= ActionModalityControllers::eLMB;
      return modality;
    }

    void LightTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _lightEditor = new Noggit::Ui::Tools::LightEditor(mapView(), mapView());
        _lightEditor->onSkySelected = [this](int sky_id) { selectSky(sky_id, false); };
        _lightEditor->onBrowserLightSelected = [this](int light_id) { _browserLightId = light_id; };
        _lightEditor->onDeleteSelected = [this] { deleteSelectedSky(); };
        QObject::connect(NOGGIT_ACTION_MGR, &ActionManager::historyNavigated, _lightEditor, [this]
        {
          // Undoing a paste removes the selected Sky from storage. Clear the
          // stale tool/gizmo state immediately so another viewport handle can
          // be selected before the next tick.
          if (_selectedSkyId && !selectedSky())
          {
            _selectedSkyId = 0;
            _gizmoWasUsing = false;
            _moveSky = false;
            mapView()->getWorld()->renderer()->skies()->selectSkyById(0);
            _lightEditor->refreshLightList();
            _lightEditor->clearSkySelection();
            _knownSkyCount = mapView()->getWorld()->renderer()->skies()->skies.size();
            _skyCountInitialized = true;
            mapView()->invalidate();
          }
        });
        toolPanel->registerTool(this, _lightEditor);
    }

    void LightTool::onTick(float deltaTime, TickParameters const& params)
    {
        auto& skies = mapView()->getWorld()->renderer()->skies()->skies;
        if (!_skyCountInitialized)
        {
          _knownSkyCount = skies.size();
          _skyCountInitialized = true;
        }
        else if (_knownSkyCount != skies.size())
        {
          if (_selectedSkyId && !selectedSky())
            selectSky(0, false);
          _lightEditor->refreshLightList(_selectedSkyId);
          if (_selectedSkyId)
            _lightEditor->selectSkyById(_selectedSkyId);
          else
            _lightEditor->clearSkySelection();
          _knownSkyCount = skies.size();
        }

        if (mapView()->timeSpeed() > 0.0f)
          _lightEditor->UpdateToolTime();

        if (params.camera_moved_since_last_draw)
          _lightEditor->updateActiveLights();

        if (params.camera_moved_since_last_draw || mapView()->timeSpeed() > 0.0f)
          _lightEditor->updateLightningInfo();

        Sky* sky = selectedSky();
        if (!sky)
          return;

        _lightEditor->updateSelectedSkyTransform();

        float speed = 80.0f;
        if (params.mod_ctrl_down && params.mod_shift_down)
          speed *= 0.01f;
        else if (params.mod_ctrl_down)
          speed *= 0.1f;
        else if (params.mod_shift_down)
          speed *= 10.0f;

        if (_keyX != 0.0f || _keyZ != 0.0f)
        {
          NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_TRANSFORMED,
            ActionModalityControllers::eNUM | ActionModalityControllers::eTRANSLATE);
          transformSelected(glm::vec3(_keyX, 0.0f, _keyZ) * speed * deltaTime);
        }

        if (_keyScale != 0.0f)
        {
          NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_TRANSFORMED,
            ActionModalityControllers::eNUM | ActionModalityControllers::eSCALE);
          transformSelected({}, std::pow(2.0f, _keyScale * deltaTime));
        }

        if (_moveSky)
        {
          if (params.mod_alt_down)
          {
            NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_TRANSFORMED,
              ActionModalityControllers::eALT | ActionModalityControllers::eMMB);
            transformSelected({}, std::pow(2.0f, _mouseVertical * 4.0f));
          }
          else if (params.mod_shift_down)
          {
            NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_TRANSFORMED,
              ActionModalityControllers::eSHIFT | ActionModalityControllers::eMMB);
            transformSelected({ 0.0f, _mouseVertical * 80.0f, 0.0f });
          }
          else if (!params.mod_ctrl_down)
          {
            NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_TRANSFORMED,
              ActionModalityControllers::eMMB);
            glm::vec3 delta = (_mouseHorizontal * params.dirUp + _mouseVertical * params.dirRight) * 500.0f;
            transformSelected(delta);
          }
        }

        _mouseHorizontal = 0.0f;
        _mouseVertical = 0.0f;
    }

    void LightTool::onSelected()
    {
      _lightEditor->UpdateToolTime();
      _lightEditor->updateActiveLights();
      _lightEditor->updateLightningInfo();

      // force lightning update when swapping tool because of local lightning always rendering in light mode
      // See : updateLightingUniformBlock()
      mapView()->_world->renderer()->skies()->force_update();
      mapView()->enableGizmoBar();
    }

    void LightTool::onDeselected()
    {
      mapView()->_world->renderer()->skies()->force_update();
      mapView()->disableGizmoBar();
      // todo, hide light info popup, or make it work in all modes
      _lightEditor->_lightning_info_dialog->hide();
    }

    void LightTool::onMousePress(MousePressParameters const& params)
    {
      if (params.button == Qt::MouseButton::MiddleButton)
        _moveSky = true;
    }

    void LightTool::onMouseRelease(MouseReleaseParameters const& params)
    {
      if (params.button == Qt::MouseButton::MiddleButton)
      {
        _moveSky = false;
        persistSelected();
        return;
      }

      if (params.button == Qt::MouseButton::LeftButton)
      {
        // A stale ImGuizmo hover flag must not prevent recovery when undo has
        // removed the light that owned the gizmo.
        bool const valid_selection = selectedSky() != nullptr;
        if (!valid_selection || (!ImGuizmo::IsUsing() && !ImGuizmo::IsOver()))
          pickSky(params.mouse_position);
      }
    }

    void LightTool::onMouseMove(MouseMoveParameters const& params)
    {
      if (!_moveSky)
      {
        _mouseHorizontal = 0.0f;
        _mouseVertical = 0.0f;
        return;
      }

      _mouseHorizontal = -mapView()->aspect_ratio() * params.relative_movement.dx() / static_cast<float>(mapView()->width());
      _mouseVertical = -params.relative_movement.dy() / static_cast<float>(mapView()->height());
    }

    void LightTool::onFocusLost()
    {
      _keyX = 0.0f;
      _keyZ = 0.0f;
      _keyScale = 0.0f;
      _moveSky = false;
      _mouseHorizontal = 0.0f;
      _mouseVertical = 0.0f;
      persistSelected();
    }

    Sky* LightTool::selectedSky()
    {
      if (!_selectedSkyId)
        return nullptr;
      return mapView()->getWorld()->renderer()->skies()->findSkyById(_selectedSkyId);
    }

    void LightTool::selectSky(int sky_id, bool update_editor)
    {
      Sky* sky = sky_id ? mapView()->getWorld()->renderer()->skies()->findSkyById(sky_id) : nullptr;
      if (sky && (sky->global || sky->zone_light))
        sky = nullptr;

      _selectedSkyId = sky ? sky->Id : 0;
      if (_selectedSkyId)
        _browserLightId = _selectedSkyId;
      mapView()->getWorld()->renderer()->skies()->selectSkyById(_selectedSkyId);

      if (update_editor && _lightEditor)
      {
        if (_selectedSkyId)
          _lightEditor->selectSkyById(_selectedSkyId);
        else
          _lightEditor->clearSkySelection();
      }
      mapView()->invalidate();
    }

    void LightTool::pickSky(QPoint const& mouse_position)
    {
      // Use the coordinates from this release event. MapView::intersect_ray()
      // uses its last mouse-move position, which can be stale after focus was
      // transferred from the Light Selection list back to the viewport.
      float const ndc_x = 2.0f * mouse_position.x() / mapView()->width() - 1.0f;
      float const ndc_y = 1.0f - 2.0f * mouse_position.y() / mapView()->height();
      glm::mat4 const inverse_view_projection = glm::inverse(mapView()->projection() * mapView()->model_view());
      glm::vec4 const world_position = inverse_view_projection * glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f);
      glm::vec3 const ray_target = glm::vec3(world_position) / world_position.w;
      glm::vec3 const camera = mapView()->getCamera()->position;
      math::ray const ray{camera, ray_target - camera};
      int closest_id = 0;
      float closest_t = std::numeric_limits<float>::max();

      for (Sky const& sky : mapView()->getWorld()->renderer()->skies()->skies)
      {
        if (sky.global || sky.zone_light)
          continue;

        float distance = glm::distance(sky.pos, camera);
        float handle_radius = glm::clamp(distance * 0.012f, 5.0f, 30.0f);
        auto hit = ray.intersects_sphere({ sky.pos, handle_radius });
        if (hit.hit && hit.t < closest_t)
        {
          closest_t = hit.t;
          closest_id = sky.Id;
        }
      }

      selectSky(closest_id);
    }

    void LightTool::copySelectedSky()
    {
      if (Sky* sky = selectedSky(); sky && (!_browserLightId || sky->Id == _browserLightId))
      {
        _skyClipboard = *sky;
        _lightEditor->updateClipboard(*_skyClipboard);
        _skyClipboard->pos = glm::vec3(0.0f);
        _skyClipboard->setSelected(false);
        return;
      }

      // A selected browser row from another continent may not have a live Sky in
      // the current renderer. Materialize its Light.dbc record solely as the
      // clipboard source, and retarget the eventual paste to this map.
      if (_browserLightId)
      {
        for (DBCFile::Iterator record = gLightDB.begin(); record != gLightDB.end(); ++record)
        {
          if (record->getInt(LightDB::ID) != _browserLightId)
            continue;

          _skyClipboard.emplace(record, mapView()->getWorld()->getRenderContext());
          _lightEditor->updateClipboard(*_skyClipboard);
          _skyClipboard->setMapId(mapView()->getWorld()->getMapID());
          _skyClipboard->pos = glm::vec3(0.0f);
          _skyClipboard->global = false;
          _skyClipboard->zone_light = false;
          _skyClipboard->setSelected(false);
          return;
        }
      }
    }

    void LightTool::pasteSky()
    {
      if (!_skyClipboard)
        return;

      glm::vec3 position = mapView()->cursorPosition() + _skyClipboard->pos;
      unsigned int new_id = gLightDB.getEmptyRecordID(LightDB::ID);
      auto* action = NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_ADDED);
      Sky* pasted = mapView()->getWorld()->renderer()->skies()->createNewSky(&*_skyClipboard, new_id, position);
      if (pasted)
      {
        action->registerSkyAdded(pasted);
        mapView()->setDbcDirty(&gLightDB);
        _lightEditor->refreshLightList(pasted->Id);
        // Keep repeated copy/paste work on the Light Selection tab. Editing
        // the pasted light remains available through double-click or the
        // explicit Edit button.
        selectSky(pasted->Id, false);
        _knownSkyCount = mapView()->getWorld()->renderer()->skies()->skies.size();
        _skyCountInitialized = true;

        // Rebuilding the list can leave keyboard focus on the QListWidget,
        // which consumes the next Ctrl+C/Ctrl+V instead of forwarding it to
        // MapView's tool hotkeys. Return focus to the viewport so the selected
        // pasted sphere can immediately be copied or pasted again.
        mapView()->setFocus(Qt::OtherFocusReason);
      }
      NOGGIT_ACTION_MGR->endAction();
    }

    void LightTool::deleteSelectedSky()
    {
      int const sky_id = _selectedSkyId;
      if (!sky_id)
        return;

      Sky* sky = selectedSky();
      if (!sky)
        return;

      auto* action = NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_REMOVED);
      action->registerSkyRemoved(sky);
      selectSky(0, false);
      if (mapView()->getWorld()->renderer()->skies()->deleteSkyById(sky_id))
      {
        mapView()->setDbcDirty(&gLightDB);
        _lightEditor->refreshLightList();
        _lightEditor->clearSkySelection();
        _knownSkyCount = mapView()->getWorld()->renderer()->skies()->skies.size();
        _skyCountInitialized = true;
        mapView()->invalidate();
      }
      NOGGIT_ACTION_MGR->endAction();
    }

    void LightTool::transformSelected(glm::vec3 const& translation, float radius_multiplier)
    {
      Sky* sky = selectedSky();
      if (!sky)
        return;

      if (NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerSkyTransformed(sky);

      sky->pos += translation;
      if (radius_multiplier != 1.0f)
      {
        sky->r1 = glm::clamp(sky->r1 * radius_multiplier, 0.1f, 100000.0f);
        sky->r2 = glm::clamp(sky->r2 * radius_multiplier, sky->r1, 100000.0f);
      }

      mapView()->getWorld()->renderer()->skies()->force_update();
      if (_lightEditor)
        _lightEditor->updateSelectedSkyTransform();
      mapView()->invalidate();
    }

    void LightTool::persistSelected()
    {
      if (Sky* sky = selectedSky())
      {
        sky->save_light_record();
        mapView()->setDbcDirty(&gLightDB);
      }
    }

    void LightTool::renderImGui(ImGuizmo::MODE mode, ImGuizmo::OPERATION operation)
    {
      Sky* sky = selectedSky();
      if (!sky)
      {
        _gizmoWasUsing = false;
        return;
      }
      if (operation == ImGuizmo::ROTATE)
        return;

      auto model_view = mapView()->model_view();
      auto projection = mapView()->projection();
      glm::mat4 object_matrix = glm::translate(glm::mat4(1.0f), sky->pos);
      if (operation == ImGuizmo::SCALE)
        object_matrix = glm::scale(object_matrix, glm::vec3(sky->r2));

      glm::mat4 delta_matrix(1.0f);
      ImGuizmo::SetDrawlist();
      ImGuizmo::SetOrthographic(false);
      ImGuizmo::SetScaleGizmoAxisLock(true);
      ImGuizmo::BeginFrame();
      ImGuiIO& io = ImGui::GetIO();
      ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

      bool manipulated = ImGuizmo::Manipulate(glm::value_ptr(model_view), glm::value_ptr(projection), operation, mode,
        glm::value_ptr(object_matrix), glm::value_ptr(delta_matrix), nullptr);
      bool using_gizmo = ImGuizmo::IsUsing();

      if (manipulated && using_gizmo)
      {
        NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eSKY_TRANSFORMED, ActionModalityControllers::eLMB);
        if (NOGGIT_CUR_ACTION)
          NOGGIT_CUR_ACTION->registerSkyTransformed(sky);

        glm::vec3 scale;
        glm::quat orientation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(operation == ImGuizmo::TRANSLATE ? delta_matrix : object_matrix,
          scale, orientation, translation, skew, perspective);

        if (operation == ImGuizmo::TRANSLATE)
          sky->pos += translation;
        else if (operation == ImGuizmo::SCALE)
        {
          float new_outer = glm::clamp(std::max({ scale.x, scale.y, scale.z }), 0.1f, 100000.0f);
          float ratio = new_outer / std::max(sky->r2, 0.1f);
          sky->r1 = glm::clamp(sky->r1 * ratio, 0.1f, new_outer);
          sky->r2 = new_outer;
        }

        mapView()->getWorld()->renderer()->skies()->force_update();
        _lightEditor->updateSelectedSkyTransform();
        mapView()->invalidate();
      }

      if (_gizmoWasUsing && !using_gizmo)
        persistSelected();
      _gizmoWasUsing = using_gizmo;
    }

    void LightTool::setupHotkeys()
    {
      auto* view = mapView();
      auto in_light_tool = [view] { return view->get_editing_mode() == editing_mode::light; };

      addHotkey("copySelection"_hash, { .onPress = [this] { copySelectedSky(); }, .condition = in_light_tool });
      addHotkey("paste"_hash, { .onPress = [this] { pasteSky(); }, .condition = in_light_tool });
      addHotkey("duplacteSelection"_hash, { .onPress = [this] { copySelectedSky(); pasteSky(); }, .condition = in_light_tool });
      addHotkey("deleteSelection"_hash, { .onPress = [this] { deleteSelectedSky(); }, .condition = in_light_tool });
      addHotkey("moveSelectedDown"_hash, { .onPress = [this] { _keyX = 1.0f; }, .onRelease = [this] { _keyX = 0.0f; persistSelected(); }, .condition = in_light_tool });
      addHotkey("moveSelectedUp"_hash, { .onPress = [this] { _keyX = -1.0f; }, .onRelease = [this] { _keyX = 0.0f; persistSelected(); }, .condition = in_light_tool });
      addHotkey("moveSelectedLeft"_hash, { .onPress = [this] { _keyZ = 1.0f; }, .onRelease = [this] { _keyZ = 0.0f; persistSelected(); }, .condition = in_light_tool });
      addHotkey("moveSelectedRight"_hash, { .onPress = [this] { _keyZ = -1.0f; }, .onRelease = [this] { _keyZ = 0.0f; persistSelected(); }, .condition = in_light_tool });
      addHotkey("increaseSelectedScale"_hash, { .onPress = [this] { _keyScale = 1.0f; }, .onRelease = [this] { _keyScale = 0.0f; persistSelected(); }, .condition = in_light_tool });
      addHotkey("decreaseSelectedScale"_hash, { .onPress = [this] { _keyScale = -1.0f; }, .onRelease = [this] { _keyScale = 0.0f; persistSelected(); }, .condition = in_light_tool });
    }
}
