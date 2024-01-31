#include "asset_processor.hpp"
#include <daxa/types.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fstream>
#include <cstring>
#include <FreeImage.h>
#include <variant>

#pragma region IMAGE_RAW_DATA_LOADING_HELPERS
struct RawImageData
{
    std::vector<std::byte> raw_data;
    std::filesystem::path image_path;
    fastgltf::MimeType mime_type;
};

using RawDataRet = std::variant<std::monostate, AssetProcessor::AssetLoadResultCode, RawImageData>;

struct RawImageDataFromURIInfo
{
    fastgltf::sources::URI const & uri;
    fastgltf::Asset const & asset;
    // Wihtout the scename.glb part
    std::filesystem::path const scene_dir_path;
};

static auto raw_image_data_from_path(std::filesystem::path image_path) -> RawDataRet
{
    std::ifstream ifs{image_path, std::ios::binary};
    if (!ifs)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_OPEN_TEXTURE_FILE;
    }
    ifs.seekg(0, ifs.end);
    i32 const filesize = ifs.tellg();
    ifs.seekg(0, ifs.beg);
    std::vector<std::byte> raw(filesize);
    if (!ifs.read(r_cast<char *>(raw.data()), filesize))
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_READ_TEXTURE_FILE;
    }
    return RawImageData{
        .raw_data = std::move(raw),
        .image_path = image_path,
        .mime_type = {}};
}

static auto raw_image_data_from_URI(RawImageDataFromURIInfo const & info) -> RawDataRet
{
    /// NOTE: Having global paths in your gltf is just wrong. I guess we could later support them by trying to
    //        load the file anyways but cmon what are the chances of that being successful - for now let's just return error
    if (!info.uri.uri.isLocalPath())
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_UNSUPPORTED_ABSOLUTE_PATH;
    }
    /// NOTE: I don't really see how fileoffsets could be valid in a URI context. Since we have no information about the size
    //        of the data we always just load everything in the file. Having just a single offset thus does not allow to pack
    //        multiple images into a single file so we just error on this for now.
    if (info.uri.fileByteOffset != 0)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_URI_FILE_OFFSET_NOT_SUPPORTED;
    }
    std::filesystem::path const full_image_path = info.scene_dir_path / info.uri.uri.fspath();
    DEBUG_MSG(fmt::format("[AssetProcessor::raw_image_data_from_URI] Loading image {} ...", full_image_path.string()));
    RawDataRet raw_image_data_ret = raw_image_data_from_path(full_image_path);
    if (std::holds_alternative<AssetProcessor::AssetLoadResultCode>(raw_image_data_ret))
    {
        return raw_image_data_ret;
    }
    RawImageData & raw_data = std::get<RawImageData>(raw_image_data_ret);
    raw_data.mime_type = info.uri.mimeType;
    return raw_data;
}

struct RawImageDataFromBufferViewInfo
{
    fastgltf::sources::BufferView const & buffer_view;
    fastgltf::Asset const & asset;
    // Wihtout the scename.glb part
    std::filesystem::path const scene_dir_path;
};

static auto raw_image_data_from_buffer_view(RawImageDataFromBufferViewInfo const & info) -> RawDataRet
{
    fastgltf::BufferView const & gltf_buffer_view = info.asset.bufferViews.at(info.buffer_view.bufferViewIndex);
    fastgltf::Buffer const & gltf_buffer = info.asset.buffers.at(gltf_buffer_view.bufferIndex);

    if (!std::holds_alternative<fastgltf::sources::URI>(gltf_buffer.data))
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_FAULTY_BUFFER_VIEW;
    }
    fastgltf::sources::URI uri = std::get<fastgltf::sources::URI>(gltf_buffer.data);

    /// NOTE: load the section of the file containing the buffer for the mesh index buffer.
    std::filesystem::path const full_buffer_path = info.scene_dir_path / uri.uri.fspath();
    std::ifstream ifs{full_buffer_path, std::ios::binary};
    if (!ifs)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_OPEN_GLTF;
    }
    /// NOTE: Only load the relevant part of the file containing the view of the buffer we actually need.
    ifs.seekg(gltf_buffer_view.byteOffset + uri.fileByteOffset);
    std::vector<std::byte> raw = {};
    raw.resize(gltf_buffer_view.byteLength);
    /// NOTE: Only load the relevant part of the file containing the view of the buffer we actually need.
    if (!ifs.read(r_cast<char *>(raw.data()), gltf_buffer_view.byteLength))
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_READ_BUFFER_IN_GLTF;
    }
    return RawImageData{
        .raw_data = std::move(raw),
        .image_path = full_buffer_path,
        .mime_type = uri.mimeType};
}
#pragma engregion

#pragma region IMAGE_RAW_DATA_PARSING_HELPERS
struct ParsedImageData
{
    daxa::BufferId src_buffer;
    daxa::ImageId dst_image;
};

using ParsedImageRet = std::variant<std::monostate, AssetProcessor::AssetLoadResultCode, ParsedImageData>;

enum struct ChannelDataType
{
    SIGNED_INT,
    UNSIGNED_INT,
    FLOATING_POINT
};

struct ChannelInfo
{
    u8 byte_size;
    ChannelDataType data_type;
};
using ParsedChannel = std::variant<std::monostate, AssetProcessor::AssetLoadResultCode, ChannelInfo>;

constexpr static auto parse_channel_info(FREE_IMAGE_TYPE image_type) -> ParsedChannel
{
    ChannelInfo ret = {};
    switch (image_type)
    {
        case FREE_IMAGE_TYPE::FIT_BITMAP:
        {
            ret.byte_size = 1u;
            ret.data_type = ChannelDataType::UNSIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_UINT16:
        {
            ret.byte_size = 2u;
            ret.data_type = ChannelDataType::UNSIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_INT16:
        {
            ret.byte_size = 2u;
            ret.data_type = ChannelDataType::SIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_UINT32:
        {
            ret.byte_size = 4u;
            ret.data_type = ChannelDataType::UNSIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_INT32:
        {
            ret.byte_size = 4u;
            ret.data_type = ChannelDataType::SIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_FLOAT:
        {
            ret.byte_size = 4u;
            ret.data_type = ChannelDataType::FLOATING_POINT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_RGB16:
        {
            ret.byte_size = 2u;
            ret.data_type = ChannelDataType::UNSIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_RGBA16:
        {
            ret.byte_size = 2u;
            ret.data_type = ChannelDataType::UNSIGNED_INT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_RGBF:
        {
            ret.byte_size = 4u;
            ret.data_type = ChannelDataType::FLOATING_POINT;
            break;
        }
        case FREE_IMAGE_TYPE::FIT_RGBAF:
        {
            ret.byte_size = 4u;
            ret.data_type = ChannelDataType::FLOATING_POINT;
            break;
        }
        default:
            return AssetProcessor::AssetLoadResultCode::ERROR_UNSUPPORTED_TEXTURE_PIXEL_FORMAT;
    }
    return ret;
};

struct PixelInfo
{
    u8 channel_count;
    u8 channel_byte_size;
    ChannelDataType channel_data_type;
};

constexpr static auto daxa_image_format_from_pixel_info(PixelInfo const & info) -> daxa::Format
{
    std::array<std::array<std::array<daxa::Format, 3>, 4>, 3> translation = {
        // BYTE SIZE 1
        std::array{
            // CHANNEL COUNT 1
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R8_SRGB, daxa::Format::R8_SINT, daxa::Format::UNDEFINED}},
            // CHANNEL COUNT 2
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R8G8_SRGB, daxa::Format::R8G8_SINT, daxa::Format::UNDEFINED}},
            /// NOTE: Free image stores images in BGRA on little endians (Win,Linux) this will break on Mac
            // CHANNEL COUNT 3
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::B8G8R8A8_SRGB, daxa::Format::B8G8R8A8_SINT, daxa::Format::UNDEFINED}},
            // CHANNEL COUNT 4
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::B8G8R8A8_SRGB, daxa::Format::B8G8R8A8_SINT, daxa::Format::UNDEFINED}},
        },
        // BYTE SIZE 2
        std::array{
            // CHANNEL COUNT 1
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R16_UINT, daxa::Format::R16_SINT, daxa::Format::R16_SFLOAT}},
            // CHANNEL COUNT 2
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R16G16_UINT, daxa::Format::R16G16_SINT, daxa::Format::R16G16_SFLOAT}},
            // CHANNEL COUNT 3
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R16G16B16A16_UINT, daxa::Format::R16G16B16A16_SINT, daxa::Format::R16G16B16A16_SFLOAT}},
            // CHANNEL COUNT 4
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R16G16B16A16_UINT, daxa::Format::R16G16B16A16_SINT, daxa::Format::R16G16B16A16_SFLOAT}},
        },
        // BYTE SIZE 4
        std::array{
            // CHANNEL COUNT 1
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R32_UINT, daxa::Format::R32_SINT, daxa::Format::R32_SFLOAT}},
            // CHANNEL COUNT 2
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R32G32_UINT, daxa::Format::R32G32_SINT, daxa::Format::R32G32_SFLOAT}},
            // CHANNEL COUNT 3
            /// TODO: Channel count 3 might not be supported possible just replace with four channel alternatives
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R32G32B32_UINT, daxa::Format::R32G32B32_SINT, daxa::Format::R32G32B32_SFLOAT}},
            // CHANNEL COUNT 4
            std::array{/* CHANNEL FORMAT */ std::array{daxa::Format::R32G32B32A32_UINT, daxa::Format::R32G32B32A32_SINT, daxa::Format::R32G32B32A32_SFLOAT}},
        },
    };
    u8 channel_byte_size_idx{};
    switch (info.channel_byte_size)
    {
        case 1:
            channel_byte_size_idx = 0u;
            break;
        case 2:
            channel_byte_size_idx = 1u;
            break;
        case 4:
            channel_byte_size_idx = 2u;
            break;
        default:
            return daxa::Format::UNDEFINED;
    }
    u8 const channel_count_idx = info.channel_count - 1;
    u8 channel_format_idx{};
    switch (info.channel_data_type)
    {
        case ChannelDataType::UNSIGNED_INT:
            channel_format_idx = 0u;
            break;
        case ChannelDataType::SIGNED_INT:
            channel_format_idx = 1u;
            break;
        case ChannelDataType::FLOATING_POINT:
            channel_format_idx = 2u;
            break;
        default:
            return daxa::Format::UNDEFINED;
    }
    return translation[channel_byte_size_idx][channel_count_idx][channel_format_idx];
};

static auto free_image_parse_raw_image_data(RawImageData && raw_data, daxa::Device & device) -> ParsedImageRet
{
    /// NOTE: Since we handle the image data loading ourselves we need to wrap the buffer with a FreeImage
    //        wrapper so that it can internally process the data
    FIMEMORY * fif_memory_wrapper = FreeImage_OpenMemory(r_cast<BYTE *>(raw_data.raw_data.data()), raw_data.raw_data.size());
    defer { FreeImage_CloseMemory(fif_memory_wrapper); };
    FREE_IMAGE_FORMAT image_format = FreeImage_GetFileTypeFromMemory(fif_memory_wrapper, 0);
    // could not deduce filetype from metadata in memory try to guess the format from the file extension
    if (image_format == FIF_UNKNOWN)
    {
        image_format = FreeImage_GetFIFFromFilename(raw_data.image_path.string().c_str());
    }
    // could not deduce filetype at all
    if (image_format == FIF_UNKNOWN)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_UNKNOWN_FILETYPE_FORMAT;
    }
    if (!FreeImage_FIFSupportsReading(image_format))
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_UNSUPPORTED_READ_FOR_FILEFORMAT;
    }
    FIBITMAP * image_bitmap = FreeImage_LoadFromMemory(image_format, fif_memory_wrapper);
    defer { FreeImage_Unload(image_bitmap); };
    if (!image_bitmap)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_READ_TEXTURE_FILE_FROM_MEMSTREAM;
    }
    FREE_IMAGE_TYPE const image_type = FreeImage_GetImageType(image_bitmap);
    FREE_IMAGE_COLOR_TYPE const color_type = FreeImage_GetColorType(image_bitmap);
    u32 const bits_per_pixel = FreeImage_GetBPP(image_bitmap);
    u32 const width = FreeImage_GetWidth(image_bitmap);
    u32 const height = FreeImage_GetHeight(image_bitmap);
    bool const has_red_channel = FreeImage_GetRedMask(image_bitmap) != 0;
    bool const has_green_channel = FreeImage_GetGreenMask(image_bitmap) != 0;
    bool const has_blue_channel = FreeImage_GetBlueMask(image_bitmap) != 0;

    bool const should_contain_all_color_channels =
        (color_type == FREE_IMAGE_COLOR_TYPE::FIC_RGB) ||
        (color_type == FREE_IMAGE_COLOR_TYPE::FIC_RGBALPHA);
    bool const contains_all_color_channels = has_red_channel && has_green_channel && has_blue_channel;
    DBG_ASSERT_TRUE_M(should_contain_all_color_channels == contains_all_color_channels,
                      std::string("[ERROR][free_image_parse_raw_image_data()] Image color type indicates color channels present") +
                          std::string(" but not all channels were present accoring to color masks"));

    ParsedChannel parsed_channel = parse_channel_info(image_type);
    if (auto const * err = std::get_if<AssetProcessor::AssetLoadResultCode>(&parsed_channel))
    {
        return *err;
    }

    ChannelInfo const & channel_info = std::get<ChannelInfo>(parsed_channel);
    u32 const channel_count = bits_per_pixel / (channel_info.byte_size * 8u);

    daxa::Format daxa_image_format = daxa_image_format_from_pixel_info({
        .channel_count = s_cast<u8>(channel_count),
        .channel_byte_size = channel_info.byte_size,
        .channel_data_type = channel_info.data_type,
    });

    /// TODO: Breaks for 32bit 3 channel images (or overallocates idk)
    u32 const rounded_channel_count = channel_count == 3 ? 4 : channel_count;
    FIBITMAP * modified_bitmap;
    if (channel_count == 3)
    {
        modified_bitmap = FreeImage_ConvertTo32Bits(image_bitmap);
    }
    else
    {
        modified_bitmap = image_bitmap;
    }
    defer
    {
        if (channel_count == 3)
            FreeImage_Unload(modified_bitmap);
    };
    FreeImage_FlipVertical(modified_bitmap);
    ParsedImageData ret = {};
    u32 const total_image_byte_size = width * height * rounded_channel_count * channel_info.byte_size;
    ret.src_buffer = device.create_buffer({
        .size = total_image_byte_size,
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
        .name = raw_data.image_path.filename().string() + " staging",
    });
    std::byte * staging_dst_ptr = device.get_host_address_as<std::byte>(ret.src_buffer).value();
    memcpy(staging_dst_ptr, r_cast<std::byte *>(FreeImage_GetBits(modified_bitmap)), total_image_byte_size);

    ret.dst_image = device.create_image({
        .dimensions = 2,
        .format = daxa_image_format,
        .size = {width, height, 1},
        /// TODO: Add support for generating mip levels
        .mip_level_count = 1,
        .array_layer_count = 1,
        .sample_count = 1,
        /// TODO: Potentially take more flags from the user here
        .usage =
            daxa::ImageUsageFlagBits::TRANSFER_DST |
            daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = raw_data.image_path.filename().string(),
    });
    return ret;
}

#pragma endregion

AssetProcessor::AssetProcessor(daxa::Device device)
    : _device{std::move(device)}
{
// call this ONLY when linking with FreeImage as a static library
#ifdef FREEIMAGE_LIB
    FreeImage_Initialise();
#endif
}

AssetProcessor::~AssetProcessor()
{
// call this ONLY when linking with FreeImage as a static library
#ifdef FREEIMAGE_LIB
    FreeImage_DeInitialise();
#endif
}

auto AssetProcessor::load_nonmanifest_texture(std::filesystem::path const & filepath) -> NonmanifestLoadRet
{
    RawDataRet raw_data_ret = raw_image_data_from_path(filepath);
    if (std::holds_alternative<AssetProcessor::AssetLoadResultCode>(raw_data_ret))
    {
        return std::get<AssetProcessor::AssetLoadResultCode>(raw_data_ret);
    }
    RawImageData & raw_data = std::get<RawImageData>(raw_data_ret);
    ParsedImageRet parsed_data_ret = free_image_parse_raw_image_data(std::move(raw_data), _device);
    if (auto const * error = std::get_if<AssetProcessor::AssetLoadResultCode>(&parsed_data_ret))
    {
        return *error;
    }
    ParsedImageData const & parsed_data = std::get<ParsedImageData>(parsed_data_ret);

    auto recorder = _device.create_command_recorder({});
    recorder.destroy_buffer_deferred(parsed_data.src_buffer);
    recorder.pipeline_barrier_image_transition({
        .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
        .dst_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
        .image_id = parsed_data.dst_image,
    });

    recorder.copy_buffer_to_image({
        .buffer = parsed_data.src_buffer,
        .image = parsed_data.dst_image,
        .image_extent = _device.info_image(parsed_data.dst_image).value().size,
    });

    recorder.pipeline_barrier_image_transition({
        .src_access = daxa::AccessConsts::TRANSFER_WRITE,
        .dst_access = daxa::AccessConsts::ALL_GRAPHICS_READ,
        .src_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
        /// TODO: Take the usage from the user for now images only used as attachments
        .dst_layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
        .image_id = parsed_data.dst_image,
    });

    daxa::ExecutableCommandList command_list = recorder.complete_current_commands();
    _device.submit_commands({.command_lists = {&command_list, 1}});
    _device.wait_idle();
    return parsed_data.dst_image;
}

auto AssetProcessor::load_texture(Scene & scene, u32 texture_manifest_index) -> AssetLoadResultCode
{
    TextureManifestEntry const & texture_entry = scene._material_texture_manifest.at(texture_manifest_index);
    GltfAssetManifestEntry const & scene_entry = scene._gltf_asset_manifest.at(texture_entry.scene_file_manifest_index);
    fastgltf::Asset const & gltf_asset = scene_entry.gltf_asset;
    fastgltf::Image const & image = gltf_asset.images.at(texture_entry.in_scene_file_index);
    std::vector<std::byte> raw_data = {};

    RawDataRet ret = {};
    if (auto const * uri = std::get_if<fastgltf::sources::URI>(&image.data))
    {
        ret = std::move(raw_image_data_from_URI(RawImageDataFromURIInfo{
            .uri = *uri,
            .asset = gltf_asset,
            .scene_dir_path = std::filesystem::path(scene_entry.path).remove_filename(),
        }));
    }
    else if (auto const * buffer_view = std::get_if<fastgltf::sources::BufferView>(&image.data))
    {
        ret = std::move(raw_image_data_from_buffer_view(RawImageDataFromBufferViewInfo{
            .buffer_view = *buffer_view,
            .asset = gltf_asset,
            .scene_dir_path = std::filesystem::path(scene_entry.path).remove_filename(),
        }));
    }
    else
    {
        return AssetLoadResultCode::ERROR_FAULTY_BUFFER_VIEW;
    }

    if (auto const * error = std::get_if<AssetLoadResultCode>(&ret))
    {
        return *error;
    }
    RawImageData & raw_image_data = std::get<RawImageData>(ret);
    ParsedImageRet parsed_data_ret;
    if (raw_image_data.mime_type == fastgltf::MimeType::KTX2)
    {
        // KTX handles image loading
    }
    else
    {
        // FreeImage handles image loading
        parsed_data_ret = free_image_parse_raw_image_data(std::move(raw_image_data), _device);
    }
    if (auto const * error = std::get_if<AssetProcessor::AssetLoadResultCode>(&parsed_data_ret))
    {
        return *error;
    }
    ParsedImageData const & parsed_data = std::get<ParsedImageData>(parsed_data_ret);
    /// NOTE: Append the processed texture to the upload queue.
    {
        std::unique_lock l{*_mtx};
        _upload_texture_queue.push_back(TextureUpload{
            .scene = &scene,
            .staging_buffer = parsed_data.src_buffer,
            .dst_image = parsed_data.dst_image,
            .texture_manifest_index = texture_manifest_index});
    }
    return AssetLoadResultCode::SUCCESS;
}

/// NOTE: Overload ElementTraits for glm vec3 for fastgltf to understand the type.
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<float, fastgltf::AccessorType::Vec3>
{
};

template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<float, fastgltf::AccessorType::Vec2>
{
};

template <typename ElemT, bool IS_INDEX_BUFFER>
auto load_accessor_data_from_file(
    std::filesystem::path const & root_path,
    fastgltf::Asset const & gltf_asset,
    fastgltf::Accessor const & accesor)
    -> std::variant<std::vector<ElemT>, AssetProcessor::AssetLoadResultCode>
{
    static_assert(!IS_INDEX_BUFFER || std::is_same_v<ElemT, u32>, "Index Buffer must be u32");
    fastgltf::BufferView const & gltf_buffer_view = gltf_asset.bufferViews.at(accesor.bufferViewIndex.value());
    fastgltf::Buffer const & gltf_buffer = gltf_asset.buffers.at(gltf_buffer_view.bufferIndex);
    if (!std::holds_alternative<fastgltf::sources::URI>(gltf_buffer.data))
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_FAULTY_BUFFER_VIEW;
    }
    fastgltf::sources::URI uri = std::get<fastgltf::sources::URI>(gltf_buffer.data);

    /// NOTE: load the section of the file containing the buffer for the mesh index buffer.
    std::filesystem::path const full_buffer_path = root_path / uri.uri.fspath();
    std::ifstream ifs{full_buffer_path, std::ios::binary};
    if (!ifs)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_OPEN_GLTF;
    }
    /// NOTE: Only load the relevant part of the file containing the view of the buffer we actually need.
    ifs.seekg(gltf_buffer_view.byteOffset + accesor.byteOffset + uri.fileByteOffset);
    std::vector<u16> raw = {};
    raw.resize(gltf_buffer_view.byteLength / 2);
    /// NOTE: Only load the relevant part of the file containing the view of the buffer we actually need.
    auto const elem_byte_size = fastgltf::getElementByteSize(accesor.type, accesor.componentType);
    if (!ifs.read(r_cast<char *>(raw.data()), accesor.count * elem_byte_size))
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_COULD_NOT_READ_BUFFER_IN_GLTF;
    }
    auto buffer_adapter = [&](fastgltf::Buffer const & buffer)
    {
        /// NOTE:   We only have a ptr to the loaded data to the accessors section of the buffer.
        ///         Fastgltf expects a ptr to the begin of the buffer, so we just subtract the offsets.
        ///         Fastgltf adds these on in the accessor tool, so in the end it gets the right ptr.
        auto const fastgltf_reverse_byte_offset = (gltf_buffer_view.byteOffset + accesor.byteOffset);
        return r_cast<std::byte *>(raw.data()) - fastgltf_reverse_byte_offset;
    };

    std::vector<ElemT> ret(accesor.count);
    if constexpr (IS_INDEX_BUFFER)
    {
        /// NOTE: Transform the loaded file section into a 32 bit index buffer.
        if (accesor.componentType == fastgltf::ComponentType::UnsignedShort)
        {
            std::vector<u16> u16_index_buffer(accesor.count);
            fastgltf::copyFromAccessor<u16>(gltf_asset, accesor, u16_index_buffer.data(), buffer_adapter);
            for (size_t i = 0; i < u16_index_buffer.size(); ++i)
            {
                ret[i] = s_cast<u32>(u16_index_buffer[i]);
            }
        }
        else
        {
            fastgltf::copyFromAccessor<u32>(gltf_asset, accesor, ret.data(), buffer_adapter);
        }
    }
    else
    {
        fastgltf::copyFromAccessor<ElemT>(gltf_asset, accesor, ret.data(), buffer_adapter);
    }
    return {std::move(ret)};
}

auto AssetProcessor::load_mesh(LoadMeshInfo const & info) -> MeshLoadRet
{
    fastgltf::Asset & gltf_asset = info.asset;
    fastgltf::Mesh & gltf_mesh = gltf_asset.meshes[info.gltf_mesh_index];
    fastgltf::Primitive & gltf_prim = gltf_mesh.primitives[info.gltf_primitive_index];

/// NOTE: Process indices (they are required)
#pragma region INDICES
    if (!gltf_prim.indicesAccessor.has_value())
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_MISSING_INDEX_BUFFER;
    }
    fastgltf::Accessor & index_buffer_gltf_accessor = gltf_asset.accessors.at(gltf_prim.indicesAccessor.value());
    bool const index_buffer_accessor_valid =
        (index_buffer_gltf_accessor.componentType == fastgltf::ComponentType::UnsignedInt ||
         index_buffer_gltf_accessor.componentType == fastgltf::ComponentType::UnsignedShort) &&
        index_buffer_gltf_accessor.type == fastgltf::AccessorType::Scalar &&
        index_buffer_gltf_accessor.bufferViewIndex.has_value();
    if (!index_buffer_accessor_valid)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_FAULTY_INDEX_BUFFER_GLTF_ACCESSOR;
    }
    auto index_buffer_data = load_accessor_data_from_file<u32, true>(std::filesystem::path{info.asset_path}.remove_filename(), gltf_asset, index_buffer_gltf_accessor);
    if (auto const * err = std::get_if<AssetProcessor::AssetLoadResultCode>(&index_buffer_data))
    {
        return *err;
    }
    std::vector<u32> index_buffer = std::get<std::vector<u32>>(std::move(index_buffer_data));
#pragma endregion

/// NOTE: Load vertex positions
#pragma region VERTICES
    auto vert_attrib_iter = gltf_prim.findAttribute(VERT_ATTRIB_POSITION_NAME);
    if (vert_attrib_iter == gltf_prim.attributes.end())
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_MISSING_VERTEX_POSITIONS;
    }
    fastgltf::Accessor & gltf_vertex_pos_accessor = gltf_asset.accessors.at(vert_attrib_iter->second);
    bool const gltf_vertex_pos_accessor_valid =
        gltf_vertex_pos_accessor.componentType == fastgltf::ComponentType::Float &&
        gltf_vertex_pos_accessor.type == fastgltf::AccessorType::Vec3;
    if (!gltf_vertex_pos_accessor_valid)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_FAULTY_GLTF_VERTEX_POSITIONS;
    }
    // TODO: we can probably load this directly into the staging buffer.
    auto vertex_pos_result = load_accessor_data_from_file<glm::vec3, false>(std::filesystem::path{info.asset_path}.remove_filename(), gltf_asset, gltf_vertex_pos_accessor);
    if (auto const * err = std::get_if<AssetProcessor::AssetLoadResultCode>(&vertex_pos_result))
    {
        return *err;
    }
    std::vector<glm::vec3> vert_positions = std::get<std::vector<glm::vec3>>(std::move(vertex_pos_result));
    u32 const vertex_count = s_cast<u32>(vert_positions.size());
#pragma endregion

/// NOTE: Load vertex UVs
#pragma region UVS
    auto texcoord0_attrib_iter = gltf_prim.findAttribute(VERT_ATTRIB_TEXCOORD0_NAME);
    if (texcoord0_attrib_iter == gltf_prim.attributes.end())
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_MISSING_VERTEX_TEXCOORD_0;
    }
    fastgltf::Accessor & gltf_vertex_texcoord0_accessor = gltf_asset.accessors.at(texcoord0_attrib_iter->second);
    bool const gltf_vertex_texcoord0_accessor_valid =
        gltf_vertex_texcoord0_accessor.componentType == fastgltf::ComponentType::Float &&
        gltf_vertex_texcoord0_accessor.type == fastgltf::AccessorType::Vec2;
    if (!gltf_vertex_texcoord0_accessor_valid)
    {
        return AssetProcessor::AssetLoadResultCode::ERROR_FAULTY_GLTF_VERTEX_TEXCOORD_0;
    }
    auto vertex_texcoord0_pos_result = load_accessor_data_from_file<glm::vec2, false>(std::filesystem::path{info.asset_path}.remove_filename(), gltf_asset, gltf_vertex_texcoord0_accessor);
    if (auto const * err = std::get_if<AssetProcessor::AssetLoadResultCode>(&vertex_texcoord0_pos_result))
    {
        return *err;
    }
    std::vector<glm::vec2> vert_texcoord0 = std::get<std::vector<glm::vec2>>(std::move(vertex_texcoord0_pos_result));
    DBG_ASSERT_TRUE_M(vert_texcoord0.size() == vert_positions.size(), "[AssetProcessor::load_mesh()] Mismatched position and uv count");
#pragma endregion

    /// NOTE: Generate meshlets:
    constexpr usize MAX_VERTICES = MAX_VERTICES_PER_MESHLET;
    constexpr usize MAX_TRIANGLES = MAX_TRIANGLES_PER_MESHLET;
    // No clue what cone culling is.
    constexpr float CONE_WEIGHT = 1.0f;
    // TODO: Make this optimization optional!
    {
        std::vector<u32> optimized_indices(index_buffer.size());
        meshopt_optimizeVertexCache(optimized_indices.data(), index_buffer.data(), index_buffer.size(), vertex_count);
        index_buffer = std::move(optimized_indices);
    }
    size_t max_meshlets = meshopt_buildMeshletsBound(index_buffer.size(), MAX_VERTICES, MAX_TRIANGLES);
    std::vector<meshopt_Meshlet> meshlets(max_meshlets);
    std::vector<u32> meshlet_indirect_vertices(max_meshlets * MAX_VERTICES);
    std::vector<u8> meshlet_micro_indices(max_meshlets * MAX_TRIANGLES * 3);
    size_t meshlet_count = meshopt_buildMeshlets(
        meshlets.data(),
        meshlet_indirect_vertices.data(),
        meshlet_micro_indices.data(),
        index_buffer.data(),
        index_buffer.size(),
        r_cast<float *>(vert_positions.data()),
        s_cast<usize>(vertex_count),
        sizeof(glm::vec3),
        MAX_VERTICES,
        MAX_TRIANGLES,
        CONE_WEIGHT);
    // TODO: Compute OBBs
    std::vector<BoundingSphere> meshlet_bounds(meshlet_count);
    for (size_t meshlet_i = 0; meshlet_i < meshlet_count; ++meshlet_i)
    {
        meshopt_Bounds raw_bounds = meshopt_computeMeshletBounds(
            &meshlet_indirect_vertices[meshlets[meshlet_i].vertex_offset],
            &meshlet_micro_indices[meshlets[meshlet_i].triangle_offset],
            meshlets[meshlet_i].triangle_count,
            r_cast<float *>(vert_positions.data()),
            s_cast<usize>(vertex_count),
            sizeof(glm::vec3));
        meshlet_bounds[meshlet_i].center.x = raw_bounds.center[0];
        meshlet_bounds[meshlet_i].center.y = raw_bounds.center[1];
        meshlet_bounds[meshlet_i].center.z = raw_bounds.center[2];
        meshlet_bounds[meshlet_i].radius = raw_bounds.radius;
    }
    // Trimm array sizes.
    meshopt_Meshlet const & last = meshlets[meshlet_count - 1];
    meshlet_indirect_vertices.resize(last.vertex_offset + last.vertex_count);
    meshlet_micro_indices.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3));
    meshlets.resize(meshlet_count);

    u32 const total_mesh_buffer_size = 
        sizeof(Meshlet) * meshlet_count + 
        sizeof(BoundingSphere) * meshlet_count + 
        sizeof(u8) * meshlet_micro_indices.size() + 
        sizeof(u32) * meshlet_indirect_vertices.size() +
        sizeof(daxa_f32vec3) * vert_positions.size() +
        sizeof(daxa_f32vec2) * vert_texcoord0.size();

    /// NOTE: Fill GPUMesh runtime data
    GPUMesh mesh = {};
    mesh.mesh_buffer = _device.create_buffer({
        .size = s_cast<daxa::usize>(total_mesh_buffer_size),
        .name = gltf_mesh.name.c_str(),
    });
    daxa::DeviceAddress mesh_bda = _device.get_device_address(std::bit_cast<daxa::BufferId>(mesh.mesh_buffer)).value();

    daxa::BufferId staging_buffer = _device.create_buffer({
        .size = s_cast<daxa::usize>(total_mesh_buffer_size),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
        .name = std::string(gltf_mesh.name.c_str()) + " staging",
    });
    auto staging_ptr = _device.get_host_address(staging_buffer).value();

    u32 accumulated_offset = 0;
    // ---
    mesh.meshlets = mesh_bda + accumulated_offset;
    std::memcpy(
        staging_ptr + accumulated_offset,
        meshlets.data(),
        meshlets.size() * sizeof(Meshlet));
    accumulated_offset += sizeof(Meshlet) * meshlet_count;
    // ---
    mesh.meshlet_bounds = mesh_bda + accumulated_offset;
    std::memcpy(
        staging_ptr + accumulated_offset,
        meshlet_bounds.data(),
        meshlet_bounds.size() * sizeof(BoundingSphere));
    accumulated_offset += sizeof(BoundingSphere) * meshlet_count;
    // ---
    DBG_ASSERT_TRUE_M(meshlet_micro_indices.size() % 4 == 0, "Damn");
    mesh.micro_indices = mesh_bda + accumulated_offset;
    std::memcpy(
        staging_ptr + accumulated_offset,
        meshlet_micro_indices.data(),
        meshlet_micro_indices.size() * sizeof(u8));
    accumulated_offset += sizeof(u8) * meshlet_micro_indices.size();
    // ---
    mesh.indirect_vertices = mesh_bda + accumulated_offset;
    std::memcpy(
        staging_ptr + accumulated_offset,
        meshlet_indirect_vertices.data(),
        meshlet_indirect_vertices.size() * sizeof(u32));
    accumulated_offset += sizeof(u32) * meshlet_indirect_vertices.size();
    // ---
    mesh.vertex_positions = mesh_bda + accumulated_offset;
    std::memcpy(
        staging_ptr + accumulated_offset,
        vert_positions.data(),
        vert_positions.size() * sizeof(daxa_f32vec3));
    accumulated_offset += sizeof(daxa_f32vec3) * vert_positions.size();
    // ---
    mesh.vertex_uvs = mesh_bda + accumulated_offset;
    std::memcpy(
        staging_ptr + accumulated_offset,
        vert_texcoord0.data(),
        vert_positions.size() * sizeof(daxa_f32vec2));
    // ---
    /// TODO: If there is no material index add default debug material?
    mesh.material_index = info.global_material_manifest_offset + gltf_prim.materialIndex.value();
    mesh.meshlet_count = meshlet_count;
    mesh.vertex_count = vertex_count;


    /// NOTE: Append the processed mesh to the upload queue.
    {
        std::unique_lock l{*_mtx};
        _upload_mesh_queue.push_back(MeshUpload{
            .staging_buffer = staging_buffer,
            .mesh_buffer = std::bit_cast<daxa::BufferId>(mesh.mesh_buffer),
        });
    }
    return mesh;
}

auto AssetProcessor::record_gpu_load_processing_commands() -> daxa::ExecutableCommandList
{
    std::unique_lock l{*_mtx};
    auto recorder = _device.create_command_recorder({});
#pragma region RECORD_MESH_UPLOAD_COMMANDS
    for (MeshUpload & mesh_upload : _upload_mesh_queue)
    {
        /// NOTE: copy from staging buffer to buffer and delete staging memory.
        recorder.copy_buffer_to_buffer({
            .src_buffer = mesh_upload.staging_buffer,
            .dst_buffer = mesh_upload.mesh_buffer,
            .size = _device.info_buffer(mesh_upload.mesh_buffer).value().size,
        });
        recorder.destroy_buffer_deferred(mesh_upload.staging_buffer);

        // /// NOTE: write an update to the meshes info buffer array.
        // MeshManifestEntry & mesh_entry = mesh_upload.scene->_mesh_manifest.at(mesh_upload.mesh_manifest_index);
        // auto const gpu_meshes_buffer = mesh_upload.scene->_gpu_mesh_group_manifest.get_state().buffers[0];
        // // TODO: replace staging buffer with offset into staging memory pool!
        // auto const meshes_buffer_update_staging_buffer = _device.create_buffer({
        //     .size = sizeof(GPUMesh),
        //     .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
        //     .name = "gpumeshes update",
        // });

        // recorder.destroy_buffer_deferred(meshes_buffer_update_staging_buffer);
        // auto const & mesh_descriptor = mesh_entry.runtime.value();
        // auto const mesh_buffer_bda = _device.get_device_address(mesh_buffer).value();
        // *_device.get_host_address_as<GPUMesh>(meshes_buffer_update_staging_buffer).value() = {
        //     .mesh_buffer = mesh_descriptor.mesh_buffer,
        //     .material_index = mesh_descriptor.material_index,
        //     .meshlet_count = mesh_descriptor.meshlet_count,
        //     .vertex_count = mesh_descriptor.vertex_count,
        //     .meshlets = mesh_buffer_bda + mesh_descriptor.offset_meshlets,
        //     .meshlet_bounds = mesh_buffer_bda + mesh_descriptor.offset_meshlet_bounds,
        //     .micro_indices = mesh_buffer_bda + mesh_descriptor.offset_micro_indices,
        //     .indirect_vertices = mesh_buffer_bda + mesh_descriptor.offset_indirect_vertices,
        //     .vertex_positions = mesh_buffer_bda + mesh_descriptor.offset_vertex_positions,
        //     .vertex_uvs = mesh_buffer_bda + mesh_descriptor.offset_vertex_texcoodrs0,
        // };
        // daxa::BufferId gpu_mesh_manifest = mesh_upload.scene->_gpu_mesh_manifest.get_state().buffers[0];
        // /// NOTE: Write the mesh manifest on the gpu.
        // recorder.copy_buffer_to_buffer({
        //     .src_buffer = meshes_buffer_update_staging_buffer,
        //     .dst_buffer = gpu_mesh_manifest,
        //     .dst_offset = sizeof(GPUMesh) * mesh_upload.mesh_manifest_index,
        //     .size = sizeof(GPUMesh),
        // });
        // recorder.destroy_buffer_deferred(staging_buffer);
        // /// NOTE: Copy the actual mesh data from the staging buffer to the actual buffer.
        // recorder.copy_buffer_to_buffer({
        //     .src_buffer = mesh_upload.staging_buffer,
        //     .dst_buffer = mesh_buffer,
        //     .size = _device.info_buffer(mesh_upload.staging_buffer).value().size,
        // });
        // recorder.destroy_buffer_deferred(mesh_upload.staging_buffer);
    }
    recorder.pipeline_barrier({
        .src_access = daxa::AccessConsts::TRANSFER_WRITE,
        .dst_access = daxa::AccessConsts::READ,
    });
    _upload_mesh_queue.clear();
#pragma endregion

#pragma region RECORD_TEXTURE_UPLOAD_COMMANDS
    for (TextureUpload const & texture_upload : _upload_texture_queue)
    {
        texture_upload.scene->_material_texture_manifest.at(texture_upload.texture_manifest_index).runtime = texture_upload.dst_image;
        /// TODO: If we are generating mips this will need to change
        recorder.pipeline_barrier_image_transition({
            .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
            .dst_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .image_id = texture_upload.dst_image,
        });
    }
    for (TextureUpload const & texture_upload : _upload_texture_queue)
    {
        recorder.copy_buffer_to_image({
            .buffer = texture_upload.staging_buffer,
            .image = texture_upload.dst_image,
            .image_offset = {0, 0, 0},
            .image_extent = _device.info_image(texture_upload.dst_image).value().size,
        });
        recorder.destroy_buffer_deferred(texture_upload.staging_buffer);
    }
    for (TextureUpload const & texture_upload : _upload_texture_queue)
    {
        recorder.pipeline_barrier_image_transition({
            .src_access = daxa::AccessConsts::TRANSFER_WRITE,
            .dst_access = daxa::AccessConsts::TOP_OF_PIPE_READ_WRITE,
            .src_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .dst_layout = daxa::ImageLayout::READ_ONLY_OPTIMAL,
            .image_id = texture_upload.dst_image,
        });
    }
#pragma endregion
#pragma region RECORD_MATERIAL_UPLOAD_COMMANDS
    /// NOTE: We need to propagate each loaded texture image ID into the material manifest This will be done in two steps:
    //        1) We update the CPU manifest with the correct values and remember the materials that were updated
    //        2) For each dirty material we generate a copy buffer to buffer comand to update the GPU manifest
    std::vector<u32> dirty_material_entry_indices = {};
    // 1) Update CPU Manifest
    for (TextureUpload const & texture_upload : _upload_texture_queue)
    {
        auto const & texture_manifest = texture_upload.scene->_material_texture_manifest;
        auto & material_manifest = texture_upload.scene->_material_manifest;
        TextureManifestEntry const & texture_manifest_entry = texture_manifest.at(texture_upload.texture_manifest_index);
        for (auto const material_using_texture_info : texture_manifest_entry.material_manifest_indices)
        {
            MaterialManifestEntry & material_entry = material_manifest.at(material_using_texture_info.material_manifest_index);
            if (material_using_texture_info.diffuse)
            {
                material_entry.diffuse_tex_index = texture_upload.texture_manifest_index;
            }
            if (material_using_texture_info.normal)
            {
                material_entry.normal_tex_index = texture_upload.texture_manifest_index;
            }
            /// NOTE: Add material index only if it was not added previously
            if (std::find(
                    dirty_material_entry_indices.begin(),
                    dirty_material_entry_indices.end(),
                    material_using_texture_info.material_manifest_index) ==
                dirty_material_entry_indices.end())
            {
                dirty_material_entry_indices.push_back(material_using_texture_info.material_manifest_index);
            }
        }
    }
    // 2) Update GPU manifest
    daxa::BufferId materials_update_staging_buffer;
    GPUMaterial * staging_origin_ptr;
    if (dirty_material_entry_indices.size())
    {
        materials_update_staging_buffer = _device.create_buffer({
            .size = sizeof(GPUMaterial) * dirty_material_entry_indices.size(),
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
            .name = "gpu materials update",
        });
        recorder.destroy_buffer_deferred(materials_update_staging_buffer);
        staging_origin_ptr = _device.get_host_address_as<GPUMaterial>(materials_update_staging_buffer).value();
    }
    for (u32 dirty_materials_index = 0; dirty_materials_index < dirty_material_entry_indices.size(); dirty_materials_index++)
    {
        auto const & texture_manifest = _upload_texture_queue.at(0).scene->_material_texture_manifest;
        auto const & material_manifest = _upload_texture_queue.at(0).scene->_material_manifest;
        MaterialManifestEntry const & material = material_manifest.at(dirty_material_entry_indices.at(dirty_materials_index));
        daxa::ImageId diffuse_id = {};
        daxa::ImageId normal_id = {};
        if (material.diffuse_tex_index.has_value())
        {
            diffuse_id = texture_manifest.at(material.diffuse_tex_index.value()).runtime.value();
        }
        if (material.normal_tex_index.has_value())
        {
            normal_id = texture_manifest.at(material.normal_tex_index.value()).runtime.value();
        }
        staging_origin_ptr[dirty_materials_index].diffuse_texture_id = diffuse_id.default_view();
        staging_origin_ptr[dirty_materials_index].normal_texture_id = normal_id.default_view();

        daxa::BufferId gpu_material_manifest = _upload_texture_queue.at(0).scene->_gpu_material_manifest.get_state().buffers[0];
        recorder.copy_buffer_to_buffer({
            .src_buffer = materials_update_staging_buffer,
            .dst_buffer = gpu_material_manifest,
            .src_offset = sizeof(GPUMaterial) * dirty_materials_index,
            .dst_offset = sizeof(GPUMaterial) * dirty_material_entry_indices.at(dirty_materials_index),
            .size = sizeof(GPUMaterial),
        });
    }
    recorder.pipeline_barrier({
        .src_access = daxa::AccessConsts::TRANSFER_WRITE,
        .dst_access = daxa::AccessConsts::READ,
    });
    _upload_texture_queue.clear();
#pragma endregion
    return recorder.complete_current_commands();
}

// auto AssetProcessor::load_all(Scene & scene) -> AssetProcessor::AssetLoadResultCode
// {
//     std::optional<AssetProcessor::AssetLoadResultCode> err = {};
//     // for (u32 i = 0; i < scene._material_texture_manifest.size(); ++i)
//     // {
//     //     if (!scene._material_texture_manifest.at(i).runtime.has_value())
//     //     {
//     //         auto result = load_texture(scene, i);
//     //         if (result != AssetProcessor::AssetLoadResultCode::SUCCESS && !err.has_value())
//     //         {
//     //             err = result;
//     //         }
//     //     }
//     // }
//     for (u32 i = 0; i < scene._mesh_manifest.size(); ++i)
//     {
//         if (!scene._mesh_manifest.at(i).runtime.has_value())
//         {
//             auto result = load_mesh(scene, i);
//             if (result != AssetProcessor::AssetLoadResultCode::SUCCESS && !err.has_value())
//             {
//                 err = result;
//             }
//         }
//     }
//     if (err.has_value())
//     {
//         return err.value();
//     }
//     return AssetProcessor::AssetLoadResultCode::SUCCESS;
// }