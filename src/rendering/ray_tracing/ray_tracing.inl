#pragma once

#define DAXA_RAY_TRACING 1

#include <daxa/daxa.inl>
#include <daxa/utils/task_graph.inl>

#include "../../shader_shared/shared.inl"
#include "../../shader_shared/globals.inl"
#include "../../shader_shared/geometry.inl"
#include "../../shader_shared/visbuffer.inl"
#include "../../shader_shared/scene.inl"
#include "../../shader_shared/raytracing.inl"

#define RT_AO_X 8
#define RT_AO_Y 8

DAXA_DECL_TASK_HEAD_BEGIN(RayTraceAmbientOcclusionH)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE_CONCURRENT, daxa_RWBufferPtr(RenderGlobalData), globals)
DAXA_TH_IMAGE_ID(COMPUTE_SHADER_STORAGE_READ_WRITE_CONCURRENT, REGULAR_2D, debug_lens_image)
DAXA_TH_IMAGE_TYPED_ID(COMPUTE_SHADER_STORAGE_READ_WRITE_CONCURRENT, RWTexture2DId<daxa_f32vec4>, debug_image)
DAXA_TH_IMAGE_TYPED_ID(COMPUTE_SHADER_STORAGE_WRITE_ONLY, RWTexture2DId<daxa_f32>, ao_image)
DAXA_TH_IMAGE_TYPED_ID(COMPUTE_SHADER_STORAGE_READ_ONLY, Texture2DId<daxa_u32>, vis_image)
DAXA_TH_IMAGE_TYPED_ID(COMPUTE_SHADER_STORAGE_READ_ONLY, Texture2DId<daxa_f32>, depth_image)
DAXA_TH_IMAGE_ID(COMPUTE_SHADER_SAMPLED, REGULAR_2D, sky)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(GPUMaterial), material_manifest)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(MeshletInstancesBufferHead), instantiated_meshlets)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(GPUMesh), meshes)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(GPUMeshGroup), mesh_groups)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(daxa_f32mat4x3), combined_transforms)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(MergedSceneBlasIndirection), geo_inst_indirections)
DAXA_TH_TLAS_ID(COMPUTE_SHADER_READ, tlas)
DAXA_DECL_TASK_HEAD_END

struct RayTraceAmbientOcclusionPush
{
    RayTraceAmbientOcclusionH::AttachmentShaderBlob attach;
};

#if defined(__cplusplus)

#include "../../gpu_context.hpp"

inline auto ray_trace_ao_compute_pipeline_info() -> daxa::ComputePipelineCompileInfo
{
    return {
        .shader_info = daxa::ShaderCompileInfo{
            .source = daxa::ShaderFile{"./src/rendering/ray_tracing/ray_tracing.hlsl"},
            .compile_options = {
                .entry_point = "entry_rt_ao",
                .language = daxa::ShaderLanguage::SLANG,
            },
        },
        .push_constant_size = s_cast<u32>(sizeof(RayTraceAmbientOcclusionPush)),
        .name = std::string{RayTraceAmbientOcclusionH::NAME},
    };
}

inline auto ray_trace_ao_rt_pipeline_info() -> daxa::RayTracingPipelineCompileInfo
{
    return {
        .ray_gen_infos = {
            {
                .source = daxa::ShaderFile{"./src/rendering/ray_tracing/ray_tracing.hlsl"},
                .compile_options = {.entry_point = "ray_gen", .language = daxa::ShaderLanguage::SLANG},
            },
        },
        .any_hit_infos = {
            {
                .source = daxa::ShaderFile{"./src/rendering/ray_tracing/ray_tracing.hlsl"},
                .compile_options = {.entry_point = "any_hit", .language = daxa::ShaderLanguage::SLANG},
            }
        },
        .closest_hit_infos = {
            {
                .source = daxa::ShaderFile{"./src/rendering/ray_tracing/ray_tracing.hlsl"},
                .compile_options = {.entry_point = "closest_hit", .language = daxa::ShaderLanguage::SLANG},
            },
        },
        .miss_hit_infos = {
            {
                .source = daxa::ShaderFile{"./src/rendering/ray_tracing/ray_tracing.hlsl"},
                .compile_options = {.entry_point = "miss", .language = daxa::ShaderLanguage::SLANG},
            },
        },
        .shader_groups_infos = {
            // Gen Group
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::GENERAL,
                .general_shader_index = 0,
            },
            // Miss group
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::GENERAL,
                .general_shader_index = 3,
            },
            // Hit group
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::TRIANGLES_HIT_GROUP,
                .closest_hit_shader_index = 2,
            },
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::TRIANGLES_HIT_GROUP,
                .closest_hit_shader_index = 2,
                .any_hit_shader_index = 1,
            },
        },
        .max_ray_recursion_depth = 1,
        .push_constant_size = sizeof(RayTraceAmbientOcclusionPush),
        .name = std::string{RayTraceAmbientOcclusionH::NAME},
    };
}

struct RayTraceAmbientOcclusionTask : RayTraceAmbientOcclusionH::Task
{
    AttachmentViews views = {};
    GPUContext * context = {};
    RenderContext * r_context = {};

    void callback(daxa::TaskInterface ti)
    {
        if (ti.get(AT.tlas).ids[0] != context->dummy_tlas_id)
        {
            RayTraceAmbientOcclusionPush push = { };
            push.attach = ti.attachment_shader_blob;
            auto const & ao_image = ti.device.info_image(ti.get(AT.ao_image).ids[0]).value();
            if(r_context->render_data.settings.use_rt_pipeline_for_ao) 
            {
                auto const & rt_pipeline = context->ray_tracing_pipelines.at(ray_trace_ao_rt_pipeline_info().name);
                ti.recorder.set_pipeline(*rt_pipeline.pipeline);
                ti.recorder.push_constant(push);
                ti.recorder.trace_rays({
                    .width = ao_image.size.x,
                    .height = ao_image.size.y,
                    .depth = 1,
                    .shader_binding_table = rt_pipeline.sbt,
                });
            }
            else
            {
                ti.recorder.set_pipeline(*context->compute_pipelines.at(ray_trace_ao_compute_pipeline_info().name));
                ti.recorder.push_constant(push);
                u32 const dispatch_x = round_up_div(ao_image.size.x, RT_AO_X);
                u32 const dispatch_y = round_up_div(ao_image.size.y, RT_AO_Y);
                ti.recorder.dispatch({.x = dispatch_x, .y = dispatch_y, .z = 1});
            }
        }
    }
};

#endif