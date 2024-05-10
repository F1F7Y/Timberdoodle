#pragma once

#include <daxa/daxa.inl>
#include <daxa/utils/task_graph.inl>

#include "../../shader_shared/shared.inl"

#include "draw_visbuffer.inl"
#include "cull_meshes.inl"
#include "analyze_visbuffer.inl"
#include "gen_hiz.inl"
#include "prepopulate_meshlets.inl"

#include "../scene_renderer_context.hpp"

namespace raster_visbuf
{
    inline auto create_visbuffer(daxa::TaskGraph tg, RenderContext const & render_context) -> daxa::TaskImageView
    {
        return tg.create_transient_image({
            .format = daxa::Format::R32_UINT,
            .size = { 
                render_context.render_data.settings.render_target_size.x,
                render_context.render_data.settings.render_target_size.y,
                1,
            },
            .name = "visbuffer",
        });
    }

    inline auto create_atomic_visbuffer(daxa::TaskGraph tg, RenderContext const & render_context) -> daxa::TaskImageView
    {
        return tg.create_transient_image({
            .format = daxa::Format::R64_UINT,
            .size = { 
                render_context.render_data.settings.render_target_size.x,
                render_context.render_data.settings.render_target_size.y,
                1,
            },
            .name = "atomic visbuffer",
        });
    }
    inline auto create_depth(daxa::TaskGraph tg, RenderContext const & render_context) -> daxa::TaskImageView
    {
        daxa::Format format = render_context.render_data.settings.enable_atomic_visbuffer ? daxa::Format::R32_SFLOAT : daxa::Format::D32_SFLOAT;
        return tg.create_transient_image({
            .format = format,
            .size = { 
                render_context.render_data.settings.render_target_size.x,
                render_context.render_data.settings.render_target_size.y,
                1,
            },
            .name = "depth",
        });
    }
}