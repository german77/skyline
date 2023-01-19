// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 yuzu Team and Contributors (https://github.com/yuzu-emu/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/texture/texture.h>
#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/common/pipeline.inc>
#include <gpu/interconnect/common/file_pipeline_state_accessor.h>
#include <gpu/cache/graphics_pipeline_cache.h>
#include <gpu/shader_manager.h>
#include <gpu.h>
#include "graphics_pipeline_state_accessor.h"
#include "pipeline_manager.h"

namespace skyline::gpu::interconnect::maxwell3d {
    static constexpr Shader::Stage ConvertCompilerShaderStage(engine::Pipeline::Shader::Type stage) {
        switch (stage) {
            case engine::Pipeline::Shader::Type::VertexCullBeforeFetch:
                return Shader::Stage::VertexA;
            case engine::Pipeline::Shader::Type::Vertex:
                return Shader::Stage::VertexB;
            case engine::Pipeline::Shader::Type::TessellationInit:
                return Shader::Stage::TessellationControl;
            case engine::Pipeline::Shader::Type::Tessellation:
                return Shader::Stage::TessellationEval;
            case engine::Pipeline::Shader::Type::Geometry:
                return Shader::Stage::Geometry;
            case engine::Pipeline::Shader::Type::Pixel:
                return Shader::Stage::Fragment;
            default:
                throw exception("Invalid shader stage: {}", stage);
        }
    }

    static vk::ShaderStageFlagBits ConvertVkShaderStage(engine::Pipeline::Shader::Type stage) {
        switch (stage) {
            case engine::Pipeline::Shader::Type::VertexCullBeforeFetch:
            case engine::Pipeline::Shader::Type::Vertex:
                return vk::ShaderStageFlagBits::eVertex;
            case engine::Pipeline::Shader::Type::TessellationInit:
                return vk::ShaderStageFlagBits::eTessellationControl;
            case engine::Pipeline::Shader::Type::Tessellation:
                return vk::ShaderStageFlagBits::eTessellationEvaluation;
            case engine::Pipeline::Shader::Type::Geometry:
                return vk::ShaderStageFlagBits::eGeometry;
            case engine::Pipeline::Shader::Type::Pixel:
                return vk::ShaderStageFlagBits::eFragment;
            default:
                throw exception("Invalid shader stage: {}", stage);
        }
    }

    static Shader::TessPrimitive ConvertShaderTessPrimitive(engine::TessellationParameters::DomainType domainType) {
        switch (domainType) {
            case engine::TessellationParameters::DomainType::Isoline:
                return Shader::TessPrimitive::Isolines;
            case engine::TessellationParameters::DomainType::Triangle:
                return Shader::TessPrimitive::Triangles;
            case engine::TessellationParameters::DomainType::Quad:
                return Shader::TessPrimitive::Quads;
        }
    }

    static Shader::TessSpacing ConvertShaderTessSpacing(engine::TessellationParameters::Spacing spacing) {
        switch (spacing) {
            case engine::TessellationParameters::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case engine::TessellationParameters::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            case engine::TessellationParameters::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
        }
    }

    static Shader::AttributeType ConvertShaderAttributeType(engine::VertexAttribute attribute) {
        if (attribute.source == engine::VertexAttribute::Source::Inactive)
            return Shader::AttributeType::Disabled;

        switch (attribute.numericalType) {
            case engine::VertexAttribute::NumericalType::Snorm:
            case engine::VertexAttribute::NumericalType::Unorm:
            case engine::VertexAttribute::NumericalType::Float:
                return Shader::AttributeType::Float;
            case engine::VertexAttribute::NumericalType::Sint:
                return Shader::AttributeType::SignedInt;
            case engine::VertexAttribute::NumericalType::Uint:
                return Shader::AttributeType::UnsignedInt;
            case engine::VertexAttribute::NumericalType::Sscaled:
                return Shader::AttributeType::SignedScaled;
            case engine::VertexAttribute::NumericalType::Uscaled:
                return Shader::AttributeType::UnsignedScaled;
            default:
                throw exception("Invalid numerical type: {}", static_cast<u8>(attribute.numericalType));
        }
    }

    static Shader::OutputTopology ConvertShaderOutputTopology(engine::DrawTopology topology) {
        switch (topology) {
            case engine::DrawTopology::Points:
                return Shader::OutputTopology::PointList;
            case engine::DrawTopology::LineStrip:
                return Shader::OutputTopology::LineStrip;
            default:
                return Shader::OutputTopology::TriangleStrip;
        }
    }
    /**
     * @notes Roughly based on https://github.com/yuzu-emu/yuzu/blob/4ffbbc534884841f9a5536e57539bf3d1642af26/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L127
     */
    static Shader::RuntimeInfo MakeRuntimeInfo(const PackedPipelineState &packedState, Shader::IR::Program &program, Shader::IR::Program *lastProgram, bool hasGeometry) {
        Shader::RuntimeInfo info;
        if (lastProgram) {
            info.previous_stage_stores = lastProgram->info.stores;
            if (lastProgram->is_geometry_passthrough)
                info.previous_stage_stores.mask |= lastProgram->info.passthrough.mask;
        } else {
            info.previous_stage_stores.mask.set();
        }

        switch (program.stage) {
            case Shader::Stage::VertexB:
                if (!hasGeometry) {
                    if (packedState.topology == engine::DrawTopology::Points)
                        info.fixed_state_point_size = packedState.pointSize;

                    if (packedState.transformFeedbackEnable)
                        info.xfb_varyings = packedState.GetTransformFeedbackVaryings();

                    info.convert_depth_mode = packedState.openGlNdc;
                }
                ranges::transform(packedState.vertexAttributes, info.generic_input_types.begin(), &ConvertShaderAttributeType);
                break;
            case Shader::Stage::TessellationEval:
                info.tess_clockwise = packedState.outputPrimitives != engine::TessellationParameters::OutputPrimitives::TrianglesCCW;
                info.tess_primitive = ConvertShaderTessPrimitive(packedState.domainType);
                info.tess_spacing = ConvertShaderTessSpacing(packedState.spacing);
                break;
            case Shader::Stage::Geometry:
                if (program.output_topology == Shader::OutputTopology::PointList)
                    info.fixed_state_point_size = packedState.pointSize;

                if (packedState.transformFeedbackEnable)
                    info.xfb_varyings = packedState.GetTransformFeedbackVaryings();

                info.convert_depth_mode = packedState.openGlNdc;
                break;
            case Shader::Stage::Fragment:
                if (packedState.alphaTestEnable) {
                    info.alpha_test_func = packedState.GetAlphaFunc();
                    info.alpha_test_reference = packedState.alphaRef;
                }

                break;
            default:
                break;
        }
        switch (packedState.topology) {
            case engine::DrawTopology::Points:
                info.input_topology = Shader::InputTopology::Points;
                break;
            case engine::DrawTopology::Lines:
            case engine::DrawTopology::LineLoop:
            case engine::DrawTopology::LineStrip:
                info.input_topology = Shader::InputTopology::Lines;
                break;
            case engine::DrawTopology::Triangles:
            case engine::DrawTopology::TriangleStrip:
            case engine::DrawTopology::TriangleFan:
            case engine::DrawTopology::Quads:
            case engine::DrawTopology::QuadStrip:
            case engine::DrawTopology::Polygon:
            case engine::DrawTopology::Patch:
                info.input_topology = Shader::InputTopology::Triangles;
                break;
            case engine::DrawTopology::LineListAdjcy:
            case engine::DrawTopology::LineStripAdjcy:
                info.input_topology = Shader::InputTopology::LinesAdjacency;
                break;
            case engine::DrawTopology::TriangleListAdjcy:
            case engine::DrawTopology::TriangleStripAdjcy:
                info.input_topology = Shader::InputTopology::TrianglesAdjacency;
                break;
        }
        info.force_early_z = packedState.apiMandatedEarlyZ;
        info.y_negate = packedState.flipYEnable;
        return info;
    }

    static std::array<Pipeline::ShaderStage, engine::ShaderStageCount> MakePipelineShaders(GPU &gpu, const PipelineStateAccessor &accessor, const PackedPipelineState &packedState) {
        gpu.shader.ResetPools();

        using PipelineStage = engine::Pipeline::Shader::Type;
        auto pipelineStage{[](u32 i) { return static_cast<PipelineStage>(i); }};
        auto stageIdx{[](PipelineStage stage) { return static_cast<u8>(stage); }};

        std::array<Shader::IR::Program, engine::PipelineCount> programs;
        Shader::IR::Program *layerConversionSourceProgram{};
        bool ignoreVertexCullBeforeFetch{};

        for (u32 i{}; i < engine::PipelineCount; i++) {
            if (!packedState.shaderHashes[i]) {
                if (i == stageIdx(PipelineStage::Geometry) && layerConversionSourceProgram)
                    programs[i] = gpu.shader.GenerateGeometryPassthroughShader(*layerConversionSourceProgram, ConvertShaderOutputTopology(packedState.topology));

                continue;
            }

            auto binary{accessor.GetShaderBinary(i)};
            auto program{gpu.shader.ParseGraphicsShader(
                packedState.postVtgShaderAttributeSkipMask,
                ConvertCompilerShaderStage(static_cast<PipelineStage>(i)),
                binary.binary, binary.baseOffset,
                packedState.bindlessTextureConstantBufferSlotSelect,
                packedState.viewportTransformEnable,
                [&](u32 index, u32 offset) {
                    u32 shaderStage{i > 0 ? (i - 1) : 0};
                    return accessor.GetConstantBufferValue(shaderStage, index, offset);
                }, [&](u32 index) {
                    return accessor.GetTextureType(BindlessHandle{ .raw = index }.textureIndex);
                })};
            if (i == stageIdx(PipelineStage::Vertex) && packedState.shaderHashes[stageIdx(PipelineStage::VertexCullBeforeFetch)]) {
                ignoreVertexCullBeforeFetch = true;
                programs[i] = gpu.shader.CombineVertexShaders(programs[stageIdx(PipelineStage::VertexCullBeforeFetch)], program, binary.binary);
            } else {
                programs[i] = program;
            }

            if (programs[i].info.requires_layer_emulation)
                layerConversionSourceProgram = &programs[i];
        }

        bool hasGeometry{packedState.shaderHashes[stageIdx(PipelineStage::Geometry)] && !programs[stageIdx(PipelineStage::Geometry)].is_geometry_passthrough};
        Shader::Backend::Bindings bindings{};
        Shader::IR::Program *lastProgram{};

        std::array<Pipeline::ShaderStage, engine::ShaderStageCount> shaderStages{};

        for (u32 i{stageIdx(ignoreVertexCullBeforeFetch ? PipelineStage::Vertex : PipelineStage::VertexCullBeforeFetch)}; i < engine::PipelineCount; i++) {
            if (!packedState.shaderHashes[i] && !(i == stageIdx(PipelineStage::Geometry) && layerConversionSourceProgram))
                continue;

            auto runtimeInfo{MakeRuntimeInfo(packedState, programs[i], lastProgram, hasGeometry)};
            shaderStages[i - (i >= 1 ? 1 : 0)] = {ConvertVkShaderStage(pipelineStage(i)), gpu.shader.CompileShader(runtimeInfo, programs[i], bindings), programs[i].info};

            lastProgram = &programs[i];
        }

        return shaderStages;
    }

    static Pipeline::DescriptorInfo MakePipelineDescriptorInfo(const std::array<Pipeline::ShaderStage, engine::ShaderStageCount> &shaderStages, bool needsIndividualTextureBindingWrites) {
        Pipeline::DescriptorInfo descriptorInfo{};
        u32 bindingIndex{};

        for (size_t i{}; i < engine::ShaderStageCount; i++) {
            const auto &stage{shaderStages[i]};
            if (!stage.module)
                continue;

            auto &stageDescInfo{descriptorInfo.stages[i]};

            auto pushBindings{[&](vk::DescriptorType type, const auto &descs, u32 &count, auto &&descCb, bool individualDescWrites = false) {
                descriptorInfo.totalWriteDescCount += individualDescWrites ? descs.size() : ((descs.size() > 0) ? 1 : 0);

                for (u32 descIdx{}; descIdx < descs.size(); descIdx++) {
                    const auto &desc{descs[descIdx]};
                    count += desc.count;

                    descCb(desc, descIdx);

                    descriptorInfo.copyDescs.push_back(vk::CopyDescriptorSet{
                        .srcBinding = bindingIndex,
                        .srcArrayElement = 0,
                        .dstBinding = bindingIndex,
                        .dstArrayElement = 0,
                        .descriptorCount = desc.count,
                    });

                    descriptorInfo.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
                        .binding = bindingIndex++,
                        .descriptorType = type,
                        .descriptorCount = desc.count,
                        .stageFlags = stage.stage,
                    });
                }
            }};

            pushBindings(vk::DescriptorType::eUniformBuffer, stage.info.constant_buffer_descriptors, stageDescInfo.uniformBufferDescCount, [&](const Shader::ConstantBufferDescriptor &desc, u32 descIdx) {
                for (u32 cbufIdx{desc.index}; cbufIdx < desc.index + desc.count; cbufIdx++) {
                    auto &usage{stageDescInfo.cbufUsages[cbufIdx]};
                    usage.uniformBuffers.push_back({bindingIndex, descIdx});
                    usage.totalBufferDescCount += desc.count;
                    usage.writeDescCount++;
                }
            });
            pushBindings(vk::DescriptorType::eStorageBuffer, stage.info.storage_buffers_descriptors, stageDescInfo.storageBufferDescCount, [&](const Shader::StorageBufferDescriptor &desc, u32 descIdx) {
                auto &usage{stageDescInfo.cbufUsages[desc.cbuf_index]};
                usage.storageBuffers.push_back({bindingIndex, descIdx, descriptorInfo.totalStorageBufferCount});
                usage.totalBufferDescCount += desc.count;
                usage.writeDescCount++;
                descriptorInfo.totalStorageBufferCount += desc.count;
            });
            descriptorInfo.totalBufferDescCount += stageDescInfo.uniformBufferDescCount + stageDescInfo.storageBufferDescCount;

            pushBindings(vk::DescriptorType::eUniformTexelBuffer, stage.info.texture_buffer_descriptors, stageDescInfo.uniformTexelBufferDescCount, [](const auto &, u32) {
                Logger::Warn("Texture buffer descriptors are not supported");
            });
            pushBindings(vk::DescriptorType::eStorageTexelBuffer, stage.info.image_buffer_descriptors, stageDescInfo.storageTexelBufferDescCount, [](const auto &, u32) {
                Logger::Warn("Image buffer descriptors are not supported");
            });
            descriptorInfo.totalTexelBufferDescCount += stageDescInfo.uniformTexelBufferDescCount + stageDescInfo.storageTexelBufferDescCount;

            pushBindings(vk::DescriptorType::eCombinedImageSampler, stage.info.texture_descriptors, stageDescInfo.combinedImageSamplerDescCount, [&](const Shader::TextureDescriptor &desc, u32 descIdx) {
                auto addUsage{[&](auto idx) {
                    auto &usage{stageDescInfo.cbufUsages[idx]};
                    usage.combinedImageSamplers.push_back({bindingIndex, descIdx, descriptorInfo.totalCombinedImageSamplerCount});
                    usage.totalImageDescCount += desc.count;
                    usage.writeDescCount++;
                }};

                addUsage(desc.cbuf_index);
                if (desc.has_secondary)
                    addUsage(desc.secondary_cbuf_index);

                descriptorInfo.totalCombinedImageSamplerCount += desc.count;
            }, needsIndividualTextureBindingWrites);
            pushBindings(vk::DescriptorType::eStorageImage, stage.info.image_descriptors, stageDescInfo.storageImageDescCount, [](const auto &, u32) {
                Logger::Warn("Image descriptors are not supported");
            });
            descriptorInfo.totalImageDescCount += stageDescInfo.combinedImageSamplerDescCount + stageDescInfo.storageImageDescCount;
        }
        return descriptorInfo;
    }

    static vk::Format ConvertVertexInputAttributeFormat(engine::VertexAttribute::ComponentBitWidths componentBitWidths, engine::VertexAttribute::NumericalType numericalType) {
        #define FORMAT_CASE(bitWidths, type, vkType, vkFormat, ...) \
            case engine::VertexAttribute::ComponentBitWidths::bitWidths | engine::VertexAttribute::NumericalType::type: \
                return vk::Format::vkFormat ## vkType ##__VA_ARGS__

        #define FORMAT_INT_CASE(size, vkFormat, ...) \
            FORMAT_CASE(size, Uint, Uint, vkFormat, ##__VA_ARGS__); \
            FORMAT_CASE(size, Sint, Sint, vkFormat, ##__VA_ARGS__);

        #define FORMAT_INT_FLOAT_CASE(size, vkFormat, ...) \
            FORMAT_INT_CASE(size, vkFormat, ##__VA_ARGS__); \
            FORMAT_CASE(size, Float, Sfloat, vkFormat, ##__VA_ARGS__);

        #define FORMAT_NORM_INT_SCALED_CASE(size, vkFormat, ...) \
            FORMAT_INT_CASE(size, vkFormat, ##__VA_ARGS__);               \
            FORMAT_CASE(size, Unorm, Unorm, vkFormat, ##__VA_ARGS__);     \
            FORMAT_CASE(size, Snorm, Snorm, vkFormat, ##__VA_ARGS__);     \
            FORMAT_CASE(size, Uscaled, Uscaled, vkFormat, ##__VA_ARGS__); \
            FORMAT_CASE(size, Sscaled, Sscaled, vkFormat, ##__VA_ARGS__)

        #define FORMAT_NORM_INT_SCALED_FLOAT_CASE(size, vkFormat) \
            FORMAT_NORM_INT_SCALED_CASE(size, vkFormat); \
            FORMAT_CASE(size, Float, Sfloat, vkFormat)

        // No mobile support scaled formats, so pass as int and the shader compiler will convert to float for us
        if (numericalType == engine::VertexAttribute::NumericalType::Sscaled)
            numericalType = engine::VertexAttribute::NumericalType::Sint;
        else if (numericalType == engine::VertexAttribute::NumericalType::Uscaled)
            numericalType = engine::VertexAttribute::NumericalType::Uint;

        switch (componentBitWidths | numericalType) {
            /* 8-bit components */
            FORMAT_NORM_INT_SCALED_CASE(R8, eR8);
            FORMAT_NORM_INT_SCALED_CASE(R8_G8, eR8G8);
            FORMAT_NORM_INT_SCALED_CASE(G8R8, eR8G8);
            FORMAT_NORM_INT_SCALED_CASE(R8_G8_B8, eR8G8B8);
            FORMAT_NORM_INT_SCALED_CASE(R8_G8_B8_A8, eR8G8B8A8);
            FORMAT_NORM_INT_SCALED_CASE(A8B8G8R8, eR8G8B8A8);
            FORMAT_NORM_INT_SCALED_CASE(X8B8G8R8, eR8G8B8A8);

                /* 16-bit components */
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16, eR16);
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16_G16, eR16G16);
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16_G16_B16, eR16G16B16);
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16_G16_B16_A16, eR16G16B16A16);

                /* 32-bit components */
            FORMAT_INT_FLOAT_CASE(R32, eR32);
            FORMAT_INT_FLOAT_CASE(R32_G32, eR32G32);
            FORMAT_INT_FLOAT_CASE(R32_G32_B32, eR32G32B32);
            FORMAT_INT_FLOAT_CASE(R32_G32_B32_A32, eR32G32B32A32);

                /* 10-bit RGB, 2-bit A */
            FORMAT_NORM_INT_SCALED_CASE(A2B10G10R10, eA2B10G10R10, Pack32);

                /* 11-bit G and R, 10-bit B */
            FORMAT_CASE(B10G11R11, Float, Ufloat, eB10G11R11, Pack32);

            default:
                Logger::Warn("Unimplemented Maxwell3D Vertex Buffer Format: {} | {}", static_cast<u8>(componentBitWidths), static_cast<u8>(numericalType));
                return vk::Format::eR8G8B8A8Unorm;
        }

        #undef FORMAT_CASE
        #undef FORMAT_INT_CASE
        #undef FORMAT_INT_FLOAT_CASE
        #undef FORMAT_NORM_INT_SCALED_CASE
        #undef FORMAT_NORM_INT_SCALED_FLOAT_CASE
    }

    static vk::PrimitiveTopology ConvertPrimitiveTopology(engine::DrawTopology topology) {
        switch (topology) {
            case engine::DrawTopology::Points:
                return vk::PrimitiveTopology::ePointList;
            case engine::DrawTopology::Lines:
                return vk::PrimitiveTopology::eLineList;
            case engine::DrawTopology::LineStrip:
                return vk::PrimitiveTopology::eLineStrip;
            case engine::DrawTopology::Triangles:
                return vk::PrimitiveTopology::eTriangleList;
            case engine::DrawTopology::TriangleStrip:
                return vk::PrimitiveTopology::eTriangleStrip;
            case engine::DrawTopology::TriangleFan:
                return vk::PrimitiveTopology::eTriangleFan;
            case engine::DrawTopology::Quads:
                return vk::PrimitiveTopology::eTriangleList; // Uses quad conversion
            case engine::DrawTopology::LineListAdjcy:
                return vk::PrimitiveTopology::eLineListWithAdjacency;
            case engine::DrawTopology::LineStripAdjcy:
                return vk::PrimitiveTopology::eLineStripWithAdjacency;
            case engine::DrawTopology::TriangleListAdjcy:
                return vk::PrimitiveTopology::eTriangleListWithAdjacency;
            case engine::DrawTopology::TriangleStripAdjcy:
                return vk::PrimitiveTopology::eTriangleStripWithAdjacency;
            case engine::DrawTopology::Patch:
                return vk::PrimitiveTopology::ePatchList;
            default:
                Logger::Warn("Unimplemented input assembly topology: {}", static_cast<u8>(topology));
                return vk::PrimitiveTopology::eTriangleList;
        }
    }

    static vk::ProvokingVertexModeEXT ConvertProvokingVertex(engine::ProvokingVertex::Value provokingVertex) {
        switch (provokingVertex) {
            case engine::ProvokingVertex::Value::First:
                return vk::ProvokingVertexModeEXT::eFirstVertex;
            case engine::ProvokingVertex::Value::Last:
                return vk::ProvokingVertexModeEXT::eLastVertex;
        }
    }

    static cache::GraphicsPipelineCache::CompiledPipeline MakeCompiledPipeline(GPU &gpu,
                                                                               const PackedPipelineState &packedState,
                                                                               const std::array<Pipeline::ShaderStage, engine::ShaderStageCount> &shaderStages,
                                                                               span<vk::DescriptorSetLayoutBinding> layoutBindings) {
        boost::container::static_vector<vk::PipelineShaderStageCreateInfo, engine::ShaderStageCount> shaderStageInfos;
        for (const auto &stage : shaderStages)
            if (stage.module)
                shaderStageInfos.push_back(vk::PipelineShaderStageCreateInfo{
                    .stage = stage.stage,
                    .module = &*stage.module,
                    .pName = "main"
                });

        boost::container::static_vector<vk::VertexInputBindingDescription, engine::VertexStreamCount> bindingDescs;
        boost::container::static_vector<vk::VertexInputBindingDivisorDescriptionEXT, engine::VertexStreamCount> bindingDivisorDescs;
        boost::container::static_vector<vk::VertexInputAttributeDescription, engine::VertexAttributeCount> attributeDescs;

        for (u32 i{}; i < engine::VertexStreamCount; i++) {
            const auto &binding{packedState.vertexBindings[i]};
            bindingDescs.push_back({
                                       .binding = i,
                                       .stride = packedState.vertexStrides[i],
                                       .inputRate = binding.GetInputRate(),
                                   });

            if (binding.GetInputRate() == vk::VertexInputRate::eInstance) {
                if (!gpu.traits.supportsVertexAttributeDivisor)
                    [[unlikely]]
                        Logger::Warn("Vertex attribute divisor used on guest without host support");
                else if (!gpu.traits.supportsVertexAttributeZeroDivisor && binding.divisor == 0)
                    [[unlikely]]
                        Logger::Warn("Vertex attribute zero divisor used on guest without host support");
                else
                    bindingDivisorDescs.push_back({
                                                      .binding = i,
                                                      .divisor = binding.divisor,
                                                  });
            }
        }

        for (u32 i{}; i < engine::VertexAttributeCount; i++) {
            const auto &attribute{packedState.vertexAttributes[i]};
            if (attribute.source == engine::VertexAttribute::Source::Active && shaderStages[0].info.loads.Generic(i))
                attributeDescs.push_back({
                                             .location = i,
                                             .binding = attribute.stream,
                                             .format = ConvertVertexInputAttributeFormat(attribute.componentBitWidths, attribute.numericalType),
                                             .offset = attribute.offset,
                                         });
        }

        vk::StructureChain<vk::PipelineVertexInputStateCreateInfo, vk::PipelineVertexInputDivisorStateCreateInfoEXT> vertexInputState{
            vk::PipelineVertexInputStateCreateInfo{
                .vertexBindingDescriptionCount = static_cast<u32>(bindingDescs.size()),
                .pVertexBindingDescriptions = bindingDescs.data(),
                .vertexAttributeDescriptionCount = static_cast<u32>(attributeDescs.size()),
                .pVertexAttributeDescriptions = attributeDescs.data(),
            },
            vk::PipelineVertexInputDivisorStateCreateInfoEXT{
                .vertexBindingDivisorCount = static_cast<u32>(bindingDivisorDescs.size()),
                .pVertexBindingDivisors = bindingDivisorDescs.data(),
            },
        };

        if (bindingDivisorDescs.empty())
            vertexInputState.unlink<vk::PipelineVertexInputDivisorStateCreateInfoEXT>();

        vk::PipelineInputAssemblyStateCreateInfo inputAssemblyState{
            .topology = ConvertPrimitiveTopology(packedState.topology),
            .primitiveRestartEnable = packedState.primitiveRestartEnabled,
        };

        vk::PipelineTessellationStateCreateInfo tessellationState{
            .patchControlPoints = packedState.patchSize,
        };

        vk::StructureChain<vk::PipelineRasterizationStateCreateInfo, vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT> rasterizationState{};

        auto &rasterizationCreateInfo{rasterizationState.get<vk::PipelineRasterizationStateCreateInfo>()};
        rasterizationCreateInfo.rasterizerDiscardEnable = packedState.rasterizerDiscardEnable;
        rasterizationCreateInfo.polygonMode = packedState.GetPolygonMode();
        rasterizationCreateInfo.cullMode = vk::CullModeFlags{packedState.cullMode};
        rasterizationCreateInfo.frontFace = packedState.frontFaceClockwise ? vk::FrontFace::eClockwise : vk::FrontFace::eCounterClockwise;
        rasterizationCreateInfo.depthBiasEnable = packedState.depthBiasEnable;
        rasterizationCreateInfo.depthClampEnable = packedState.depthClampEnable;
        if (!gpu.traits.supportsDepthClamp)
            Logger::Warn("Depth clamp used on guest without host support");
        rasterizationState.get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>().provokingVertexMode = ConvertProvokingVertex(packedState.provokingVertex);

        constexpr vk::PipelineMultisampleStateCreateInfo multisampleState{
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
        };

        vk::PipelineDepthStencilStateCreateInfo depthStencilState{
            .depthTestEnable = packedState.depthTestEnable,
            .depthWriteEnable = packedState.depthWriteEnable,
            .depthCompareOp = packedState.GetDepthFunc(),
            .depthBoundsTestEnable = packedState.depthBoundsTestEnable,
            .stencilTestEnable = packedState.stencilTestEnable
        };

        std::tie(depthStencilState.front, depthStencilState.back) = packedState.GetStencilOpsState();

        boost::container::static_vector<vk::PipelineColorBlendAttachmentState, engine::ColorTargetCount> attachmentBlendStates;
        boost::container::static_vector<vk::Format, engine::ColorTargetCount> colorAttachmentFormats;

        for (u32 i{}; i < engine::ColorTargetCount; i++) {
            if (i < packedState.GetColorRenderTargetCount()) {
                attachmentBlendStates.push_back(packedState.GetAttachmentBlendState(i));
                texture::Format format{packedState.GetColorRenderTargetFormat(packedState.ctSelect[i])};
                colorAttachmentFormats.push_back(format ? format->vkFormat : vk::Format::eUndefined);
            } else {
                colorAttachmentFormats.push_back(vk::Format::eUndefined);
            }
        }

        vk::PipelineColorBlendStateCreateInfo colorBlendState{
            .logicOpEnable = packedState.logicOpEnable,
            .logicOp = packedState.GetLogicOp(),
            .attachmentCount = static_cast<u32>(attachmentBlendStates.size()),
            .pAttachments = attachmentBlendStates.data()
        };


        static constexpr u32 BaseDynamicStateCount{9};
        static constexpr u32 ExtendedDynamicStateCount{BaseDynamicStateCount + 1};

        constexpr std::array<vk::DynamicState, ExtendedDynamicStateCount> dynamicStates{
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
            vk::DynamicState::eLineWidth,
            vk::DynamicState::eDepthBias,
            vk::DynamicState::eBlendConstants,
            vk::DynamicState::eDepthBounds,
            vk::DynamicState::eStencilCompareMask,
            vk::DynamicState::eStencilWriteMask,
            vk::DynamicState::eStencilReference,
            // VK_EXT_dynamic_state starts here
            vk::DynamicState::eVertexInputBindingStrideEXT
        };

        vk::PipelineDynamicStateCreateInfo dynamicState{
            .dynamicStateCount = gpu.traits.supportsExtendedDynamicState ? ExtendedDynamicStateCount : BaseDynamicStateCount,
            .pDynamicStates = dynamicStates.data()
        };

        // Dynamic state will be used instead of these
        std::array<vk::Rect2D, engine::ViewportCount> emptyScissors{};
        std::array<vk::Viewport, engine::ViewportCount> emptyViewports{};

        vk::PipelineViewportStateCreateInfo viewportState{
            .viewportCount = static_cast<u32>(gpu.traits.supportsMultipleViewports ? engine::ViewportCount : 1),
            .pViewports = emptyViewports.data(),
            .scissorCount = static_cast<u32>(gpu.traits.supportsMultipleViewports ? engine::ViewportCount : 1),
            .pScissors = emptyScissors.data(),
        };

        texture::Format depthStencilFormat{packedState.GetDepthRenderTargetFormat()};

        return gpu.graphicsPipelineCache.GetCompiledPipeline(cache::GraphicsPipelineCache::PipelineState{
            .shaderStages = shaderStageInfos,
            .vertexState = vertexInputState,
            .inputAssemblyState = inputAssemblyState,
            .tessellationState = tessellationState,
            .viewportState = viewportState,
            .rasterizationState = rasterizationState,
            .multisampleState = multisampleState,
            .depthStencilState = depthStencilState,
            .colorBlendState = colorBlendState,
            .dynamicState = dynamicState,
            .colorFormats = colorAttachmentFormats,
            .depthStencilFormat = depthStencilFormat ? depthStencilFormat->vkFormat : vk::Format::eUndefined,
            .sampleCount = vk::SampleCountFlagBits::e1, //TODO: fix after MSAA support
        }, layoutBindings);
    }

    Pipeline::Pipeline(GPU &gpu, PipelineStateAccessor &accessor, const PackedPipelineState &packedState)
        : sourcePackedState{packedState},
          shaderStages{MakePipelineShaders(gpu, accessor, sourcePackedState)},
          descriptorInfo{MakePipelineDescriptorInfo(shaderStages, gpu.traits.quirks.needsIndividualTextureBindingWrites)},
          compiledPipeline{MakeCompiledPipeline(gpu, sourcePackedState, shaderStages, descriptorInfo.descriptorSetLayoutBindings)} {
        storageBufferViews.resize(descriptorInfo.totalStorageBufferCount);
        accessor.MarkComplete();
    }

    void Pipeline::SyncCachedStorageBufferViews(ContextTag executionTag) {
        if (lastExecutionTag != executionTag) {
            for (auto &view : storageBufferViews)
                view.PurgeCaches();

            lastExecutionTag = executionTag;
        }
    }

    Pipeline *Pipeline::LookupNext(const PackedPipelineState &packedState) {
        auto it{std::find_if(transitionCache.begin(), transitionCache.end(), [&packedState](auto pipeline) {
            if (pipeline && pipeline->sourcePackedState == packedState)
                return true;
            else
                return false;
        })};

        if (it != transitionCache.end()) {
            std::swap(*it, *transitionCache.begin());
            return *transitionCache.begin();
        }

        return nullptr;
    }

    void Pipeline::AddTransition(Pipeline *next) {
        transitionCache[transitionCacheNextIdx] = next;
        transitionCacheNextIdx = (transitionCacheNextIdx + 1) % transitionCache.size();
    }

    bool Pipeline::CheckBindingMatch(Pipeline *other) {
        if (auto it{bindingMatchCache.find(other)}; it != bindingMatchCache.end())
            return it->second;

        for (size_t i{}; i < shaderStages.size(); i++) {
            if (!shaderStages[i].BindingsEqual(other->shaderStages[i])) {
                bindingMatchCache[other] = false;
                return false;
            }
        }

        bindingMatchCache[other] = true;
        return true;
    }

    u32 Pipeline::GetTotalSampledImageCount() const {
        return descriptorInfo.totalCombinedImageSamplerCount;
    }

    DescriptorUpdateInfo *Pipeline::SyncDescriptors(InterconnectContext &ctx, ConstantBufferSet &constantBuffers, Samplers &samplers, Textures &textures, span<TextureView *> sampledImages) {
        SyncCachedStorageBufferViews(ctx.executor.executionTag);

        u32 writeIdx{};
        auto writes{ctx.executor.allocator->AllocateUntracked<vk::WriteDescriptorSet>(descriptorInfo.totalWriteDescCount)};

        u32 bufferIdx{};
        auto bufferDescs{ctx.executor.allocator->AllocateUntracked<vk::DescriptorBufferInfo>(descriptorInfo.totalBufferDescCount)};
        auto bufferDescDynamicBindings{ctx.executor.allocator->AllocateUntracked<DynamicBufferBinding>(descriptorInfo.totalBufferDescCount)};
        u32 imageIdx{};
        auto imageDescs{ctx.executor.allocator->AllocateUntracked<vk::DescriptorImageInfo>(descriptorInfo.totalImageDescCount)};

        u32 storageBufferIdx{}; // Need to keep track of this to index into the cached view array
        u32 combinedImageSamplerIdx{}; // Need to keep track of this to index into the sampled image array
        u32 bindingIdx{};

        /**
         * @brief Adds descriptor writes for a single Vulkan descriptor type that uses buffer descriptors
         * @param count Total number of descriptors to write, including array elements
         */
        auto writeBufferDescs{[&](vk::DescriptorType type, const auto &descs, u32 count, auto getBufferCb) {
            if (!descs.empty()) {
                writes[writeIdx++] = {
                    .dstBinding = bindingIdx,
                    .descriptorCount = count,
                    .descriptorType = type,
                    .pBufferInfo = &bufferDescs[bufferIdx],
                };

                bindingIdx += descs.size();

                // The underlying buffer bindings will be resolved from the dynamic ones during recording
                for (const auto &desc : descs)
                    for (u32 arrayIdx{}; arrayIdx < desc.count; arrayIdx++)
                        bufferDescDynamicBindings[bufferIdx++] = getBufferCb(desc, arrayIdx);
            }
        }};

        auto writeImageDescs{[&](vk::DescriptorType type, const auto &descs, u32 count, auto getTextureCb, bool needsIndividualTextureBindingWrites) {
            if (!descs.empty()) {
                if (!needsIndividualTextureBindingWrites) {
                    writes[writeIdx++] = {
                        .dstBinding = bindingIdx,
                        .descriptorCount = count,
                        .descriptorType = type,
                        .pImageInfo = &imageDescs[imageIdx],
                    };

                    bindingIdx += descs.size();
                }

                for (const auto &desc : descs) {
                    if (needsIndividualTextureBindingWrites) {
                        writes[writeIdx++] = {
                            .dstBinding = bindingIdx++,
                            .descriptorCount = desc.count,
                            .descriptorType = type,
                            .pImageInfo = &imageDescs[imageIdx],
                        };
                    }

                    for (u32 arrayIdx{}; arrayIdx < desc.count; arrayIdx++)
                        imageDescs[imageIdx++] = getTextureCb(desc, arrayIdx);
                }
            }
        }};

        for (size_t i{}; i < shaderStages.size(); i++) {
            const auto &stage{shaderStages[i]};
            if (!stage.module)
                continue;

            const auto &stageDescInfo{descriptorInfo.stages[i]};

            writeBufferDescs(vk::DescriptorType::eUniformBuffer, stage.info.constant_buffer_descriptors, stageDescInfo.uniformBufferDescCount,
                             [&](const Shader::ConstantBufferDescriptor &desc, size_t arrayIdx) {
                                 size_t cbufIdx{desc.index + arrayIdx};
                                 return GetConstantBufferBinding(ctx, stage.info, constantBuffers[i][cbufIdx].view, cbufIdx);
                             });

            writeBufferDescs(vk::DescriptorType::eStorageBuffer, stage.info.storage_buffers_descriptors, stageDescInfo.storageBufferDescCount,
                             [&](const Shader::StorageBufferDescriptor &desc, size_t arrayIdx) {
                                 return GetStorageBufferBinding(ctx, desc, constantBuffers[i][desc.cbuf_index], storageBufferViews[storageBufferIdx++]);
                             });

            bindingIdx += stageDescInfo.uniformTexelBufferDescCount;
            bindingIdx += stageDescInfo.storageTexelBufferDescCount;

            writeImageDescs(vk::DescriptorType::eCombinedImageSampler, stage.info.texture_descriptors, stageDescInfo.combinedImageSamplerDescCount,
                            [&](const Shader::TextureDescriptor &desc, size_t arrayIdx) {
                                BindlessHandle handle{ReadBindlessHandle(ctx, constantBuffers[i], desc, arrayIdx)};
                                auto binding{GetTextureBinding(ctx, desc, samplers, textures, handle)};
                                sampledImages[combinedImageSamplerIdx++] = binding.second;
                                return binding.first;
                            }, ctx.gpu.traits.quirks.needsIndividualTextureBindingWrites);

            bindingIdx += stageDescInfo.storageImageDescCount;
        }

        // Since we don't implement all descriptor types the number of writes might not match what's expected
        if (!writeIdx)
            return nullptr;

        return ctx.executor.allocator->EmplaceUntracked<DescriptorUpdateInfo>(DescriptorUpdateInfo{
            .writes = writes.first(writeIdx),
            .bufferDescs = bufferDescs.first(bufferIdx),
            .bufferDescDynamicBindings = bufferDescDynamicBindings.first(bufferIdx),
            .pipelineLayout = compiledPipeline.pipelineLayout,
            .descriptorSetLayout = compiledPipeline.descriptorSetLayout,
            .bindPoint = vk::PipelineBindPoint::eGraphics,
            .descriptorSetIndex = 0,
        });
    }

    DescriptorUpdateInfo *Pipeline::SyncDescriptorsQuickBind(InterconnectContext &ctx, ConstantBufferSet &constantBuffers, Samplers &samplers, Textures &textures, ConstantBuffers::QuickBind quickBind, span<TextureView *> sampledImages) {
        SyncCachedStorageBufferViews(ctx.executor.executionTag);

        size_t stageIndex{static_cast<size_t>(quickBind.stage)};
        const auto &stageDescInfo{descriptorInfo.stages[stageIndex]};
        const auto &cbufUsageInfo{stageDescInfo.cbufUsages[quickBind.index]};
        if (!cbufUsageInfo.writeDescCount)
            return nullptr;

        const auto &shaderInfo{shaderStages[stageIndex].info};
        auto &stageConstantBuffers{constantBuffers[stageIndex]};

        u32 writeIdx{};
        auto writes{ctx.executor.allocator->AllocateUntracked<vk::WriteDescriptorSet>(cbufUsageInfo.writeDescCount)};

        u32 bufferIdx{};
        auto bufferDescs{ctx.executor.allocator->AllocateUntracked<vk::DescriptorBufferInfo>(cbufUsageInfo.totalBufferDescCount)};
        auto bufferDescDynamicBindings{ctx.executor.allocator->AllocateUntracked<DynamicBufferBinding>(cbufUsageInfo.totalBufferDescCount)};

        u32 imageIdx{};
        auto imageDescs{ctx.executor.allocator->AllocateUntracked<vk::DescriptorImageInfo>(cbufUsageInfo.totalImageDescCount)};

        /**
         * @brief Unified function to add descriptor set writes for any descriptor type
         * @note Since quick bind always results in one write per buffer, `needsIndividualTextureBindingWrites` is implicit
         */
        auto writeDescs{[&]<bool ImageDesc, bool BufferDesc>(vk::DescriptorType type, const auto &usages, const auto &descs, auto getBindingCb) {
            for (const auto &usage : usages) {
                const auto &shaderDesc{descs[usage.shaderDescIdx]};

                writes[writeIdx] = {
                    .dstBinding = usage.binding,
                    .descriptorCount = shaderDesc.count,
                    .descriptorType = type,
                };

                if constexpr (ImageDesc)
                    writes[writeIdx].pImageInfo = &imageDescs[imageIdx];
                else if constexpr (BufferDesc)
                    writes[writeIdx].pBufferInfo = &bufferDescs[bufferIdx];

                writeIdx++;

                for (size_t i{}; i < shaderDesc.count; i++) {
                    if constexpr (ImageDesc)
                        imageDescs[imageIdx++] = getBindingCb(usage, shaderDesc, i);
                    else if constexpr (BufferDesc)
                        bufferDescDynamicBindings[bufferIdx++] = getBindingCb(usage, shaderDesc, i);
                }
            }
        }};

        writeDescs.operator()<false, true>(vk::DescriptorType::eUniformBuffer, cbufUsageInfo.uniformBuffers, shaderInfo.constant_buffer_descriptors,
                                           [&](auto usage, const Shader::ConstantBufferDescriptor &desc, size_t arrayIdx) -> DynamicBufferBinding {
                                               size_t cbufIdx{desc.index + arrayIdx};
                                               return GetConstantBufferBinding(ctx, shaderInfo, stageConstantBuffers[cbufIdx].view, cbufIdx);
                                           });

        writeDescs.operator()<false, true>(vk::DescriptorType::eStorageBuffer, cbufUsageInfo.storageBuffers, shaderInfo.storage_buffers_descriptors,
                                           [&](auto usage, const Shader::StorageBufferDescriptor &desc, size_t arrayIdx) {
                                               return GetStorageBufferBinding(ctx, desc, stageConstantBuffers[desc.cbuf_index], storageBufferViews[usage.entirePipelineIdx + arrayIdx]);
                                           });

        writeDescs.operator()<true, false>(vk::DescriptorType::eCombinedImageSampler, cbufUsageInfo.combinedImageSamplers, shaderInfo.texture_descriptors,
                                           [&](auto usage, const Shader::TextureDescriptor &desc, size_t arrayIdx) {
                                               BindlessHandle handle{ReadBindlessHandle(ctx, stageConstantBuffers, desc, arrayIdx)};
                                               auto binding{GetTextureBinding(ctx, desc, samplers, textures, handle)};
                                               sampledImages[usage.entirePipelineIdx + arrayIdx] = binding.second;
                                               return binding.first;
                                           });

        // Since we don't implement all descriptor types the number of writes might not match what's expected
        if (!writeIdx)
            return nullptr;

        return ctx.executor.allocator->EmplaceUntracked<DescriptorUpdateInfo>(DescriptorUpdateInfo{
            .copies = descriptorInfo.copyDescs,
            .writes = writes.first(writeIdx),
            .bufferDescs = bufferDescs.first(bufferIdx),
            .bufferDescDynamicBindings = bufferDescDynamicBindings.first(bufferIdx),
            .pipelineLayout = compiledPipeline.pipelineLayout,
            .descriptorSetLayout = compiledPipeline.descriptorSetLayout,
            .bindPoint = vk::PipelineBindPoint::eGraphics,
            .descriptorSetIndex = 0,
        });
    }

    PipelineManager::PipelineManager(GPU &gpu) {
        std::ifstream stream{gpu.graphicsPipelineCacheManager->OpenReadStream()};
        i64 lastKnownGoodOffset{stream.tellg()};
        try {
            auto startTime{util::GetTimeNs()};
            PipelineStateBundle bundle;

            while (bundle.Deserialise(stream)) {
                lastKnownGoodOffset = stream.tellg();
                auto accessor{FilePipelineStateAccessor{bundle}};
                map.emplace(bundle.GetKey<PackedPipelineState>(), std::make_unique<Pipeline>(gpu, accessor, bundle.GetKey<PackedPipelineState>()));
            }

            Logger::Info("Loaded {} graphics pipelines in {}ms", map.size(), (util::GetTimeNs() - startTime) / constant::NsInMillisecond);
        } catch (const exception &e) {
            Logger::Warn("Pipeline cache corrupted at: 0x{:X}, error: {}", lastKnownGoodOffset, e.what());
            gpu.graphicsPipelineCacheManager->InvalidateAllAfter(static_cast<u64>(lastKnownGoodOffset));
            return;
        }
    }

    Pipeline *PipelineManager::FindOrCreate(InterconnectContext &ctx, Textures &textures, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const std::array<ShaderBinary, engine::PipelineCount> &shaderBinaries) {
        auto it{map.find(packedState)};
        if (it != map.end())
            return it->second.get();

        auto bundle{std::make_unique<PipelineStateBundle>()};
        bundle->Reset(packedState);
        auto accessor{RuntimeGraphicsPipelineStateAccessor{std::move(bundle), ctx, textures, constantBuffers, shaderBinaries}};
        return map.emplace(packedState, std::make_unique<Pipeline>(ctx.gpu, accessor, packedState)).first->second.get();
    }
}

