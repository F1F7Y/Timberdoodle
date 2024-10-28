#pragma once

#include <daxa/daxa.inl>
#include <daxa/utils/task_graph.inl>

#include "../../shader_shared/shared.inl"
#include "../../shader_shared/globals.inl"
#include "../../shader_shared/geometry.inl"

#define GEN_HIZ_X 16
#define GEN_HIZ_Y 16
#define GEN_HIZ_LEVELS_PER_DISPATCH 16
#define GEN_HIZ_WINDOW_X 64
#define GEN_HIZ_WINDOW_Y 64

DAXA_DECL_TASK_HEAD_BEGIN(GenHizTH)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE_CONCURRENT, daxa_BufferPtr(RenderGlobalData), globals)
DAXA_TH_IMAGE_ID(COMPUTE_SHADER_SAMPLED, REGULAR_2D, src)
DAXA_TH_IMAGE_ID_MIP_ARRAY(COMPUTE_SHADER_STORAGE_READ_WRITE, REGULAR_2D, mips, GEN_HIZ_LEVELS_PER_DISPATCH)
DAXA_DECL_TASK_HEAD_END

struct GenHizPush
{
    DAXA_TH_BLOB(GenHizTH, attach)
    daxa_RWBufferPtr(daxa_u32) counter;
    daxa_u32 mip_count;
    daxa_u32 total_workgroup_count;
};

#if DAXA_LANGUAGE != DAXA_LANGUAGE_GLSL

// The hiz generation only works for power of two images.
// The hiz mip chain is always power of two sized.
// the hiz mip chain is sized to the next smaller power of two relative to the src depth image.
// An example size: 1440x2560p src depth image has an hiz with a size of 1024x2048 at mip 0.

// The shader code will only ever see power of two hiz information, all calculations are done in power of two hiz space.
// In order to properly generate and sample the src image, gather operations are used.
// Rescaling the image implicitly in the calculation for mip0 of the hiz.

// The hiz gen works in a single dispatch in order to avoid costly pipeline barriers.
// In order to achieve this all workgroups incement an atomic counter when they are done.
// InterlockedAdd/atomicAdd's return the previous value before the add.
// The last workgroup to finish can use this counter to know it is the last one `(bool last_to_finish = (atomicAdd(counter, 1) == (total_workgroups -1))`

// Each workgroup downsamples a mip tile. A tile is a 64x64 section of the original image.
// NOTE: The src image will be treated as if it was sized to the next power of two in size. A 1440x2560p image will be treated as a 2048x4098 image!
// In total a 64x64 sample area is tapped in the first level. This may differ for non power of two depth sizes, these will be oversampled.

DAXA_DECL_TASK_HEAD_BEGIN(GenHizH2)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE_CONCURRENT, daxa_BufferPtr(RenderGlobalData), globals)
DAXA_TH_IMAGE_TYPED(COMPUTE_SHADER_STORAGE_READ_WRITE_CONCURRENT, daxa::RWTexture2DId<daxa_f32vec4>, debug_image)
DAXA_TH_IMAGE_TYPED(COMPUTE_SHADER_SAMPLED, daxa::Texture2DId<float>, src)
DAXA_TH_IMAGE_TYPED_MIP_ARRAY(COMPUTE_SHADER_STORAGE_READ_WRITE, daxa::RWTexture2DId<float>, hiz, GEN_HIZ_LEVELS_PER_DISPATCH)
DAXA_DECL_TASK_HEAD_END

struct GenHizData
{
    GenHizH2::AttachmentShaderBlob attach;
    daxa_RWBufferPtr(daxa_u32) workgroup_finish_counter;
    daxa_u32 total_workgroup_count;
    daxa_u32 mip_count;
    daxa_u32vec2 dst_mip0_size;
    daxa_u32vec2 src_size;
};

struct GenHizPush2
{
    daxa_BufferPtr(GenHizData) data;
};

#endif // #if DAXA_LANGUAGE != DAXA_LANGUAGE_GLSL

#if defined(__cplusplus)

#include "../scene_renderer_context.hpp"

inline auto gen_hiz_pipeline_compile_info2()
{
    return daxa::ComputePipelineCompileInfo2{ .source = daxa::ShaderFile{"./src/rendering/rasterize_visbuffer/gen_hiz.hlsl"}, .entry_point = "entry_gen_hiz", .push_constant_size = sizeof(GenHizPush2), .name = "GenHiz2" };
}

inline auto fill_cull_data(RenderContext const & render_context) -> CullData
{
    CullData ret = {};
    auto const rt_size = render_context.render_data.settings.render_target_size;

    ret.hiz_size = { (rt_size.x + 1) / 2, (rt_size.y + 1) / 2 };
    ret.hiz_size_rcp = { 1.0f / static_cast<f32>(ret.hiz_size.x), 1.0f / static_cast<f32>(ret.hiz_size.y) };

    ret.physical_hiz_size = { std::bit_ceil( ret.hiz_size.x ), std::bit_ceil( ret.hiz_size.y ) };
    ret.hiz_size_rcp = { 1.0f / static_cast<f32>(ret.physical_hiz_size.x), 1.0f / static_cast<f32>(ret.physical_hiz_size.y) };

    return ret;
}

struct TaskGenHizSinglePassInfo
{
    RenderContext * render_context = {};
    daxa::TaskGraph & tg;
    daxa::TaskImageView src = {};
    daxa::TaskBufferView globals = {};
    daxa::TaskImageView debug_image = {};
    daxa::TaskImageView * hiz = {};
};
void task_gen_hiz_single_pass(TaskGenHizSinglePassInfo const & info)
{
    daxa_u32vec2 const hiz_size = {
        std::max(1u, info.render_context->render_data.cull_data.physical_hiz_size.x),
        std::max(1u, info.render_context->render_data.cull_data.physical_hiz_size.y),
    };
    u32 const mip_count = 1 + static_cast<u32>(std::floor(std::log2(std::max(hiz_size.x, hiz_size.y))));
    *info.hiz = info.tg.create_transient_image({
        .format = daxa::Format::R32_SFLOAT,
        .size = {hiz_size.x, hiz_size.y, 1},
        .mip_level_count = mip_count,
        .name = "hiz",
    });
    info.tg.add_task(daxa::InlineTaskWithHead<GenHizH2::Task>{
        .views = std::array{
            GenHizH2::AT.globals | info.globals,
            GenHizH2::AT.debug_image | info.debug_image,
            GenHizH2::AT.src | info.src,
            GenHizH2::AT.hiz | *info.hiz,
        },
        .task = [=, render_context = info.render_context](daxa::TaskInterface ti)
        {
            auto const& AT = GenHizH2::AT;
            ti.recorder.set_pipeline(*render_context->gpu_context->compute_pipelines.at(gen_hiz_pipeline_compile_info2().name));
            auto const dispatch_x = round_up_div(hiz_size.x * 2, GEN_HIZ_WINDOW_X);
            auto const dispatch_y = round_up_div(hiz_size.y * 2, GEN_HIZ_WINDOW_Y);
            GenHizData data = {
                .attach = ti.attachment_shader_blob,
                .workgroup_finish_counter = ti.allocator->allocate_fill(0u).value().device_address,
                .total_workgroup_count = dispatch_x * dispatch_y,
                .mip_count = mip_count,
                .dst_mip0_size = hiz_size,
                .src_size = daxa_u32vec2{ ti.info(AT.src).value().size.x, ti.info(AT.src).value().size.y },
            };
            auto data_alloc = ti.allocator->allocate_fill(data, 8).value();
            ti.recorder.push_constant(data_alloc.device_address);
            
            render_context->render_times.start_gpu_timer(ti.recorder, RenderTimes::VISBUFFER_GEN_HIZ);
            ti.recorder.dispatch({ dispatch_x, dispatch_y, 1 });
            render_context->render_times.end_gpu_timer(ti.recorder, RenderTimes::VISBUFFER_GEN_HIZ);
        },
    });
}

#endif