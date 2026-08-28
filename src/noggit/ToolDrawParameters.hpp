// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include "tool_enums.hpp"
#include "MinimapRenderSettings.hpp"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include <vector>

struct MinimapRenderSettings;

namespace Noggit
{
    struct ToolDrawParameters
    {
        float radius = 0.0f;
        float inner_radius = 0.0f;
        float angle = 0.0f;
        float orientation = 0.0f;
        glm::vec3 ref_pos;
        glm::vec3 cursor_position_override;
        bool use_cursor_position_override = false;
        bool angled_mode = false;
        bool use_ref_pos = false;
        bool show_unpaintable_chunks = false;
        bool show_stamp_protection = false;
        glm::vec3 stamp_protection_center{};
        float stamp_protection_radius = 1.f;
        CursorType cursor_type = CursorType::CIRCLE;
        bool project_cursor_on_water = false;
        bool show_liquid_vertices = false;
        int liquid_attribute_overlay = 0;
        int liquid_edit_layer = -1;
        std::uint64_t liquid_surface_token = 0;
        int liquid_brush_falloff = 1;
        eTerrainType terrain_type = eTerrainType::eTerrainType_Flat;
        int displayed_water_layer = -1;
        glm::vec4 cursor_color = { 1.f, 1.f, 1.f, 1.f };
        bool show_painted_stamp_selection = false;
        std::vector<glm::vec3> road_preview_centerline;
        std::vector<glm::vec3> road_preview_left_edge;
        std::vector<glm::vec3> road_preview_right_edge;
        bool road_preview_blocked = false;
        std::vector<glm::vec3> road_reference_centerline;
        std::vector<glm::vec3> road_reference_left_edge;
        std::vector<glm::vec3> road_reference_right_edge;
        std::vector<std::vector<glm::vec3>> road_reference_mask_lines;
        std::vector<std::vector<glm::vec3>> stamp_height_preview_lines;
        MinimapRenderSettings minimapRenderSettings;
    };
}
