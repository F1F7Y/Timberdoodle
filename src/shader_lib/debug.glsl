#pragma once

#include "shader_shared/debug.inl"

void debug_draw_circle(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, ShaderDebugCircleDraw draw)
{
    const uint capacity = deref(debug_info).circle_draw_capacity;
    const uint index = atomicAdd(deref(debug_info).circle_draw_indirect_info.instance_count, 1);
    if (index < capacity)
    {
        deref(deref(debug_info).circle_draws + index) = draw;
    }
    else
    {
        atomicAdd(deref(debug_info).gpu_output.exceeded_circle_draw_capacity, 1);
    }
}

void debug_draw_rectangle(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, ShaderDebugRectangleDraw draw)
{
    const uint capacity = deref(debug_info).rectangle_draw_capacity;
    const uint index = atomicAdd(deref(debug_info).rectangle_draw_indirect_info.instance_count, 1);
    if (index < capacity)
    {
        deref(deref(debug_info).rectangle_draws + index) = draw;
    }
    else
    {
        atomicAdd(deref(debug_info).gpu_output.exceeded_rectangle_draw_capacity, 1);
    }
}

void debug_draw_aabb(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, ShaderDebugAABBDraw draw)
{
    const uint capacity = deref(debug_info).aabb_draw_capacity;
    const uint index = atomicAdd(deref(debug_info).aabb_draw_indirect_info.instance_count, 1);
    if (index < capacity)
    {
        deref(deref(debug_info).aabb_draws + index) = draw;
    }
    else
    {
        atomicAdd(deref(debug_info).gpu_output.exceeded_aabb_draw_capacity, 1);
    }
}

bool debug_detector(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, uvec2 xy)
{
    return xy == deref(debug_info).cpu_input.texel_detector_pos;
}

void debug_detector_write_i32(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, uvec2 xy, int value, int channel)
{
    if (xy == deref(debug_info).cpu_input.texel_detector_pos && channel >= 0 && channel <= 4)
    {
        deref(debug_info).gpu_output.debug_ivec4[channel] = value;
    }
}

void debug_detector_write_i32vec4(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, uvec2 xy, ivec4 value)
{
    if (xy == deref(debug_info).cpu_input.texel_detector_pos)
    {
        deref(debug_info).gpu_output.debug_ivec4 = value;
    }
}

void debug_detector_write_f32(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, uvec2 xy, float value, int channel)
{
    if (xy == deref(debug_info).cpu_input.texel_detector_pos && channel >= 0 && channel <= 4)
    {
        deref(debug_info).gpu_output.debug_fvec4[channel] = value;
    }
}

void debug_detector_write_f32vec4(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, uvec2 xy, vec4 value)
{
    if (xy == deref(debug_info).cpu_input.texel_detector_pos)
    {
        deref(debug_info).gpu_output.debug_fvec4 = value;
    }
}

bool debug_in_detector_window(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, uvec2 xy, out uvec2 window_index)
{
    ShaderDebugInput cpu_in = deref(debug_info).cpu_input;
    uvec2 window_top_left_corner = cpu_in.texel_detector_pos - cpu_in.texel_detector_window_half_size;
    uvec2 window_bottom_right_corner = cpu_in.texel_detector_pos + cpu_in.texel_detector_window_half_size;
    window_index = xy - window_top_left_corner;
    return (all(greaterThanEqual(xy, window_top_left_corner)) && all(lessThanEqual(xy, window_bottom_right_corner)));
}

void debug_write_detector_image(daxa_RWBufferPtr(ShaderDebugBufferHead) debug_info, daxa_ImageViewId detector_image, uvec2 xy, vec4 value)
{
    ShaderDebugInput cpu_in = deref(debug_info).cpu_input;
    uvec2 window_index;
    if (debug_in_detector_window(debug_info, xy, window_index))
    {
        imageStore(daxa_image2D(detector_image), ivec2(window_index), value);

        if (xy == cpu_in.texel_detector_pos)
        {
            deref(debug_info).gpu_output.texel_detector_center_value = value;
        }
    }
}