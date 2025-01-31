//
// Copyright (c) 2015 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// StateManager11.cpp: Defines a class for caching D3D11 state

#include "libANGLE/renderer/d3d/d3d11/StateManager11.h"

#include "common/bitset_utils.h"
#include "common/mathutil.h"
#include "common/utilities.h"
#include "libANGLE/Context.h"
#include "libANGLE/Query.h"
#include "libANGLE/VertexArray.h"
#include "libANGLE/renderer/d3d/TextureD3D.h"
#include "libANGLE/renderer/d3d/d3d11/Buffer11.h"
#include "libANGLE/renderer/d3d/d3d11/Context11.h"
#include "libANGLE/renderer/d3d/d3d11/Framebuffer11.h"
#include "libANGLE/renderer/d3d/d3d11/IndexBuffer11.h"
#include "libANGLE/renderer/d3d/d3d11/RenderTarget11.h"
#include "libANGLE/renderer/d3d/d3d11/Renderer11.h"
#include "libANGLE/renderer/d3d/d3d11/ShaderExecutable11.h"
#include "libANGLE/renderer/d3d/d3d11/TextureStorage11.h"
#include "libANGLE/renderer/d3d/d3d11/TransformFeedback11.h"
#include "libANGLE/renderer/d3d/d3d11/VertexArray11.h"
#include "libANGLE/renderer/d3d/d3d11/VertexBuffer11.h"

namespace rx
{

namespace
{
bool ImageIndexConflictsWithSRV(const gl::ImageIndex &index, D3D11_SHADER_RESOURCE_VIEW_DESC desc)
{
    unsigned mipLevel  = index.mipIndex;
    gl::TextureType textureType = index.type;

    switch (desc.ViewDimension)
    {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
        {
            bool allLevels         = (desc.Texture2D.MipLevels == std::numeric_limits<UINT>::max());
            unsigned int maxSrvMip = desc.Texture2D.MipLevels + desc.Texture2D.MostDetailedMip;
            maxSrvMip              = allLevels ? INT_MAX : maxSrvMip;

            unsigned mipMin = index.mipIndex;
            unsigned mipMax = INT_MAX;

            return textureType == gl::TextureType::_2D &&
                   gl::RangeUI(mipMin, mipMax)
                       .intersects(gl::RangeUI(desc.Texture2D.MostDetailedMip, maxSrvMip));
        }

        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        {
            GLint layerIndex = index.layerIndex;

            bool allLevels = (desc.Texture2DArray.MipLevels == std::numeric_limits<UINT>::max());
            unsigned int maxSrvMip =
                desc.Texture2DArray.MipLevels + desc.Texture2DArray.MostDetailedMip;
            maxSrvMip = allLevels ? INT_MAX : maxSrvMip;

            unsigned maxSlice = desc.Texture2DArray.FirstArraySlice + desc.Texture2DArray.ArraySize;

            // Cube maps can be mapped to Texture2DArray SRVs
            return (textureType == gl::TextureType::_2DArray ||
                    textureType == gl::TextureType::CubeMap) &&
                   desc.Texture2DArray.MostDetailedMip <= mipLevel && mipLevel < maxSrvMip &&
                   desc.Texture2DArray.FirstArraySlice <= static_cast<UINT>(layerIndex) &&
                   static_cast<UINT>(layerIndex) < maxSlice;
        }

        case D3D11_SRV_DIMENSION_TEXTURECUBE:
        {
            bool allLevels = (desc.TextureCube.MipLevels == std::numeric_limits<UINT>::max());
            unsigned int maxSrvMip = desc.TextureCube.MipLevels + desc.TextureCube.MostDetailedMip;
            maxSrvMip              = allLevels ? INT_MAX : maxSrvMip;

            return textureType == gl::TextureType::CubeMap &&
                   desc.TextureCube.MostDetailedMip <= mipLevel && mipLevel < maxSrvMip;
        }

        case D3D11_SRV_DIMENSION_TEXTURE3D:
        {
            bool allLevels         = (desc.Texture3D.MipLevels == std::numeric_limits<UINT>::max());
            unsigned int maxSrvMip = desc.Texture3D.MipLevels + desc.Texture3D.MostDetailedMip;
            maxSrvMip              = allLevels ? INT_MAX : maxSrvMip;

            return textureType == gl::TextureType::_3D &&
                   desc.Texture3D.MostDetailedMip <= mipLevel && mipLevel < maxSrvMip;
        }
        default:
            // We only handle the cases corresponding to valid image indexes
            UNIMPLEMENTED();
    }

    return false;
}

// Does *not* increment the resource ref count!!
ID3D11Resource *GetViewResource(ID3D11View *view)
{
    ID3D11Resource *resource = nullptr;
    ASSERT(view);
    view->GetResource(&resource);
    resource->Release();
    return resource;
}

int GetWrapBits(GLenum wrap)
{
    switch (wrap)
    {
        case GL_CLAMP_TO_EDGE:
            return 0x1;
        case GL_REPEAT:
            return 0x2;
        case GL_MIRRORED_REPEAT:
            return 0x3;
        default:
            UNREACHABLE();
            return 0;
    }
}

Optional<size_t> FindFirstNonInstanced(
    const std::vector<const TranslatedAttribute *> &currentAttributes)
{
    for (size_t index = 0; index < currentAttributes.size(); ++index)
    {
        if (currentAttributes[index]->divisor == 0)
        {
            return Optional<size_t>(index);
        }
    }

    return Optional<size_t>::Invalid();
}

void SortAttributesByLayout(const gl::Program *program,
                            const std::vector<TranslatedAttribute> &vertexArrayAttribs,
                            const std::vector<TranslatedAttribute> &currentValueAttribs,
                            AttribIndexArray *sortedD3DSemanticsOut,
                            std::vector<const TranslatedAttribute *> *sortedAttributesOut)
{
    sortedAttributesOut->clear();

    const auto &locationToSemantic =
        GetImplAs<ProgramD3D>(program)->getAttribLocationToD3DSemantics();

    for (auto locationIndex : program->getActiveAttribLocationsMask())
    {
        int d3dSemantic = locationToSemantic[locationIndex];
        if (sortedAttributesOut->size() <= static_cast<size_t>(d3dSemantic))
        {
            sortedAttributesOut->resize(d3dSemantic + 1);
        }

        (*sortedD3DSemanticsOut)[d3dSemantic] = d3dSemantic;

        const auto *arrayAttrib = &vertexArrayAttribs[locationIndex];
        if (arrayAttrib->attribute && arrayAttrib->attribute->enabled)
        {
            (*sortedAttributesOut)[d3dSemantic] = arrayAttrib;
        }
        else
        {
            ASSERT(currentValueAttribs[locationIndex].attribute);
            (*sortedAttributesOut)[d3dSemantic] = &currentValueAttribs[locationIndex];
        }
    }
}

void UpdateUniformBuffer(ID3D11DeviceContext *deviceContext,
                         UniformStorage11 *storage,
                         const d3d11::Buffer *buffer)
{
    deviceContext->UpdateSubresource(buffer->get(), 0, nullptr, storage->getDataPointer(0, 0), 0,
                                     0);
}

size_t GetReservedBufferCount(bool usesPointSpriteEmulation)
{
    return usesPointSpriteEmulation ? 1 : 0;
}

bool CullsEverything(const gl::State &glState)
{
    return (glState.getRasterizerState().cullFace &&
            glState.getRasterizerState().cullMode == gl::CullFaceMode::FrontAndBack);
}
}  // anonymous namespace

// StateManager11::ViewCache Implementation.
template <typename ViewType, typename DescType>
StateManager11::ViewCache<ViewType, DescType>::ViewCache() : mHighestUsedView(0)
{
}

template <typename ViewType, typename DescType>
StateManager11::ViewCache<ViewType, DescType>::~ViewCache()
{
}

template <typename ViewType, typename DescType>
void StateManager11::ViewCache<ViewType, DescType>::update(size_t resourceIndex, ViewType *view)
{
    ASSERT(resourceIndex < mCurrentViews.size());
    ViewRecord<DescType> *record = &mCurrentViews[resourceIndex];

    record->view = reinterpret_cast<uintptr_t>(view);
    if (view)
    {
        record->resource = reinterpret_cast<uintptr_t>(GetViewResource(view));
        view->GetDesc(&record->desc);
        mHighestUsedView = std::max(resourceIndex + 1, mHighestUsedView);
    }
    else
    {
        record->resource = 0;

        if (resourceIndex + 1 == mHighestUsedView)
        {
            do
            {
                --mHighestUsedView;
            } while (mHighestUsedView > 0 && mCurrentViews[mHighestUsedView].view == 0);
        }
    }
}

template <typename ViewType, typename DescType>
void StateManager11::ViewCache<ViewType, DescType>::clear()
{
    if (mCurrentViews.empty())
    {
        return;
    }

    memset(&mCurrentViews[0], 0, sizeof(ViewRecord<DescType>) * mCurrentViews.size());
    mHighestUsedView = 0;
}

StateManager11::SRVCache *StateManager11::getSRVCache(gl::ShaderType shaderType)
{
    switch (shaderType)
    {
        case gl::ShaderType::Vertex:
            return &mCurVertexSRVs;
        case gl::ShaderType::Fragment:
            return &mCurPixelSRVs;
        case gl::ShaderType::Compute:
            return &mCurComputeSRVs;
        default:
            UNREACHABLE();
            return &mCurVertexSRVs;
    }
}

// ShaderConstants11 implementation
ShaderConstants11::ShaderConstants11()
    : mNumActiveVSSamplers(0), mNumActivePSSamplers(0), mNumActiveCSSamplers(0)
{
    mShaderConstantsDirty.set();
}

ShaderConstants11::~ShaderConstants11()
{
}

void ShaderConstants11::init(const gl::Caps &caps)
{
    mSamplerMetadataVS.resize(caps.maxVertexTextureImageUnits);
    mSamplerMetadataPS.resize(caps.maxTextureImageUnits);
    mSamplerMetadataCS.resize(caps.maxComputeTextureImageUnits);
}

size_t ShaderConstants11::getRequiredBufferSize(gl::ShaderType shaderType) const
{
    switch (shaderType)
    {
        case gl::ShaderType::Vertex:
            return sizeof(Vertex) + mSamplerMetadataVS.size() * sizeof(SamplerMetadata);
        case gl::ShaderType::Fragment:
            return sizeof(Pixel) + mSamplerMetadataPS.size() * sizeof(SamplerMetadata);
        case gl::ShaderType::Compute:
            return sizeof(Compute) + mSamplerMetadataCS.size() * sizeof(SamplerMetadata);
        default:
            UNREACHABLE();
            return 0;
    }
}

void ShaderConstants11::markDirty()
{
    mShaderConstantsDirty.set();
    mNumActiveVSSamplers = 0;
    mNumActivePSSamplers = 0;
    mNumActiveCSSamplers = 0;
}

bool ShaderConstants11::updateSamplerMetadata(SamplerMetadata *data, const gl::Texture &texture)
{
    bool dirty             = false;
    unsigned int baseLevel = texture.getTextureState().getEffectiveBaseLevel();
    gl::TextureTarget target = (texture.getType() == gl::TextureType::CubeMap)
                                   ? gl::kCubeMapTextureTargetMin
                                   : gl::NonCubeTextureTypeToTarget(texture.getType());
    GLenum sizedFormat = texture.getFormat(target, baseLevel).info->sizedInternalFormat;
    if (data->baseLevel != static_cast<int>(baseLevel))
    {
        data->baseLevel = static_cast<int>(baseLevel);
        dirty           = true;
    }

    // Some metadata is needed only for integer textures. We avoid updating the constant buffer
    // unnecessarily by changing the data only in case the texture is an integer texture and
    // the values have changed.
    bool needIntegerTextureMetadata = false;
    // internalFormatBits == 0 means a 32-bit texture in the case of integer textures.
    int internalFormatBits = 0;
    switch (sizedFormat)
    {
        case GL_RGBA32I:
        case GL_RGBA32UI:
        case GL_RGB32I:
        case GL_RGB32UI:
        case GL_RG32I:
        case GL_RG32UI:
        case GL_R32I:
        case GL_R32UI:
            needIntegerTextureMetadata = true;
            break;
        case GL_RGBA16I:
        case GL_RGBA16UI:
        case GL_RGB16I:
        case GL_RGB16UI:
        case GL_RG16I:
        case GL_RG16UI:
        case GL_R16I:
        case GL_R16UI:
            needIntegerTextureMetadata = true;
            internalFormatBits         = 16;
            break;
        case GL_RGBA8I:
        case GL_RGBA8UI:
        case GL_RGB8I:
        case GL_RGB8UI:
        case GL_RG8I:
        case GL_RG8UI:
        case GL_R8I:
        case GL_R8UI:
            needIntegerTextureMetadata = true;
            internalFormatBits         = 8;
            break;
        case GL_RGB10_A2UI:
            needIntegerTextureMetadata = true;
            internalFormatBits         = 10;
            break;
        default:
            break;
    }
    if (needIntegerTextureMetadata)
    {
        if (data->internalFormatBits != internalFormatBits)
        {
            data->internalFormatBits = internalFormatBits;
            dirty                    = true;
        }
        // Pack the wrap values into one integer so we can fit all the metadata in one 4-integer
        // vector.
        GLenum wrapS  = texture.getWrapS();
        GLenum wrapT  = texture.getWrapT();
        GLenum wrapR  = texture.getWrapR();
        int wrapModes = GetWrapBits(wrapS) | (GetWrapBits(wrapT) << 2) | (GetWrapBits(wrapR) << 4);
        if (data->wrapModes != wrapModes)
        {
            data->wrapModes = wrapModes;
            dirty           = true;
        }
    }

    return dirty;
}

void ShaderConstants11::setComputeWorkGroups(GLuint numGroupsX,
                                             GLuint numGroupsY,
                                             GLuint numGroupsZ)
{
    mCompute.numWorkGroups[0] = numGroupsX;
    mCompute.numWorkGroups[1] = numGroupsY;
    mCompute.numWorkGroups[2] = numGroupsZ;
    mShaderConstantsDirty.set(gl::ShaderType::Compute);
}

void ShaderConstants11::setMultiviewWriteToViewportIndex(GLfloat index)
{
    mVertex.multiviewWriteToViewportIndex = index;
    mPixel.multiviewWriteToViewportIndex  = index;
    mShaderConstantsDirty.set(gl::ShaderType::Vertex);
    mShaderConstantsDirty.set(gl::ShaderType::Fragment);
}

void ShaderConstants11::onViewportChange(const gl::Rectangle &glViewport,
                                         const D3D11_VIEWPORT &dxViewport,
                                         bool is9_3,
                                         bool presentPathFast)
{
    mShaderConstantsDirty.set(gl::ShaderType::Vertex);
    mShaderConstantsDirty.set(gl::ShaderType::Fragment);

    // On Feature Level 9_*, we must emulate large and/or negative viewports in the shaders
    // using viewAdjust (like the D3D9 renderer).
    if (is9_3)
    {
        mVertex.viewAdjust[0] = static_cast<float>((glViewport.width - dxViewport.Width) +
                                                   2 * (glViewport.x - dxViewport.TopLeftX)) /
                                dxViewport.Width;
        mVertex.viewAdjust[1] = static_cast<float>((glViewport.height - dxViewport.Height) +
                                                   2 * (glViewport.y - dxViewport.TopLeftY)) /
                                dxViewport.Height;
        mVertex.viewAdjust[2] = static_cast<float>(glViewport.width) / dxViewport.Width;
        mVertex.viewAdjust[3] = static_cast<float>(glViewport.height) / dxViewport.Height;
    }

    mPixel.viewCoords[0] = glViewport.width * 0.5f;
    mPixel.viewCoords[1] = glViewport.height * 0.5f;
    mPixel.viewCoords[2] = glViewport.x + (glViewport.width * 0.5f);
    mPixel.viewCoords[3] = glViewport.y + (glViewport.height * 0.5f);

    // Instanced pointsprite emulation requires ViewCoords to be defined in the
    // the vertex shader.
    mVertex.viewCoords[0] = mPixel.viewCoords[0];
    mVertex.viewCoords[1] = mPixel.viewCoords[1];
    mVertex.viewCoords[2] = mPixel.viewCoords[2];
    mVertex.viewCoords[3] = mPixel.viewCoords[3];

    const float zNear = dxViewport.MinDepth;
    const float zFar  = dxViewport.MaxDepth;

    mPixel.depthFront[0] = (zFar - zNear) * 0.5f;
    mPixel.depthFront[1] = (zNear + zFar) * 0.5f;

    mVertex.depthRange[0] = zNear;
    mVertex.depthRange[1] = zFar;
    mVertex.depthRange[2] = zFar - zNear;

    mPixel.depthRange[0] = zNear;
    mPixel.depthRange[1] = zFar;
    mPixel.depthRange[2] = zFar - zNear;

    mPixel.viewScale[0] = 1.0f;
    mPixel.viewScale[1] = presentPathFast ? 1.0f : -1.0f;
    // Updates to the multiviewWriteToViewportIndex member are to be handled whenever the draw
    // framebuffer's layout is changed.

    mVertex.viewScale[0] = mPixel.viewScale[0];
    mVertex.viewScale[1] = mPixel.viewScale[1];
}

void ShaderConstants11::onSamplerChange(gl::ShaderType shaderType,
                                        unsigned int samplerIndex,
                                        const gl::Texture &texture)
{
    switch (shaderType)
    {
        case gl::ShaderType::Vertex:
            if (updateSamplerMetadata(&mSamplerMetadataVS[samplerIndex], texture))
            {
                mNumActiveVSSamplers = 0;
            }
            break;
        case gl::ShaderType::Fragment:
            if (updateSamplerMetadata(&mSamplerMetadataPS[samplerIndex], texture))
            {
                mNumActivePSSamplers = 0;
            }
            break;
        case gl::ShaderType::Compute:
            if (updateSamplerMetadata(&mSamplerMetadataCS[samplerIndex], texture))
            {
                mNumActiveCSSamplers = 0;
            }
            break;
        default:
            UNREACHABLE();
            break;
    }
}

gl::Error ShaderConstants11::updateBuffer(Renderer11 *renderer,
                                          gl::ShaderType shaderType,
                                          const ProgramD3D &programD3D,
                                          const d3d11::Buffer &driverConstantBuffer)
{
    bool dirty                 = false;
    size_t dataSize            = 0;
    const uint8_t *data        = nullptr;
    const uint8_t *samplerData = nullptr;

    // Re-upload the sampler meta-data if the current program uses more samplers
    // than we previously uploaded.
    int numSamplers = programD3D.getUsedSamplerRange(shaderType);

    switch (shaderType)
    {
        case gl::ShaderType::Vertex:
            dirty = mShaderConstantsDirty[gl::ShaderType::Vertex] ||
                    (mNumActiveVSSamplers < numSamplers);
            dataSize                = sizeof(Vertex);
            data                    = reinterpret_cast<const uint8_t *>(&mVertex);
            samplerData             = reinterpret_cast<const uint8_t *>(mSamplerMetadataVS.data());
            mShaderConstantsDirty.set(gl::ShaderType::Vertex, false);
            mNumActiveVSSamplers    = numSamplers;
            break;
        case gl::ShaderType::Fragment:
            dirty = mShaderConstantsDirty[gl::ShaderType::Fragment] ||
                    (mNumActivePSSamplers < numSamplers);
            dataSize                = sizeof(Pixel);
            data                    = reinterpret_cast<const uint8_t *>(&mPixel);
            samplerData             = reinterpret_cast<const uint8_t *>(mSamplerMetadataPS.data());
            mShaderConstantsDirty.set(gl::ShaderType::Fragment, false);
            mNumActivePSSamplers    = numSamplers;
            break;
        case gl::ShaderType::Compute:
            dirty = mShaderConstantsDirty[gl::ShaderType::Compute] ||
                    (mNumActiveCSSamplers < numSamplers);
            dataSize                = sizeof(Compute);
            data                    = reinterpret_cast<const uint8_t *>(&mCompute);
            samplerData             = reinterpret_cast<const uint8_t *>(mSamplerMetadataCS.data());
            mShaderConstantsDirty.set(gl::ShaderType::Compute, false);
            mNumActiveCSSamplers    = numSamplers;
            break;
        default:
            UNREACHABLE();
            break;
    }

    ASSERT(driverConstantBuffer.valid());

    if (!dirty)
    {
        return gl::NoError();
    }

    // Previous buffer contents are discarded, so we need to refresh the whole buffer.
    D3D11_MAPPED_SUBRESOURCE mapping = {0};
    ANGLE_TRY(
        renderer->mapResource(driverConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping));

    memcpy(mapping.pData, data, dataSize);
    memcpy(reinterpret_cast<uint8_t *>(mapping.pData) + dataSize,
           samplerData,
           sizeof(SamplerMetadata) * numSamplers);

    renderer->getDeviceContext()->Unmap(driverConstantBuffer.get(), 0);

    return gl::NoError();
}

static const GLenum QueryTypes[] = {GL_ANY_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED_CONSERVATIVE,
                                    GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, GL_TIME_ELAPSED_EXT,
                                    GL_COMMANDS_COMPLETED_CHROMIUM};

StateManager11::StateManager11(Renderer11 *renderer)
    : mRenderer(renderer),
      mInternalDirtyBits(),
      mCurBlendColor(0, 0, 0, 0),
      mCurSampleMask(0),
      mCurStencilRef(0),
      mCurStencilBackRef(0),
      mCurStencilSize(0),
      mCurScissorEnabled(false),
      mCurScissorRect(),
      mCurViewport(),
      mCurNear(0.0f),
      mCurFar(0.0f),
      mViewportBounds(),
      mRenderTargetIsDirty(true),
      mCurPresentPathFastEnabled(false),
      mCurPresentPathFastColorBufferHeight(0),
      mDirtyCurrentValueAttribs(),
      mCurrentValueAttribs(),
      mCurrentInputLayout(),
      mDirtyVertexBufferRange(gl::MAX_VERTEX_ATTRIBS, 0),
      mCurrentPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED),
      mLastAppliedDrawMode(GL_INVALID_INDEX),
      mCurrentMinimumDrawCount(0),
      mDirtySwizzles(false),
      mAppliedIB(nullptr),
      mAppliedIBFormat(DXGI_FORMAT_UNKNOWN),
      mAppliedIBOffset(0),
      mIndexBufferIsDirty(false),
      mVertexDataManager(renderer),
      mIndexDataManager(renderer),
      mIsMultiviewEnabled(false),
      mEmptySerial(mRenderer->generateSerial())
{
    mCurBlendState.blend                 = false;
    mCurBlendState.sourceBlendRGB        = GL_ONE;
    mCurBlendState.destBlendRGB          = GL_ZERO;
    mCurBlendState.sourceBlendAlpha      = GL_ONE;
    mCurBlendState.destBlendAlpha        = GL_ZERO;
    mCurBlendState.blendEquationRGB      = GL_FUNC_ADD;
    mCurBlendState.blendEquationAlpha    = GL_FUNC_ADD;
    mCurBlendState.colorMaskRed          = true;
    mCurBlendState.colorMaskBlue         = true;
    mCurBlendState.colorMaskGreen        = true;
    mCurBlendState.colorMaskAlpha        = true;
    mCurBlendState.sampleAlphaToCoverage = false;
    mCurBlendState.dither                = false;

    mCurDepthStencilState.depthTest                = false;
    mCurDepthStencilState.depthFunc                = GL_LESS;
    mCurDepthStencilState.depthMask                = true;
    mCurDepthStencilState.stencilTest              = false;
    mCurDepthStencilState.stencilMask              = true;
    mCurDepthStencilState.stencilFail              = GL_KEEP;
    mCurDepthStencilState.stencilPassDepthFail     = GL_KEEP;
    mCurDepthStencilState.stencilPassDepthPass     = GL_KEEP;
    mCurDepthStencilState.stencilWritemask         = static_cast<GLuint>(-1);
    mCurDepthStencilState.stencilBackFunc          = GL_ALWAYS;
    mCurDepthStencilState.stencilBackMask          = static_cast<GLuint>(-1);
    mCurDepthStencilState.stencilBackFail          = GL_KEEP;
    mCurDepthStencilState.stencilBackPassDepthFail = GL_KEEP;
    mCurDepthStencilState.stencilBackPassDepthPass = GL_KEEP;
    mCurDepthStencilState.stencilBackWritemask     = static_cast<GLuint>(-1);

    mCurRasterState.rasterizerDiscard   = false;
    mCurRasterState.cullFace            = false;
    mCurRasterState.cullMode            = gl::CullFaceMode::Back;
    mCurRasterState.frontFace           = GL_CCW;
    mCurRasterState.polygonOffsetFill   = false;
    mCurRasterState.polygonOffsetFactor = 0.0f;
    mCurRasterState.polygonOffsetUnits  = 0.0f;
    mCurRasterState.pointDrawMode       = false;
    mCurRasterState.multiSample         = false;

    // Start with all internal dirty bits set.
    mInternalDirtyBits.set();

    // Initially all current value attributes must be updated on first use.
    mDirtyCurrentValueAttribs.set();

    mCurrentVertexBuffers.fill(nullptr);
    mCurrentVertexStrides.fill(std::numeric_limits<UINT>::max());
    mCurrentVertexOffsets.fill(std::numeric_limits<UINT>::max());
}

StateManager11::~StateManager11()
{
}

template <typename SRVType>
void StateManager11::setShaderResourceInternal(gl::ShaderType shaderType,
                                               UINT resourceSlot,
                                               const SRVType *srv)
{
    auto *currentSRVs = getSRVCache(shaderType);
    ASSERT(static_cast<size_t>(resourceSlot) < currentSRVs->size());
    const ViewRecord<D3D11_SHADER_RESOURCE_VIEW_DESC> &record = (*currentSRVs)[resourceSlot];

    if (record.view != reinterpret_cast<uintptr_t>(srv))
    {
        ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
        ID3D11ShaderResourceView *srvPtr = srv ? srv->get() : nullptr;
        switch (shaderType)
        {
            case gl::ShaderType::Vertex:
                deviceContext->VSSetShaderResources(resourceSlot, 1, &srvPtr);
                break;
            case gl::ShaderType::Fragment:
                deviceContext->PSSetShaderResources(resourceSlot, 1, &srvPtr);
                break;
            case gl::ShaderType::Compute:
                deviceContext->CSSetShaderResources(resourceSlot, 1, &srvPtr);
                break;
            default:
                UNREACHABLE();
        }

        currentSRVs->update(resourceSlot, srvPtr);
    }
}

template <typename UAVType>
void StateManager11::setUnorderedAccessViewInternal(gl::ShaderType shaderType,
                                                    UINT resourceSlot,
                                                    const UAVType *uav)
{
    ASSERT(shaderType == gl::ShaderType::Compute);
    ASSERT(static_cast<size_t>(resourceSlot) < mCurComputeUAVs.size());
    const ViewRecord<D3D11_UNORDERED_ACCESS_VIEW_DESC> &record = mCurComputeUAVs[resourceSlot];

    if (record.view != reinterpret_cast<uintptr_t>(uav))
    {
        auto deviceContext                = mRenderer->getDeviceContext();
        ID3D11UnorderedAccessView *uavPtr = uav ? uav->get() : nullptr;
        deviceContext->CSSetUnorderedAccessViews(resourceSlot, 1, &uavPtr, nullptr);

        mCurComputeUAVs.update(resourceSlot, uavPtr);
    }
}

void StateManager11::updateStencilSizeIfChanged(bool depthStencilInitialized,
                                                unsigned int stencilSize)
{
    if (!depthStencilInitialized || stencilSize != mCurStencilSize)
    {
        mCurStencilSize = stencilSize;
        mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
    }
}

void StateManager11::checkPresentPath(const gl::Context *context)
{
    if (!mRenderer->presentPathFastEnabled())
        return;

    const auto *framebuffer          = context->getGLState().getDrawFramebuffer();
    const auto *firstColorAttachment = framebuffer->getFirstColorbuffer();
    const bool presentPathFastActive = UsePresentPathFast(mRenderer, firstColorAttachment);

    const int colorBufferHeight = firstColorAttachment ? firstColorAttachment->getSize().height : 0;

    if ((mCurPresentPathFastEnabled != presentPathFastActive) ||
        (presentPathFastActive && (colorBufferHeight != mCurPresentPathFastColorBufferHeight)))
    {
        mCurPresentPathFastEnabled           = presentPathFastActive;
        mCurPresentPathFastColorBufferHeight = colorBufferHeight;

        // Scissor rect may need to be vertically inverted
        mInternalDirtyBits.set(DIRTY_BIT_SCISSOR_STATE);

        // Cull Mode may need to be inverted
        mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);

        // Viewport may need to be vertically inverted
        invalidateViewport(context);
    }
}

gl::Error StateManager11::updateStateForCompute(const gl::Context *context,
                                                GLuint numGroupsX,
                                                GLuint numGroupsY,
                                                GLuint numGroupsZ)
{
    mShaderConstants.setComputeWorkGroups(numGroupsX, numGroupsY, numGroupsZ);

    // TODO(jmadill): Use dirty bits.
    const auto &glState = context->getGLState();
    auto *programD3D    = GetImplAs<ProgramD3D>(glState.getProgram());
    programD3D->updateSamplerMapping();

    // TODO(jmadill): Use dirty bits.
    ANGLE_TRY(generateSwizzlesForShader(context, gl::ShaderType::Compute));

    // TODO(jmadill): More complete implementation.
    ANGLE_TRY(syncTexturesForCompute(context));

    // TODO(Xinghua): applyUniformBuffers for compute shader.

    return gl::NoError();
}

void StateManager11::syncState(const gl::Context *context, const gl::State::DirtyBits &dirtyBits)
{
    if (!dirtyBits.any())
    {
        return;
    }

    const gl::State &state = context->getGLState();

    for (size_t dirtyBit : dirtyBits)
    {
        switch (dirtyBit)
        {
            case gl::State::DIRTY_BIT_BLEND_EQUATIONS:
            {
                const gl::BlendState &blendState = state.getBlendState();
                if (blendState.blendEquationRGB != mCurBlendState.blendEquationRGB ||
                    blendState.blendEquationAlpha != mCurBlendState.blendEquationAlpha)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_BLEND_FUNCS:
            {
                const gl::BlendState &blendState = state.getBlendState();
                if (blendState.sourceBlendRGB != mCurBlendState.sourceBlendRGB ||
                    blendState.destBlendRGB != mCurBlendState.destBlendRGB ||
                    blendState.sourceBlendAlpha != mCurBlendState.sourceBlendAlpha ||
                    blendState.destBlendAlpha != mCurBlendState.destBlendAlpha)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_BLEND_ENABLED:
                if (state.getBlendState().blend != mCurBlendState.blend)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_SAMPLE_ALPHA_TO_COVERAGE_ENABLED:
                if (state.getBlendState().sampleAlphaToCoverage !=
                    mCurBlendState.sampleAlphaToCoverage)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_DITHER_ENABLED:
                if (state.getBlendState().dither != mCurBlendState.dither)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_COLOR_MASK:
            {
                const gl::BlendState &blendState = state.getBlendState();
                if (blendState.colorMaskRed != mCurBlendState.colorMaskRed ||
                    blendState.colorMaskGreen != mCurBlendState.colorMaskGreen ||
                    blendState.colorMaskBlue != mCurBlendState.colorMaskBlue ||
                    blendState.colorMaskAlpha != mCurBlendState.colorMaskAlpha)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_BLEND_COLOR:
                if (state.getBlendColor() != mCurBlendColor)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_DEPTH_MASK:
                if (state.getDepthStencilState().depthMask != mCurDepthStencilState.depthMask)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_DEPTH_TEST_ENABLED:
                if (state.getDepthStencilState().depthTest != mCurDepthStencilState.depthTest)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_DEPTH_FUNC:
                if (state.getDepthStencilState().depthFunc != mCurDepthStencilState.depthFunc)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_STENCIL_TEST_ENABLED:
                if (state.getDepthStencilState().stencilTest != mCurDepthStencilState.stencilTest)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_STENCIL_FUNCS_FRONT:
            {
                const gl::DepthStencilState &depthStencil = state.getDepthStencilState();
                if (depthStencil.stencilFunc != mCurDepthStencilState.stencilFunc ||
                    depthStencil.stencilMask != mCurDepthStencilState.stencilMask ||
                    state.getStencilRef() != mCurStencilRef)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_STENCIL_FUNCS_BACK:
            {
                const gl::DepthStencilState &depthStencil = state.getDepthStencilState();
                if (depthStencil.stencilBackFunc != mCurDepthStencilState.stencilBackFunc ||
                    depthStencil.stencilBackMask != mCurDepthStencilState.stencilBackMask ||
                    state.getStencilBackRef() != mCurStencilBackRef)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_STENCIL_WRITEMASK_FRONT:
                if (state.getDepthStencilState().stencilWritemask !=
                    mCurDepthStencilState.stencilWritemask)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_STENCIL_WRITEMASK_BACK:
                if (state.getDepthStencilState().stencilBackWritemask !=
                    mCurDepthStencilState.stencilBackWritemask)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_STENCIL_OPS_FRONT:
            {
                const gl::DepthStencilState &depthStencil = state.getDepthStencilState();
                if (depthStencil.stencilFail != mCurDepthStencilState.stencilFail ||
                    depthStencil.stencilPassDepthFail !=
                        mCurDepthStencilState.stencilPassDepthFail ||
                    depthStencil.stencilPassDepthPass != mCurDepthStencilState.stencilPassDepthPass)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_STENCIL_OPS_BACK:
            {
                const gl::DepthStencilState &depthStencil = state.getDepthStencilState();
                if (depthStencil.stencilBackFail != mCurDepthStencilState.stencilBackFail ||
                    depthStencil.stencilBackPassDepthFail !=
                        mCurDepthStencilState.stencilBackPassDepthFail ||
                    depthStencil.stencilBackPassDepthPass !=
                        mCurDepthStencilState.stencilBackPassDepthPass)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_CULL_FACE_ENABLED:
                if (state.getRasterizerState().cullFace != mCurRasterState.cullFace)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
                    mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);
                }
                break;
            case gl::State::DIRTY_BIT_CULL_FACE:
                if (state.getRasterizerState().cullMode != mCurRasterState.cullMode)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
                    mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);
                }
                break;
            case gl::State::DIRTY_BIT_FRONT_FACE:
                if (state.getRasterizerState().frontFace != mCurRasterState.frontFace)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
                    mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);
                }
                break;
            case gl::State::DIRTY_BIT_POLYGON_OFFSET_FILL_ENABLED:
                if (state.getRasterizerState().polygonOffsetFill !=
                    mCurRasterState.polygonOffsetFill)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_POLYGON_OFFSET:
            {
                const gl::RasterizerState &rasterState = state.getRasterizerState();
                if (rasterState.polygonOffsetFactor != mCurRasterState.polygonOffsetFactor ||
                    rasterState.polygonOffsetUnits != mCurRasterState.polygonOffsetUnits)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
                }
                break;
            }
            case gl::State::DIRTY_BIT_RASTERIZER_DISCARD_ENABLED:
                if (state.getRasterizerState().rasterizerDiscard !=
                    mCurRasterState.rasterizerDiscard)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);

                    // Enabling/disabling rasterizer discard affects the pixel shader.
                    invalidateShaders();
                }
                break;
            case gl::State::DIRTY_BIT_SCISSOR:
                if (state.getScissor() != mCurScissorRect)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_SCISSOR_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_SCISSOR_TEST_ENABLED:
                if (state.isScissorTestEnabled() != mCurScissorEnabled)
                {
                    mInternalDirtyBits.set(DIRTY_BIT_SCISSOR_STATE);
                    // Rasterizer state update needs mCurScissorsEnabled and updates when it changes
                    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
                }
                break;
            case gl::State::DIRTY_BIT_DEPTH_RANGE:
                if (state.getNearPlane() != mCurNear || state.getFarPlane() != mCurFar)
                {
                    invalidateViewport(context);
                }
                break;
            case gl::State::DIRTY_BIT_VIEWPORT:
                if (state.getViewport() != mCurViewport)
                {
                    invalidateViewport(context);
                }
                break;
            case gl::State::DIRTY_BIT_DRAW_FRAMEBUFFER_BINDING:
                invalidateRenderTarget();
                if (mIsMultiviewEnabled)
                {
                    handleMultiviewDrawFramebufferChange(context);
                }
                break;
            case gl::State::DIRTY_BIT_VERTEX_ARRAY_BINDING:
                invalidateVertexBuffer();
                // Force invalidate the current value attributes, since the VertexArray11 keeps an
                // internal cache of TranslatedAttributes, and they CurrentValue attributes are
                // owned by the StateManager11/Context.
                mDirtyCurrentValueAttribs.set();
                // Invalidate the cached index buffer.
                invalidateIndexBuffer();
                break;
            case gl::State::DIRTY_BIT_UNIFORM_BUFFER_BINDINGS:
                invalidateProgramUniformBuffers();
                break;
            case gl::State::DIRTY_BIT_TEXTURE_BINDINGS:
                invalidateTexturesAndSamplers();
                break;
            case gl::State::DIRTY_BIT_SAMPLER_BINDINGS:
                invalidateTexturesAndSamplers();
                break;
            case gl::State::DIRTY_BIT_TRANSFORM_FEEDBACK_BINDING:
                invalidateTransformFeedback();
                break;
            case gl::State::DIRTY_BIT_PROGRAM_EXECUTABLE:
            {
                mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);
                invalidateShaders();
                invalidateVertexBuffer();
                invalidateRenderTarget();
                invalidateTexturesAndSamplers();
                invalidateProgramUniforms();
                invalidateProgramUniformBuffers();
                invalidateDriverUniforms();
                if (mIsMultiviewEnabled)
                {
                    gl::VertexArray *vao = state.getVertexArray();
                    ASSERT(vao);
                    // If ANGLE_multiview is enabled, the attribute divisor has to be updated for
                    // each binding.
                    VertexArray11 *vao11       = GetImplAs<VertexArray11>(vao);
                    const gl::Program *program = state.getProgram();
                    ASSERT(program);
                    int numViews = program->usesMultiview() ? program->getNumViews() : 1;
                    vao11->markAllAttributeDivisorsForAdjustment(numViews);
                }
                break;
            }
            case gl::State::DIRTY_BIT_CURRENT_VALUES:
            {
                for (auto attribIndex : state.getAndResetDirtyCurrentValues())
                {
                    invalidateCurrentValueAttrib(attribIndex);
                }
                break;
            }
            default:
                break;
        }
    }

    // TODO(jmadill): Input layout and vertex buffer state.
}

void StateManager11::handleMultiviewDrawFramebufferChange(const gl::Context *context)
{
    const auto &glState                    = context->getGLState();
    const gl::Framebuffer *drawFramebuffer = glState.getDrawFramebuffer();
    ASSERT(drawFramebuffer != nullptr);

    // Update viewport offsets.
    const std::vector<gl::Offset> *attachmentViewportOffsets =
        drawFramebuffer->getViewportOffsets();
    const std::vector<gl::Offset> &viewportOffsets =
        attachmentViewportOffsets != nullptr
            ? *attachmentViewportOffsets
            : gl::FramebufferAttachment::GetDefaultViewportOffsetVector();
    if (mViewportOffsets != viewportOffsets)
    {
        mViewportOffsets = viewportOffsets;

        // Because new viewport offsets are to be applied, we have to mark the internal viewport and
        // scissor state as dirty.
        invalidateViewport(context);
        mInternalDirtyBits.set(DIRTY_BIT_SCISSOR_STATE);
    }
    switch (drawFramebuffer->getMultiviewLayout())
    {
        case GL_FRAMEBUFFER_MULTIVIEW_SIDE_BY_SIDE_ANGLE:
            mShaderConstants.setMultiviewWriteToViewportIndex(1.0f);
            break;
        case GL_FRAMEBUFFER_MULTIVIEW_LAYERED_ANGLE:
            // Because the base view index is applied as an offset to the 2D texture array when the
            // RTV is created, we just have to pass a boolean to select which code path is to be
            // used.
            mShaderConstants.setMultiviewWriteToViewportIndex(0.0f);
            break;
        default:
            // There is no need to update the value in the constant buffer if the active framebuffer
            // object does not have a multiview layout.
            break;
    }
}

gl::Error StateManager11::syncBlendState(const gl::Context *context,
                                         const gl::Framebuffer *framebuffer,
                                         const gl::BlendState &blendState,
                                         const gl::ColorF &blendColor,
                                         unsigned int sampleMask)
{
    const d3d11::BlendState *dxBlendState = nullptr;
    const d3d11::BlendStateKey &key =
        RenderStateCache::GetBlendStateKey(context, framebuffer, blendState);

    ANGLE_TRY(mRenderer->getBlendState(key, &dxBlendState));

    ASSERT(dxBlendState != nullptr);

    float blendColors[4] = {0.0f};
    if (blendState.sourceBlendRGB != GL_CONSTANT_ALPHA &&
        blendState.sourceBlendRGB != GL_ONE_MINUS_CONSTANT_ALPHA &&
        blendState.destBlendRGB != GL_CONSTANT_ALPHA &&
        blendState.destBlendRGB != GL_ONE_MINUS_CONSTANT_ALPHA)
    {
        blendColors[0] = blendColor.red;
        blendColors[1] = blendColor.green;
        blendColors[2] = blendColor.blue;
        blendColors[3] = blendColor.alpha;
    }
    else
    {
        blendColors[0] = blendColor.alpha;
        blendColors[1] = blendColor.alpha;
        blendColors[2] = blendColor.alpha;
        blendColors[3] = blendColor.alpha;
    }

    mRenderer->getDeviceContext()->OMSetBlendState(dxBlendState->get(), blendColors, sampleMask);

    mCurBlendState = blendState;
    mCurBlendColor = blendColor;
    mCurSampleMask = sampleMask;

    return gl::NoError();
}

gl::Error StateManager11::syncDepthStencilState(const gl::State &glState)
{
    mCurDepthStencilState = glState.getDepthStencilState();
    mCurStencilRef        = glState.getStencilRef();
    mCurStencilBackRef    = glState.getStencilBackRef();

    // get the maximum size of the stencil ref
    unsigned int maxStencil = 0;
    if (mCurDepthStencilState.stencilTest && mCurStencilSize > 0)
    {
        maxStencil = (1 << mCurStencilSize) - 1;
    }
    ASSERT((mCurDepthStencilState.stencilWritemask & maxStencil) ==
           (mCurDepthStencilState.stencilBackWritemask & maxStencil));
    ASSERT(gl::clamp(mCurStencilRef, 0, static_cast<int>(maxStencil)) ==
           gl::clamp(mCurStencilBackRef, 0, static_cast<int>(maxStencil)));
    ASSERT((mCurDepthStencilState.stencilMask & maxStencil) ==
           (mCurDepthStencilState.stencilBackMask & maxStencil));

    gl::DepthStencilState modifiedGLState = glState.getDepthStencilState();

    ASSERT(mCurDisableDepth.valid() && mCurDisableStencil.valid());

    if (mCurDisableDepth.value())
    {
        modifiedGLState.depthTest = false;
        modifiedGLState.depthMask = false;
    }

    if (mCurDisableStencil.value())
    {
        modifiedGLState.stencilTest = false;
    }
    if (!modifiedGLState.stencilTest)
    {
        modifiedGLState.stencilWritemask = 0;
        modifiedGLState.stencilBackWritemask = 0;
    }

    // If STENCIL_TEST is disabled in glState, stencil testing and writing should be disabled.
    // Verify that's true in the modifiedGLState so it is propagated to d3dState.
    ASSERT(glState.getDepthStencilState().stencilTest ||
           (!modifiedGLState.stencilTest && modifiedGLState.stencilWritemask == 0 &&
            modifiedGLState.stencilBackWritemask == 0));

    const d3d11::DepthStencilState *d3dState = nullptr;
    ANGLE_TRY(mRenderer->getDepthStencilState(modifiedGLState, &d3dState));
    ASSERT(d3dState);

    // Max D3D11 stencil reference value is 0xFF,
    // corresponding to the max 8 bits in a stencil buffer
    // GL specifies we should clamp the ref value to the
    // nearest bit depth when doing stencil ops
    static_assert(D3D11_DEFAULT_STENCIL_READ_MASK == 0xFF,
                  "Unexpected value of D3D11_DEFAULT_STENCIL_READ_MASK");
    static_assert(D3D11_DEFAULT_STENCIL_WRITE_MASK == 0xFF,
                  "Unexpected value of D3D11_DEFAULT_STENCIL_WRITE_MASK");
    UINT dxStencilRef = static_cast<UINT>(gl::clamp(mCurStencilRef, 0, 0xFF));

    mRenderer->getDeviceContext()->OMSetDepthStencilState(d3dState->get(), dxStencilRef);

    return gl::NoError();
}

gl::Error StateManager11::syncRasterizerState(const gl::Context *context,
                                              const gl::DrawCallParams &drawCallParams)
{
    // TODO: Remove pointDrawMode and multiSample from gl::RasterizerState.
    gl::RasterizerState rasterState = context->getGLState().getRasterizerState();
    rasterState.pointDrawMode       = (drawCallParams.mode() == GL_POINTS);
    rasterState.multiSample         = mCurRasterState.multiSample;

    ID3D11RasterizerState *dxRasterState = nullptr;

    if (mCurPresentPathFastEnabled)
    {
        gl::RasterizerState modifiedRasterState = rasterState;

        // If prseent path fast is active then we need invert the front face state.
        // This ensures that both gl_FrontFacing is correct, and front/back culling
        // is performed correctly.
        if (modifiedRasterState.frontFace == GL_CCW)
        {
            modifiedRasterState.frontFace = GL_CW;
        }
        else
        {
            ASSERT(modifiedRasterState.frontFace == GL_CW);
            modifiedRasterState.frontFace = GL_CCW;
        }

        ANGLE_TRY(
            mRenderer->getRasterizerState(modifiedRasterState, mCurScissorEnabled, &dxRasterState));
    }
    else
    {
        ANGLE_TRY(mRenderer->getRasterizerState(rasterState, mCurScissorEnabled, &dxRasterState));
    }

    mRenderer->getDeviceContext()->RSSetState(dxRasterState);

    mCurRasterState = rasterState;

    return gl::NoError();
}

void StateManager11::syncScissorRectangle(const gl::Rectangle &scissor, bool enabled)
{
    int modifiedScissorY = scissor.y;
    if (mCurPresentPathFastEnabled)
    {
        modifiedScissorY = mCurPresentPathFastColorBufferHeight - scissor.height - scissor.y;
    }

    if (enabled)
    {
        std::array<D3D11_RECT, gl::IMPLEMENTATION_ANGLE_MULTIVIEW_MAX_VIEWS> rectangles;
        const UINT numRectangles = static_cast<UINT>(mViewportOffsets.size());
        for (UINT i = 0u; i < numRectangles; ++i)
        {
            D3D11_RECT &rect = rectangles[i];
            int x            = scissor.x + mViewportOffsets[i].x;
            int y            = modifiedScissorY + mViewportOffsets[i].y;
            rect.left        = std::max(0, x);
            rect.top         = std::max(0, y);
            rect.right       = x + std::max(0, scissor.width);
            rect.bottom      = y + std::max(0, scissor.height);
        }
        mRenderer->getDeviceContext()->RSSetScissorRects(numRectangles, rectangles.data());
    }

    mCurScissorRect      = scissor;
    mCurScissorEnabled   = enabled;
}

void StateManager11::syncViewport(const gl::Context *context)
{
    const auto &glState          = context->getGLState();
    gl::Framebuffer *framebuffer = glState.getDrawFramebuffer();
    float actualZNear            = gl::clamp01(glState.getNearPlane());
    float actualZFar             = gl::clamp01(glState.getFarPlane());

    const auto &caps         = context->getCaps();
    int dxMaxViewportBoundsX = static_cast<int>(caps.maxViewportWidth);
    int dxMaxViewportBoundsY = static_cast<int>(caps.maxViewportHeight);
    int dxMinViewportBoundsX = -dxMaxViewportBoundsX;
    int dxMinViewportBoundsY = -dxMaxViewportBoundsY;

    bool is9_3 = mRenderer->getRenderer11DeviceCaps().featureLevel <= D3D_FEATURE_LEVEL_9_3;

    if (is9_3)
    {
        // Feature Level 9 viewports shouldn't exceed the dimensions of the rendertarget.
        dxMaxViewportBoundsX = static_cast<int>(mViewportBounds.width);
        dxMaxViewportBoundsY = static_cast<int>(mViewportBounds.height);
        dxMinViewportBoundsX = 0;
        dxMinViewportBoundsY = 0;
    }

    const auto &viewport = glState.getViewport();
    std::array<D3D11_VIEWPORT, gl::IMPLEMENTATION_ANGLE_MULTIVIEW_MAX_VIEWS> dxViewports;
    const UINT numRectangles = static_cast<UINT>(mViewportOffsets.size());

    int dxViewportTopLeftX = 0;
    int dxViewportTopLeftY = 0;
    int dxViewportWidth    = 0;
    int dxViewportHeight   = 0;

    for (UINT i = 0u; i < numRectangles; ++i)
    {
        dxViewportTopLeftX = gl::clamp(viewport.x + mViewportOffsets[i].x, dxMinViewportBoundsX,
                                       dxMaxViewportBoundsX);
        dxViewportTopLeftY = gl::clamp(viewport.y + mViewportOffsets[i].y, dxMinViewportBoundsY,
                                       dxMaxViewportBoundsY);
        dxViewportWidth  = gl::clamp(viewport.width, 0, dxMaxViewportBoundsX - dxViewportTopLeftX);
        dxViewportHeight = gl::clamp(viewport.height, 0, dxMaxViewportBoundsY - dxViewportTopLeftY);

        D3D11_VIEWPORT &dxViewport = dxViewports[i];
        dxViewport.TopLeftX        = static_cast<float>(dxViewportTopLeftX);
        if (mCurPresentPathFastEnabled)
        {
            // When present path fast is active and we're rendering to framebuffer 0, we must invert
            // the viewport in Y-axis.
            // NOTE: We delay the inversion until right before the call to RSSetViewports, and leave
            // dxViewportTopLeftY unchanged. This allows us to calculate viewAdjust below using the
            // unaltered dxViewportTopLeftY value.
            dxViewport.TopLeftY = static_cast<float>(mCurPresentPathFastColorBufferHeight -
                                                     dxViewportTopLeftY - dxViewportHeight);
        }
        else
        {
            dxViewport.TopLeftY = static_cast<float>(dxViewportTopLeftY);
        }

        // The es 3.1 spec section 9.2 states that, "If there are no attachments, rendering
        // will be limited to a rectangle having a lower left of (0, 0) and an upper right of
        // (width, height), where width and height are the framebuffer object's default width
        // and height." See http://anglebug.com/1594
        // If the Framebuffer has no color attachment and the default width or height is smaller
        // than the current viewport, use the smaller of the two sizes.
        // If framebuffer default width or height is 0, the params should not set.
        if (!framebuffer->getFirstNonNullAttachment() &&
            (framebuffer->getDefaultWidth() || framebuffer->getDefaultHeight()))
        {
            dxViewport.Width =
                static_cast<GLfloat>(std::min(viewport.width, framebuffer->getDefaultWidth()));
            dxViewport.Height =
                static_cast<GLfloat>(std::min(viewport.height, framebuffer->getDefaultHeight()));
        }
        else
        {
            dxViewport.Width  = static_cast<float>(dxViewportWidth);
            dxViewport.Height = static_cast<float>(dxViewportHeight);
        }
        dxViewport.MinDepth = actualZNear;
        dxViewport.MaxDepth = actualZFar;
    }

    mRenderer->getDeviceContext()->RSSetViewports(numRectangles, dxViewports.data());

    mCurViewport = viewport;
    mCurNear     = actualZNear;
    mCurFar      = actualZFar;

    const D3D11_VIEWPORT adjustViewport = {static_cast<FLOAT>(dxViewportTopLeftX),
                                           static_cast<FLOAT>(dxViewportTopLeftY),
                                           static_cast<FLOAT>(dxViewportWidth),
                                           static_cast<FLOAT>(dxViewportHeight),
                                           actualZNear,
                                           actualZFar};
    mShaderConstants.onViewportChange(viewport, adjustViewport, is9_3, mCurPresentPathFastEnabled);
}

void StateManager11::invalidateRenderTarget()
{
    mRenderTargetIsDirty = true;
}

void StateManager11::processFramebufferInvalidation(const gl::Context *context)
{
    if (!mRenderTargetIsDirty)
    {
        return;
    }

    ASSERT(context);

    mRenderTargetIsDirty = false;
    mInternalDirtyBits.set(DIRTY_BIT_RENDER_TARGET);

    // The pixel shader is dependent on the output layout.
    invalidateShaders();

    // The D3D11 blend state is heavily dependent on the current render target.
    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);

    gl::Framebuffer *fbo = context->getGLState().getDrawFramebuffer();
    ASSERT(fbo);

    // Disable the depth test/depth write if we are using a stencil-only attachment.
    // This is because ANGLE emulates stencil-only with D24S8 on D3D11 - we should neither read
    // nor write to the unused depth part of this emulated texture.
    bool disableDepth = (!fbo->hasDepth() && fbo->hasStencil());

    // Similarly we disable the stencil portion of the DS attachment if the app only binds depth.
    bool disableStencil = (fbo->hasDepth() && !fbo->hasStencil());

    if (!mCurDisableDepth.valid() || disableDepth != mCurDisableDepth.value() ||
        !mCurDisableStencil.valid() || disableStencil != mCurDisableStencil.value())
    {
        mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
        mCurDisableDepth   = disableDepth;
        mCurDisableStencil = disableStencil;
    }

    bool multiSample = (fbo->getCachedSamples(context) != 0);
    if (multiSample != mCurRasterState.multiSample)
    {
        mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
        mCurRasterState.multiSample = multiSample;
    }

    checkPresentPath(context);

    if (mRenderer->getRenderer11DeviceCaps().featureLevel <= D3D_FEATURE_LEVEL_9_3)
    {
        const auto *firstAttachment = fbo->getFirstNonNullAttachment();
        if (firstAttachment)
        {
            const auto &size = firstAttachment->getSize();
            if (mViewportBounds.width != size.width || mViewportBounds.height != size.height)
            {
                mViewportBounds = gl::Extents(size.width, size.height, 1);
                invalidateViewport(context);
            }
        }
    }
}

void StateManager11::invalidateBoundViews()
{
    mCurVertexSRVs.clear();
    mCurPixelSRVs.clear();

    invalidateRenderTarget();
}

void StateManager11::invalidateVertexBuffer()
{
    unsigned int limit = std::min<unsigned int>(mRenderer->getNativeCaps().maxVertexAttributes,
                                                gl::MAX_VERTEX_ATTRIBS);
    mDirtyVertexBufferRange = gl::RangeUI(0, limit);
    invalidateInputLayout();
    invalidateShaders();
    mInternalDirtyBits.set(DIRTY_BIT_CURRENT_VALUE_ATTRIBS);
}

void StateManager11::invalidateViewport(const gl::Context *context)
{
    mInternalDirtyBits.set(DIRTY_BIT_VIEWPORT_STATE);

    // Viewport affects the driver constants.
    invalidateDriverUniforms();
}

void StateManager11::invalidateTexturesAndSamplers()
{
    mInternalDirtyBits.set(DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE);
    invalidateSwizzles();

    // Texture state affects the driver uniforms (base level, etc).
    invalidateDriverUniforms();
}

void StateManager11::invalidateSwizzles()
{
    mDirtySwizzles = true;
}

void StateManager11::invalidateProgramUniforms()
{
    mInternalDirtyBits.set(DIRTY_BIT_PROGRAM_UNIFORMS);
}

void StateManager11::invalidateDriverUniforms()
{
    mInternalDirtyBits.set(DIRTY_BIT_DRIVER_UNIFORMS);
}

void StateManager11::invalidateProgramUniformBuffers()
{
    mInternalDirtyBits.set(DIRTY_BIT_PROGRAM_UNIFORM_BUFFERS);
}

void StateManager11::invalidateConstantBuffer(unsigned int slot)
{
    if (slot == d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DRIVER)
    {
        invalidateDriverUniforms();
    }
    else if (slot == d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DEFAULT_UNIFORM_BLOCK)
    {
        invalidateProgramUniforms();
    }
    else
    {
        invalidateProgramUniformBuffers();
    }
}

void StateManager11::invalidateShaders()
{
    mInternalDirtyBits.set(DIRTY_BIT_SHADERS);
}

void StateManager11::invalidateTransformFeedback()
{
    // Transform feedback affects the stream-out geometry shader.
    invalidateShaders();
    mInternalDirtyBits.set(DIRTY_BIT_TRANSFORM_FEEDBACK);
    // syncPrimitiveTopology checks the transform feedback state.
    mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);
}

void StateManager11::invalidateInputLayout()
{
    mInternalDirtyBits.set(DIRTY_BIT_VERTEX_BUFFERS_AND_INPUT_LAYOUT);
}

void StateManager11::invalidateIndexBuffer()
{
    mIndexBufferIsDirty = true;
}

void StateManager11::setRenderTarget(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv)
{
    if ((rtv && unsetConflictingView(rtv)) || (dsv && unsetConflictingView(dsv)))
    {
        mInternalDirtyBits.set(DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE);
    }

    mRenderer->getDeviceContext()->OMSetRenderTargets(1, &rtv, dsv);
    mInternalDirtyBits.set(DIRTY_BIT_RENDER_TARGET);
}

void StateManager11::setRenderTargets(ID3D11RenderTargetView **rtvs,
                                      UINT numRTVs,
                                      ID3D11DepthStencilView *dsv)
{
    bool anyDirty = false;

    for (UINT rtvIndex = 0; rtvIndex < numRTVs; ++rtvIndex)
    {
        anyDirty = anyDirty || unsetConflictingView(rtvs[rtvIndex]);
    }

    if (dsv)
    {
        anyDirty = anyDirty || unsetConflictingView(dsv);
    }

    if (anyDirty)
    {
        mInternalDirtyBits.set(DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE);
    }

    mRenderer->getDeviceContext()->OMSetRenderTargets(numRTVs, (numRTVs > 0) ? rtvs : nullptr, dsv);
    mInternalDirtyBits.set(DIRTY_BIT_RENDER_TARGET);
}

void StateManager11::onBeginQuery(Query11 *query)
{
    mCurrentQueries.insert(query);
}

void StateManager11::onDeleteQueryObject(Query11 *query)
{
    mCurrentQueries.erase(query);
}

gl::Error StateManager11::onMakeCurrent(const gl::Context *context)
{
    const gl::State &state = context->getGLState();

    for (Query11 *query : mCurrentQueries)
    {
        ANGLE_TRY(query->pause());
    }
    mCurrentQueries.clear();

    for (GLenum queryType : QueryTypes)
    {
        gl::Query *query = state.getActiveQuery(queryType);
        if (query != nullptr)
        {
            Query11 *query11 = GetImplAs<Query11>(query);
            ANGLE_TRY(query11->resume());
            mCurrentQueries.insert(query11);
        }
    }

    return gl::NoError();
}

gl::Error StateManager11::clearSRVs(gl::ShaderType shaderType, size_t rangeStart, size_t rangeEnd)
{
    if (rangeStart == rangeEnd)
    {
        return gl::NoError();
    }

    auto *currentSRVs = getSRVCache(shaderType);
    gl::Range<size_t> clearRange(rangeStart, std::min(rangeEnd, currentSRVs->highestUsed()));
    if (clearRange.empty())
    {
        return gl::NoError();
    }

    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
    switch (shaderType)
    {
        case gl::ShaderType::Vertex:
            deviceContext->VSSetShaderResources(static_cast<unsigned int>(clearRange.low()),
                                                static_cast<unsigned int>(clearRange.length()),
                                                &mNullSRVs[0]);
            break;
        case gl::ShaderType::Fragment:
            deviceContext->PSSetShaderResources(static_cast<unsigned int>(clearRange.low()),
                                                static_cast<unsigned int>(clearRange.length()),
                                                &mNullSRVs[0]);
            break;
        case gl::ShaderType::Compute:
            deviceContext->CSSetShaderResources(static_cast<unsigned int>(clearRange.low()),
                                                static_cast<unsigned int>(clearRange.length()),
                                                &mNullSRVs[0]);
            break;
        default:
            UNREACHABLE();
            break;
    }

    for (size_t samplerIndex : clearRange)
    {
        currentSRVs->update(samplerIndex, nullptr);
    }

    return gl::NoError();
}

gl::Error StateManager11::clearUAVs(gl::ShaderType shaderType, size_t rangeStart, size_t rangeEnd)
{
    ASSERT(shaderType == gl::ShaderType::Compute);
    if (rangeStart == rangeEnd)
    {
        return gl::NoError();
    }

    gl::Range<size_t> clearRange(rangeStart, std::min(rangeEnd, mCurComputeUAVs.highestUsed()));
    if (clearRange.empty())
    {
        return gl::NoError();
    }

    auto deviceContext = mRenderer->getDeviceContext();
    deviceContext->CSSetUnorderedAccessViews(static_cast<unsigned int>(clearRange.low()),
                                             static_cast<unsigned int>(clearRange.length()),
                                             &mNullUAVs[0], nullptr);

    for (size_t index : clearRange)
    {
        mCurComputeUAVs.update(index, nullptr);
    }

    return gl::NoError();
}

bool StateManager11::unsetConflictingView(ID3D11View *view)
{
    uintptr_t resource = reinterpret_cast<uintptr_t>(GetViewResource(view));
    return unsetConflictingSRVs(gl::ShaderType::Vertex, resource, nullptr) ||
           unsetConflictingSRVs(gl::ShaderType::Fragment, resource, nullptr);
}

bool StateManager11::unsetConflictingSRVs(gl::ShaderType shaderType,
                                          uintptr_t resource,
                                          const gl::ImageIndex *index)
{
    auto *currentSRVs = getSRVCache(shaderType);

    bool foundOne = false;

    for (size_t resourceIndex = 0; resourceIndex < currentSRVs->size(); ++resourceIndex)
    {
        auto &record = (*currentSRVs)[resourceIndex];

        if (record.view && record.resource == resource &&
            (!index || ImageIndexConflictsWithSRV(*index, record.desc)))
        {
            setShaderResourceInternal<d3d11::ShaderResourceView>(
                shaderType, static_cast<UINT>(resourceIndex), nullptr);
            foundOne = true;
        }
    }

    return foundOne;
}

void StateManager11::unsetConflictingAttachmentResources(
    const gl::FramebufferAttachment *attachment,
    ID3D11Resource *resource)
{
    // Unbind render target SRVs from the shader here to prevent D3D11 warnings.
    if (attachment->type() == GL_TEXTURE)
    {
        uintptr_t resourcePtr       = reinterpret_cast<uintptr_t>(resource);
        const gl::ImageIndex &index = attachment->getTextureImageIndex();
        // The index doesn't need to be corrected for the small compressed texture workaround
        // because a rendertarget is never compressed.
        unsetConflictingSRVs(gl::ShaderType::Vertex, resourcePtr, &index);
        unsetConflictingSRVs(gl::ShaderType::Fragment, resourcePtr, &index);
    }
    else if (attachment->type() == GL_FRAMEBUFFER_DEFAULT)
    {
        uintptr_t resourcePtr = reinterpret_cast<uintptr_t>(resource);
        unsetConflictingSRVs(gl::ShaderType::Vertex, resourcePtr, nullptr);
        unsetConflictingSRVs(gl::ShaderType::Fragment, resourcePtr, nullptr);
    }
}

gl::Error StateManager11::initialize(const gl::Caps &caps, const gl::Extensions &extensions)
{
    mCurVertexSRVs.initialize(caps.maxVertexTextureImageUnits);
    mCurPixelSRVs.initialize(caps.maxTextureImageUnits);

    // TODO(xinghua.cao@intel.com): need to add compute shader texture image units.
    mCurComputeSRVs.initialize(caps.maxImageUnits);

    mCurComputeUAVs.initialize(caps.maxImageUnits);

    // Initialize cached NULL SRV block
    mNullSRVs.resize(caps.maxTextureImageUnits, nullptr);

    mNullUAVs.resize(caps.maxImageUnits, nullptr);

    mCurrentValueAttribs.resize(caps.maxVertexAttributes);

    mForceSetVertexSamplerStates.resize(caps.maxVertexTextureImageUnits, true);
    mForceSetPixelSamplerStates.resize(caps.maxTextureImageUnits, true);
    mForceSetComputeSamplerStates.resize(caps.maxComputeTextureImageUnits, true);

    mCurVertexSamplerStates.resize(caps.maxVertexTextureImageUnits);
    mCurPixelSamplerStates.resize(caps.maxTextureImageUnits);
    mCurComputeSamplerStates.resize(caps.maxComputeTextureImageUnits);

    mShaderConstants.init(caps);

    mIsMultiviewEnabled = extensions.multiview;
    mViewportOffsets.resize(1u);

    ANGLE_TRY(mVertexDataManager.initialize());

    mCurrentAttributes.reserve(gl::MAX_VERTEX_ATTRIBS);

    return gl::NoError();
}

void StateManager11::deinitialize()
{
    mCurrentValueAttribs.clear();
    mInputLayoutCache.clear();
    mVertexDataManager.deinitialize();
    mIndexDataManager.deinitialize();

    mDriverConstantBufferVS.reset();
    mDriverConstantBufferPS.reset();
    mDriverConstantBufferCS.reset();

    mPointSpriteVertexBuffer.reset();
    mPointSpriteIndexBuffer.reset();
}

gl::Error StateManager11::syncFramebuffer(const gl::Context *context, gl::Framebuffer *framebuffer)
{
    Framebuffer11 *framebuffer11 = GetImplAs<Framebuffer11>(framebuffer);

    // Applies the render target surface, depth stencil surface, viewport rectangle and
    // scissor rectangle to the renderer
    ASSERT(framebuffer && !framebuffer->hasAnyDirtyBit());

    // Check for zero-sized default framebuffer, which is a special case.
    // in this case we do not wish to modify any state and just silently return false.
    // this will not report any gl error but will cause the calling method to return.
    if (framebuffer->id() == 0)
    {
        const gl::Extents &size = framebuffer->getFirstColorbuffer()->getSize();
        if (size.width == 0 || size.height == 0)
        {
            return gl::NoError();
        }
    }

    RTVArray framebufferRTVs = {{}};

    const auto &colorRTs = framebuffer11->getCachedColorRenderTargets();

    size_t appliedRTIndex  = 0;
    bool skipInactiveRTs   = mRenderer->getWorkarounds().mrtPerfWorkaround;
    const auto &drawStates = framebuffer->getDrawBufferStates();
    gl::DrawBufferMask activeProgramOutputs =
        context->getContextState().getState().getProgram()->getActiveOutputVariables();
    UINT maxExistingRT = 0;

    for (size_t rtIndex = 0; rtIndex < colorRTs.size(); ++rtIndex)
    {
        const RenderTarget11 *renderTarget = colorRTs[rtIndex];

        // Skip inactive rendertargets if the workaround is enabled.
        if (skipInactiveRTs &&
            (!renderTarget || drawStates[rtIndex] == GL_NONE || !activeProgramOutputs[rtIndex]))
        {
            continue;
        }

        if (renderTarget)
        {
            framebufferRTVs[appliedRTIndex] = renderTarget->getRenderTargetView().get();
            ASSERT(framebufferRTVs[appliedRTIndex]);
            maxExistingRT = static_cast<UINT>(appliedRTIndex) + 1;

            // Unset conflicting texture SRVs
            const auto *attachment = framebuffer->getColorbuffer(rtIndex);
            ASSERT(attachment);
            unsetConflictingAttachmentResources(attachment, renderTarget->getTexture().get());
        }

        appliedRTIndex++;
    }

    // Get the depth stencil buffers
    ID3D11DepthStencilView *framebufferDSV = nullptr;
    const auto *depthStencilRenderTarget   = framebuffer11->getCachedDepthStencilRenderTarget();
    if (depthStencilRenderTarget)
    {
        framebufferDSV = depthStencilRenderTarget->getDepthStencilView().get();
        ASSERT(framebufferDSV);

        // Unset conflicting texture SRVs
        const auto *attachment = framebuffer->getDepthOrStencilbuffer();
        ASSERT(attachment);
        unsetConflictingAttachmentResources(attachment,
                                            depthStencilRenderTarget->getTexture().get());
    }

    // TODO(jmadill): Use context caps?
    ASSERT(maxExistingRT <= static_cast<UINT>(mRenderer->getNativeCaps().maxDrawBuffers));

    // Apply the render target and depth stencil
    mRenderer->getDeviceContext()->OMSetRenderTargets(maxExistingRT, framebufferRTVs.data(),
                                                      framebufferDSV);

    return gl::NoError();
}

void StateManager11::invalidateCurrentValueAttrib(size_t attribIndex)
{
    mDirtyCurrentValueAttribs.set(attribIndex);
    mInternalDirtyBits.set(DIRTY_BIT_CURRENT_VALUE_ATTRIBS);
    invalidateInputLayout();
    invalidateShaders();
}

gl::Error StateManager11::syncCurrentValueAttribs(const gl::State &glState)
{
    const auto &activeAttribsMask  = glState.getProgram()->getActiveAttribLocationsMask();
    const auto &dirtyActiveAttribs = (activeAttribsMask & mDirtyCurrentValueAttribs);

    if (!dirtyActiveAttribs.any())
    {
        return gl::NoError();
    }

    const auto &vertexAttributes = glState.getVertexArray()->getVertexAttributes();
    const auto &vertexBindings   = glState.getVertexArray()->getVertexBindings();
    mDirtyCurrentValueAttribs    = (mDirtyCurrentValueAttribs & ~dirtyActiveAttribs);

    for (auto attribIndex : dirtyActiveAttribs)
    {
        if (vertexAttributes[attribIndex].enabled)
            continue;

        const auto *attrib                   = &vertexAttributes[attribIndex];
        const auto &currentValue             = glState.getVertexAttribCurrentValue(attribIndex);
        TranslatedAttribute *currentValueAttrib = &mCurrentValueAttribs[attribIndex];
        currentValueAttrib->currentValueType = currentValue.Type;
        currentValueAttrib->attribute        = attrib;
        currentValueAttrib->binding          = &vertexBindings[attrib->bindingIndex];

        mDirtyVertexBufferRange.extend(static_cast<unsigned int>(attribIndex));

        ANGLE_TRY(mVertexDataManager.storeCurrentValue(currentValue, currentValueAttrib,
                                                       static_cast<size_t>(attribIndex)));
    }

    return gl::NoError();
}

void StateManager11::setInputLayout(const d3d11::InputLayout *inputLayout)
{
    if (setInputLayoutInternal(inputLayout))
    {
        invalidateInputLayout();
    }
}

bool StateManager11::setInputLayoutInternal(const d3d11::InputLayout *inputLayout)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
    if (inputLayout == nullptr)
    {
        if (!mCurrentInputLayout.empty())
        {
            deviceContext->IASetInputLayout(nullptr);
            mCurrentInputLayout.clear();
            return true;
        }
    }
    else if (inputLayout->getSerial() != mCurrentInputLayout)
    {
        deviceContext->IASetInputLayout(inputLayout->get());
        mCurrentInputLayout = inputLayout->getSerial();
        return true;
    }

    return false;
}

bool StateManager11::queueVertexBufferChange(size_t bufferIndex,
                                             ID3D11Buffer *buffer,
                                             UINT stride,
                                             UINT offset)
{
    if (buffer != mCurrentVertexBuffers[bufferIndex] ||
        stride != mCurrentVertexStrides[bufferIndex] ||
        offset != mCurrentVertexOffsets[bufferIndex])
    {
        mDirtyVertexBufferRange.extend(static_cast<unsigned int>(bufferIndex));

        mCurrentVertexBuffers[bufferIndex] = buffer;
        mCurrentVertexStrides[bufferIndex] = stride;
        mCurrentVertexOffsets[bufferIndex] = offset;
        return true;
    }

    return false;
}

void StateManager11::applyVertexBufferChanges()
{
    if (mDirtyVertexBufferRange.empty())
    {
        return;
    }

    ASSERT(mDirtyVertexBufferRange.high() <= gl::MAX_VERTEX_ATTRIBS);

    UINT start = static_cast<UINT>(mDirtyVertexBufferRange.low());

    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
    deviceContext->IASetVertexBuffers(start, static_cast<UINT>(mDirtyVertexBufferRange.length()),
                                      &mCurrentVertexBuffers[start], &mCurrentVertexStrides[start],
                                      &mCurrentVertexOffsets[start]);

    mDirtyVertexBufferRange = gl::RangeUI(gl::MAX_VERTEX_ATTRIBS, 0);
}

void StateManager11::setSingleVertexBuffer(const d3d11::Buffer *buffer, UINT stride, UINT offset)
{
    ID3D11Buffer *native = buffer ? buffer->get() : nullptr;
    if (queueVertexBufferChange(0, native, stride, offset))
    {
        invalidateInputLayout();
        applyVertexBufferChanges();
    }
}

gl::Error StateManager11::updateState(const gl::Context *context,
                                      const gl::DrawCallParams &drawCallParams)
{
    const gl::State &glState = context->getGLState();
    auto *programD3D         = GetImplAs<ProgramD3D>(glState.getProgram());

    // TODO(jmadill): Use dirty bits.
    processFramebufferInvalidation(context);

    // TODO(jmadill): Use dirty bits.
    if (programD3D->updateSamplerMapping() == ProgramD3D::SamplerMapping::WasDirty)
    {
        invalidateTexturesAndSamplers();
    }

    // TODO(jmadill): Use dirty bits.
    if (programD3D->anyShaderUniformsDirty())
    {
        mInternalDirtyBits.set(DIRTY_BIT_PROGRAM_UNIFORMS);
    }

    // Swizzling can cause internal state changes with blit shaders.
    if (mDirtySwizzles)
    {
        ANGLE_TRY(generateSwizzles(context));
        mDirtySwizzles = false;
    }

    gl::Framebuffer *framebuffer = glState.getDrawFramebuffer();
    Framebuffer11 *framebuffer11 = GetImplAs<Framebuffer11>(framebuffer);
    ANGLE_TRY(framebuffer11->markAttachmentsDirty(context));

    // TODO(jiawei.shao@intel.com): This can be recomputed only on framebuffer or multisample mask
    // state changes.
    RenderTarget11 *firstRT = framebuffer11->getFirstRenderTarget();
    int samples             = (firstRT ? firstRT->getSamples() : 0);
    unsigned int sampleMask = GetBlendSampleMask(glState, samples);
    if (sampleMask != mCurSampleMask)
    {
        mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
    }

    // Changes in the draw call can affect the vertex buffer translations.
    if (!mLastFirstVertex.valid() || mLastFirstVertex.value() != drawCallParams.firstVertex())
    {
        mLastFirstVertex = drawCallParams.firstVertex();
        invalidateInputLayout();
    }

    VertexArray11 *vao11 = GetImplAs<VertexArray11>(glState.getVertexArray());
    ANGLE_TRY(vao11->syncStateForDraw(context, drawCallParams));

    if (drawCallParams.isDrawElements())
    {
        ANGLE_TRY(applyIndexBuffer(context, drawCallParams));
    }

    if (mLastAppliedDrawMode != drawCallParams.mode())
    {
        mLastAppliedDrawMode = drawCallParams.mode();
        mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);

        bool pointDrawMode = (drawCallParams.mode() == GL_POINTS);
        if (pointDrawMode != mCurRasterState.pointDrawMode)
        {
            mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);

            // Changing from points to not points (or vice-versa) affects the geometry shader.
            invalidateShaders();
        }
    }

    auto dirtyBitsCopy = mInternalDirtyBits;
    mInternalDirtyBits.reset();

    for (auto dirtyBit : dirtyBitsCopy)
    {
        switch (dirtyBit)
        {
            case DIRTY_BIT_RENDER_TARGET:
                ANGLE_TRY(syncFramebuffer(context, framebuffer));
                break;
            case DIRTY_BIT_VIEWPORT_STATE:
                syncViewport(context);
                break;
            case DIRTY_BIT_SCISSOR_STATE:
                syncScissorRectangle(glState.getScissor(), glState.isScissorTestEnabled());
                break;
            case DIRTY_BIT_RASTERIZER_STATE:
                ANGLE_TRY(syncRasterizerState(context, drawCallParams));
                break;
            case DIRTY_BIT_BLEND_STATE:
                ANGLE_TRY(syncBlendState(context, framebuffer, glState.getBlendState(),
                                         glState.getBlendColor(), sampleMask));
                break;
            case DIRTY_BIT_DEPTH_STENCIL_STATE:
                ANGLE_TRY(syncDepthStencilState(glState));
                break;
            case DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE:
                // TODO(jmadill): More fine-grained update.
                ANGLE_TRY(syncTextures(context));
                break;
            case DIRTY_BIT_PROGRAM_UNIFORMS:
                ANGLE_TRY(applyUniforms(programD3D));
                break;
            case DIRTY_BIT_DRIVER_UNIFORMS:
                // This must happen after viewport sync; the viewport affects builtin uniforms.
                ANGLE_TRY(applyDriverUniforms(*programD3D));
                break;
            case DIRTY_BIT_PROGRAM_UNIFORM_BUFFERS:
                ANGLE_TRY(syncUniformBuffers(context, programD3D));
                break;
            case DIRTY_BIT_SHADERS:
                ANGLE_TRY(syncProgram(context, drawCallParams.mode()));
                break;
            case DIRTY_BIT_CURRENT_VALUE_ATTRIBS:
                ANGLE_TRY(syncCurrentValueAttribs(glState));
                break;
            case DIRTY_BIT_TRANSFORM_FEEDBACK:
                ANGLE_TRY(syncTransformFeedbackBuffers(context));
                break;
            case DIRTY_BIT_VERTEX_BUFFERS_AND_INPUT_LAYOUT:
                ANGLE_TRY(syncVertexBuffersAndInputLayout(context, drawCallParams));
                break;
            case DIRTY_BIT_PRIMITIVE_TOPOLOGY:
                syncPrimitiveTopology(glState, programD3D, drawCallParams.mode());
                break;
            default:
                UNREACHABLE();
                break;
        }
    }

    // Check that we haven't set any dirty bits in the flushing of the dirty bits loop.
    ASSERT(mInternalDirtyBits.none());

    return gl::NoError();
}

void StateManager11::setShaderResourceShared(gl::ShaderType shaderType,
                                             UINT resourceSlot,
                                             const d3d11::SharedSRV *srv)
{
    setShaderResourceInternal(shaderType, resourceSlot, srv);

    // TODO(jmadill): Narrower dirty region.
    mInternalDirtyBits.set(DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE);
}

void StateManager11::setShaderResource(gl::ShaderType shaderType,
                                       UINT resourceSlot,
                                       const d3d11::ShaderResourceView *srv)
{
    setShaderResourceInternal(shaderType, resourceSlot, srv);

    // TODO(jmadill): Narrower dirty region.
    mInternalDirtyBits.set(DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE);
}

void StateManager11::setPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY primitiveTopology)
{
    if (setPrimitiveTopologyInternal(primitiveTopology))
    {
        mInternalDirtyBits.set(DIRTY_BIT_PRIMITIVE_TOPOLOGY);
    }
}

bool StateManager11::setPrimitiveTopologyInternal(D3D11_PRIMITIVE_TOPOLOGY primitiveTopology)
{
    if (primitiveTopology != mCurrentPrimitiveTopology)
    {
        mRenderer->getDeviceContext()->IASetPrimitiveTopology(primitiveTopology);
        mCurrentPrimitiveTopology = primitiveTopology;
        return true;
    }
    else
    {
        return false;
    }
}

void StateManager11::setDrawShaders(const d3d11::VertexShader *vertexShader,
                                    const d3d11::GeometryShader *geometryShader,
                                    const d3d11::PixelShader *pixelShader)
{
    setVertexShader(vertexShader);
    setGeometryShader(geometryShader);
    setPixelShader(pixelShader);
}

void StateManager11::setVertexShader(const d3d11::VertexShader *shader)
{
    ResourceSerial serial = shader ? shader->getSerial() : ResourceSerial(0);

    if (serial != mAppliedVertexShader)
    {
        ID3D11VertexShader *appliedShader = shader ? shader->get() : nullptr;
        mRenderer->getDeviceContext()->VSSetShader(appliedShader, nullptr, 0);
        mAppliedVertexShader = serial;
        invalidateShaders();
    }
}

void StateManager11::setGeometryShader(const d3d11::GeometryShader *shader)
{
    ResourceSerial serial = shader ? shader->getSerial() : ResourceSerial(0);

    if (serial != mAppliedGeometryShader)
    {
        ID3D11GeometryShader *appliedShader = shader ? shader->get() : nullptr;
        mRenderer->getDeviceContext()->GSSetShader(appliedShader, nullptr, 0);
        mAppliedGeometryShader = serial;
        invalidateShaders();
    }
}

void StateManager11::setPixelShader(const d3d11::PixelShader *shader)
{
    ResourceSerial serial = shader ? shader->getSerial() : ResourceSerial(0);

    if (serial != mAppliedPixelShader)
    {
        ID3D11PixelShader *appliedShader = shader ? shader->get() : nullptr;
        mRenderer->getDeviceContext()->PSSetShader(appliedShader, nullptr, 0);
        mAppliedPixelShader = serial;
        invalidateShaders();
    }
}

void StateManager11::setComputeShader(const d3d11::ComputeShader *shader)
{
    ResourceSerial serial = shader ? shader->getSerial() : ResourceSerial(0);

    if (serial != mAppliedComputeShader)
    {
        ID3D11ComputeShader *appliedShader = shader ? shader->get() : nullptr;
        mRenderer->getDeviceContext()->CSSetShader(appliedShader, nullptr, 0);
        mAppliedComputeShader = serial;
        // TODO(jmadill): Dirty bits for compute.
    }
}

void StateManager11::setVertexConstantBuffer(unsigned int slot, const d3d11::Buffer *buffer)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
    auto &currentSerial                = mCurrentConstantBufferVS[slot];

    mCurrentConstantBufferVSOffset[slot] = 0;
    mCurrentConstantBufferVSSize[slot]   = 0;

    if (buffer)
    {
        if (currentSerial != buffer->getSerial())
        {
            deviceContext->VSSetConstantBuffers(slot, 1, buffer->getPointer());
            currentSerial = buffer->getSerial();
            invalidateConstantBuffer(slot);
        }
    }
    else
    {
        if (!currentSerial.empty())
        {
            ID3D11Buffer *nullBuffer = nullptr;
            deviceContext->VSSetConstantBuffers(slot, 1, &nullBuffer);
            currentSerial.clear();
            invalidateConstantBuffer(slot);
        }
    }
}

void StateManager11::setPixelConstantBuffer(unsigned int slot, const d3d11::Buffer *buffer)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
    auto &currentSerial                = mCurrentConstantBufferPS[slot];

    mCurrentConstantBufferPSOffset[slot] = 0;
    mCurrentConstantBufferPSSize[slot]   = 0;

    if (buffer)
    {
        if (currentSerial != buffer->getSerial())
        {
            deviceContext->PSSetConstantBuffers(slot, 1, buffer->getPointer());
            currentSerial = buffer->getSerial();
            invalidateConstantBuffer(slot);
        }
    }
    else
    {
        if (!currentSerial.empty())
        {
            ID3D11Buffer *nullBuffer = nullptr;
            deviceContext->PSSetConstantBuffers(slot, 1, &nullBuffer);
            currentSerial.clear();
            invalidateConstantBuffer(slot);
        }
    }
}

void StateManager11::setDepthStencilState(const d3d11::DepthStencilState *depthStencilState,
                                          UINT stencilRef)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    if (depthStencilState)
    {
        deviceContext->OMSetDepthStencilState(depthStencilState->get(), stencilRef);
    }
    else
    {
        deviceContext->OMSetDepthStencilState(nullptr, stencilRef);
    }

    mInternalDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_STATE);
}

void StateManager11::setSimpleBlendState(const d3d11::BlendState *blendState)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    if (blendState)
    {
        deviceContext->OMSetBlendState(blendState->get(), nullptr, 0xFFFFFFFF);
    }
    else
    {
        deviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    }

    mInternalDirtyBits.set(DIRTY_BIT_BLEND_STATE);
}

void StateManager11::setRasterizerState(const d3d11::RasterizerState *rasterizerState)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    if (rasterizerState)
    {
        deviceContext->RSSetState(rasterizerState->get());
    }
    else
    {
        deviceContext->RSSetState(nullptr);
    }

    mInternalDirtyBits.set(DIRTY_BIT_RASTERIZER_STATE);
}

void StateManager11::setSimpleViewport(const gl::Extents &extents)
{
    setSimpleViewport(extents.width, extents.height);
}

void StateManager11::setSimpleViewport(int width, int height)
{
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width    = static_cast<FLOAT>(width);
    viewport.Height   = static_cast<FLOAT>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    mRenderer->getDeviceContext()->RSSetViewports(1, &viewport);
    mInternalDirtyBits.set(DIRTY_BIT_VIEWPORT_STATE);
}

void StateManager11::setSimplePixelTextureAndSampler(const d3d11::SharedSRV &srv,
                                                     const d3d11::SamplerState &samplerState)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    setShaderResourceInternal(gl::ShaderType::Fragment, 0, &srv);
    deviceContext->PSSetSamplers(0, 1, samplerState.getPointer());

    mInternalDirtyBits.set(DIRTY_BIT_TEXTURE_AND_SAMPLER_STATE);
    mForceSetPixelSamplerStates[0] = true;
}

void StateManager11::setSimpleScissorRect(const gl::Rectangle &glRect)
{
    D3D11_RECT scissorRect;
    scissorRect.left   = glRect.x;
    scissorRect.right  = glRect.x + glRect.width;
    scissorRect.top    = glRect.y;
    scissorRect.bottom = glRect.y + glRect.height;
    setScissorRectD3D(scissorRect);
}

void StateManager11::setScissorRectD3D(const D3D11_RECT &d3dRect)
{
    mRenderer->getDeviceContext()->RSSetScissorRects(1, &d3dRect);
    mInternalDirtyBits.set(DIRTY_BIT_SCISSOR_STATE);
}

// For each Direct3D sampler of either the pixel or vertex stage,
// looks up the corresponding OpenGL texture image unit and texture type,
// and sets the texture and its addressing/filtering state (or NULL when inactive).
// Sampler mapping needs to be up-to-date on the program object before this is called.
gl::Error StateManager11::applyTextures(const gl::Context *context, gl::ShaderType shaderType)
{
    ASSERT(shaderType != gl::ShaderType::Compute);
    const auto &glState    = context->getGLState();
    const auto &caps       = context->getCaps();
    ProgramD3D *programD3D = GetImplAs<ProgramD3D>(glState.getProgram());

    ASSERT(!programD3D->isSamplerMappingDirty());

    // TODO(jmadill): Use the Program's sampler bindings.
    const auto &completeTextures = glState.getCompleteTextureCache();

    unsigned int samplerRange = programD3D->getUsedSamplerRange(shaderType);
    for (unsigned int samplerIndex = 0; samplerIndex < samplerRange; samplerIndex++)
    {
        GLint textureUnit = programD3D->getSamplerMapping(shaderType, samplerIndex, caps);
        ASSERT(textureUnit != -1);
        gl::Texture *texture = completeTextures[textureUnit];

        // A nullptr texture indicates incomplete.
        if (texture)
        {
            gl::Sampler *samplerObject = glState.getSampler(textureUnit);

            const gl::SamplerState &samplerState =
                samplerObject ? samplerObject->getSamplerState() : texture->getSamplerState();

            ANGLE_TRY(setSamplerState(context, shaderType, samplerIndex, texture, samplerState));
            ANGLE_TRY(setTexture(context, shaderType, samplerIndex, texture));
        }
        else
        {
            gl::TextureType textureType =
                programD3D->getSamplerTextureType(shaderType, samplerIndex);

            // Texture is not sampler complete or it is in use by the framebuffer.  Bind the
            // incomplete texture.
            gl::Texture *incompleteTexture = nullptr;
            ANGLE_TRY(mRenderer->getIncompleteTexture(context, textureType, &incompleteTexture));
            ANGLE_TRY(setSamplerState(context, shaderType, samplerIndex, incompleteTexture,
                                      incompleteTexture->getSamplerState()));
            ANGLE_TRY(setTexture(context, shaderType, samplerIndex, incompleteTexture));
        }
    }

    // Set all the remaining textures to NULL
    size_t samplerCount = (shaderType == gl::ShaderType::Fragment)
                              ? caps.maxTextureImageUnits
                              : caps.maxVertexTextureImageUnits;
    ANGLE_TRY(clearSRVs(shaderType, samplerRange, samplerCount));

    return gl::NoError();
}

gl::Error StateManager11::syncTextures(const gl::Context *context)
{
    ANGLE_TRY(applyTextures(context, gl::ShaderType::Vertex));
    ANGLE_TRY(applyTextures(context, gl::ShaderType::Fragment));
    return gl::NoError();
}

gl::Error StateManager11::setSamplerState(const gl::Context *context,
                                          gl::ShaderType type,
                                          int index,
                                          gl::Texture *texture,
                                          const gl::SamplerState &samplerState)
{
#if !defined(NDEBUG)
    // Storage should exist, texture should be complete. Only verified in Debug.
    TextureD3D *textureD3D  = GetImplAs<TextureD3D>(texture);
    TextureStorage *storage = nullptr;
    ANGLE_TRY(textureD3D->getNativeTexture(context, &storage));
    ASSERT(storage);
#endif  // !defined(NDEBUG)

    auto *deviceContext = mRenderer->getDeviceContext();

    if (type == gl::ShaderType::Fragment)
    {
        ASSERT(static_cast<unsigned int>(index) < mRenderer->getNativeCaps().maxTextureImageUnits);

        if (mForceSetPixelSamplerStates[index] ||
            memcmp(&samplerState, &mCurPixelSamplerStates[index], sizeof(gl::SamplerState)) != 0)
        {
            ID3D11SamplerState *dxSamplerState = nullptr;
            ANGLE_TRY(mRenderer->getSamplerState(samplerState, &dxSamplerState));

            ASSERT(dxSamplerState != nullptr);
            deviceContext->PSSetSamplers(index, 1, &dxSamplerState);

            mCurPixelSamplerStates[index] = samplerState;
        }

        mForceSetPixelSamplerStates[index] = false;
    }
    else if (type == gl::ShaderType::Vertex)
    {
        ASSERT(static_cast<unsigned int>(index) <
               mRenderer->getNativeCaps().maxVertexTextureImageUnits);

        if (mForceSetVertexSamplerStates[index] ||
            memcmp(&samplerState, &mCurVertexSamplerStates[index], sizeof(gl::SamplerState)) != 0)
        {
            ID3D11SamplerState *dxSamplerState = nullptr;
            ANGLE_TRY(mRenderer->getSamplerState(samplerState, &dxSamplerState));

            ASSERT(dxSamplerState != nullptr);
            deviceContext->VSSetSamplers(index, 1, &dxSamplerState);

            mCurVertexSamplerStates[index] = samplerState;
        }

        mForceSetVertexSamplerStates[index] = false;
    }
    else if (type == gl::ShaderType::Compute)
    {
        ASSERT(static_cast<unsigned int>(index) <
               mRenderer->getNativeCaps().maxComputeTextureImageUnits);

        if (mForceSetComputeSamplerStates[index] ||
            memcmp(&samplerState, &mCurComputeSamplerStates[index], sizeof(gl::SamplerState)) != 0)
        {
            ID3D11SamplerState *dxSamplerState = nullptr;
            ANGLE_TRY(mRenderer->getSamplerState(samplerState, &dxSamplerState));

            ASSERT(dxSamplerState != nullptr);
            deviceContext->CSSetSamplers(index, 1, &dxSamplerState);

            mCurComputeSamplerStates[index] = samplerState;
        }

        mForceSetComputeSamplerStates[index] = false;
    }
    else
        UNREACHABLE();

    // Sampler metadata that's passed to shaders in uniforms is stored separately from rest of the
    // sampler state since having it in contiguous memory makes it possible to memcpy to a constant
    // buffer, and it doesn't affect the state set by PSSetSamplers/VSSetSamplers.
    mShaderConstants.onSamplerChange(type, index, *texture);

    return gl::NoError();
}

gl::Error StateManager11::setTexture(const gl::Context *context,
                                     gl::ShaderType type,
                                     int index,
                                     gl::Texture *texture)
{
    ASSERT(type != gl::ShaderType::Compute);
    const d3d11::SharedSRV *textureSRV = nullptr;

    if (texture)
    {
        TextureD3D *textureImpl = GetImplAs<TextureD3D>(texture);

        TextureStorage *texStorage = nullptr;
        ANGLE_TRY(textureImpl->getNativeTexture(context, &texStorage));

        // Texture should be complete and have a storage
        ASSERT(texStorage);

        TextureStorage11 *storage11 = GetAs<TextureStorage11>(texStorage);

        ANGLE_TRY(storage11->getSRVForSampler(context, texture->getTextureState(), &textureSRV));

        // If we get an invalid SRV here, something went wrong in the texture class and we're
        // unexpectedly missing the shader resource view.
        ASSERT(textureSRV->valid());

        textureImpl->resetDirty();
    }

    ASSERT(
        (type == gl::ShaderType::Fragment &&
         static_cast<unsigned int>(index) < mRenderer->getNativeCaps().maxTextureImageUnits) ||
        (type == gl::ShaderType::Vertex &&
         static_cast<unsigned int>(index) < mRenderer->getNativeCaps().maxVertexTextureImageUnits));

    setShaderResourceInternal(type, index, textureSRV);
    return gl::NoError();
}

gl::Error StateManager11::syncTexturesForCompute(const gl::Context *context)
{
    const auto &glState    = context->getGLState();
    const auto &caps       = context->getCaps();
    ProgramD3D *programD3D = GetImplAs<ProgramD3D>(glState.getProgram());

    // TODO(xinghua.cao@intel.com): Implement sampler feature in compute shader.
    unsigned int readonlyImageRange = programD3D->getUsedImageRange(gl::ShaderType::Compute, true);
    for (unsigned int readonlyImageIndex = 0; readonlyImageIndex < readonlyImageRange;
         readonlyImageIndex++)
    {
        GLint imageUnitIndex =
            programD3D->getImageMapping(gl::ShaderType::Compute, readonlyImageIndex, true, caps);
        ASSERT(imageUnitIndex != -1);
        const gl::ImageUnit &imageUnit = glState.getImageUnit(imageUnitIndex);
        ANGLE_TRY(setTextureForImage(context, gl::ShaderType::Compute, readonlyImageIndex, true,
                                     imageUnit));
    }

    unsigned int imageRange = programD3D->getUsedImageRange(gl::ShaderType::Compute, false);
    for (unsigned int imageIndex = 0; imageIndex < imageRange; imageIndex++)
    {
        GLint imageUnitIndex =
            programD3D->getImageMapping(gl::ShaderType::Compute, imageIndex, false, caps);
        ASSERT(imageUnitIndex != -1);
        const gl::ImageUnit &imageUnit = glState.getImageUnit(imageUnitIndex);
        ANGLE_TRY(
            setTextureForImage(context, gl::ShaderType::Compute, imageIndex, false, imageUnit));
    }

    // Set all the remaining textures to NULL
    size_t readonlyImageCount = caps.maxImageUnits;
    size_t imageCount         = caps.maxImageUnits;
    ANGLE_TRY(clearSRVs(gl::ShaderType::Compute, readonlyImageRange, readonlyImageCount));
    ANGLE_TRY(clearUAVs(gl::ShaderType::Compute, imageRange, imageCount));

    return gl::NoError();
}

gl::Error StateManager11::setTextureForImage(const gl::Context *context,
                                             gl::ShaderType type,
                                             int index,
                                             bool readonly,
                                             const gl::ImageUnit &imageUnit)
{
    TextureD3D *textureImpl = nullptr;
    if (!imageUnit.texture.get())
    {
        return gl::NoError();
    }

    textureImpl                = GetImplAs<TextureD3D>(imageUnit.texture.get());
    TextureStorage *texStorage = nullptr;
    ANGLE_TRY(textureImpl->getNativeTexture(context, &texStorage));
    // Texture should be complete and have a storage
    ASSERT(texStorage);
    TextureStorage11 *storage11 = GetAs<TextureStorage11>(texStorage);

    if (readonly)
    {
        const d3d11::SharedSRV *textureSRV = nullptr;
        ANGLE_TRY(storage11->getSRVForImage(context, imageUnit, &textureSRV));
        // If we get an invalid SRV here, something went wrong in the texture class and we're
        // unexpectedly missing the shader resource view.
        ASSERT(textureSRV->valid());
        ASSERT((static_cast<unsigned int>(index) < mRenderer->getNativeCaps().maxImageUnits));
        setShaderResourceInternal(type, index, textureSRV);
    }
    else
    {
        const d3d11::SharedUAV *textureUAV = nullptr;
        ANGLE_TRY(storage11->getUAVForImage(context, imageUnit, &textureUAV));
        // If we get an invalid UAV here, something went wrong in the texture class and we're
        // unexpectedly missing the unordered access view.
        ASSERT(textureUAV->valid());
        ASSERT((static_cast<unsigned int>(index) < mRenderer->getNativeCaps().maxImageUnits));
        setUnorderedAccessViewInternal(type, index, textureUAV);
    }

    textureImpl->resetDirty();
    return gl::NoError();
}

// Things that affect a program's dirtyness:
// 1. Directly changing the program executable -> triggered in StateManager11::syncState.
// 2. The vertex attribute layout              -> triggered in VertexArray11::syncState/signal.
// 3. The fragment shader's rendertargets      -> triggered in Framebuffer11::syncState/signal.
// 4. Enabling/disabling rasterizer discard.   -> triggered in StateManager11::syncState.
// 5. Enabling/disabling transform feedback.   -> checked in StateManager11::updateState.
// 6. An internal shader was used.             -> triggered in StateManager11::set*Shader.
// 7. Drawing with/without point sprites.      -> checked in StateManager11::updateState.
// TODO(jmadill): Use dirty bits for transform feedback.
gl::Error StateManager11::syncProgram(const gl::Context *context, GLenum drawMode)
{
    Context11 *context11 = GetImplAs<Context11>(context);
    ANGLE_TRY(context11->triggerDrawCallProgramRecompilation(context, drawMode));

    const auto &glState = context->getGLState();
    const auto *va11    = GetImplAs<VertexArray11>(glState.getVertexArray());
    auto *programD3D    = GetImplAs<ProgramD3D>(glState.getProgram());

    programD3D->updateCachedInputLayout(va11->getCurrentStateSerial(), glState);

    // Binaries must be compiled before the sync.
    ASSERT(programD3D->hasVertexExecutableForCachedInputLayout());
    ASSERT(programD3D->hasGeometryExecutableForPrimitiveType(drawMode));
    ASSERT(programD3D->hasPixelExecutableForCachedOutputLayout());

    ShaderExecutableD3D *vertexExe = nullptr;
    ANGLE_TRY(programD3D->getVertexExecutableForCachedInputLayout(&vertexExe, nullptr));

    ShaderExecutableD3D *pixelExe = nullptr;
    ANGLE_TRY(programD3D->getPixelExecutableForCachedOutputLayout(&pixelExe, nullptr));

    ShaderExecutableD3D *geometryExe = nullptr;
    ANGLE_TRY(programD3D->getGeometryExecutableForPrimitiveType(context, drawMode, &geometryExe,
                                                                nullptr));

    const d3d11::VertexShader *vertexShader =
        (vertexExe ? &GetAs<ShaderExecutable11>(vertexExe)->getVertexShader() : nullptr);

    // Skip pixel shader if we're doing rasterizer discard.
    const d3d11::PixelShader *pixelShader = nullptr;
    if (!glState.getRasterizerState().rasterizerDiscard)
    {
        pixelShader = (pixelExe ? &GetAs<ShaderExecutable11>(pixelExe)->getPixelShader() : nullptr);
    }

    const d3d11::GeometryShader *geometryShader = nullptr;
    if (glState.isTransformFeedbackActiveUnpaused())
    {
        geometryShader =
            (vertexExe ? &GetAs<ShaderExecutable11>(vertexExe)->getStreamOutShader() : nullptr);
    }
    else
    {
        geometryShader =
            (geometryExe ? &GetAs<ShaderExecutable11>(geometryExe)->getGeometryShader() : nullptr);
    }

    setDrawShaders(vertexShader, geometryShader, pixelShader);

    // Explicitly clear the shaders dirty bit.
    mInternalDirtyBits.reset(DIRTY_BIT_SHADERS);

    return gl::NoError();
}

gl::Error StateManager11::syncVertexBuffersAndInputLayout(const gl::Context *context,
                                                          const gl::DrawCallParams &drawCallParams)
{
    const gl::State &state             = context->getGLState();
    const gl::VertexArray *vertexArray = state.getVertexArray();
    VertexArray11 *vertexArray11       = GetImplAs<VertexArray11>(vertexArray);

    const auto &vertexArrayAttribs = vertexArray11->getTranslatedAttribs();
    gl::Program *program           = state.getProgram();

    // Sort the attributes according to ensure we re-use similar input layouts.
    AttribIndexArray sortedSemanticIndices;
    SortAttributesByLayout(program, vertexArrayAttribs, mCurrentValueAttribs,
                           &sortedSemanticIndices, &mCurrentAttributes);

    D3D_FEATURE_LEVEL featureLevel = mRenderer->getRenderer11DeviceCaps().featureLevel;

    // If we are using FL 9_3, make sure the first attribute is not instanced
    if (featureLevel <= D3D_FEATURE_LEVEL_9_3 && !mCurrentAttributes.empty())
    {
        if (mCurrentAttributes[0]->divisor > 0)
        {
            Optional<size_t> firstNonInstancedIndex = FindFirstNonInstanced(mCurrentAttributes);
            if (firstNonInstancedIndex.valid())
            {
                size_t index = firstNonInstancedIndex.value();
                std::swap(mCurrentAttributes[0], mCurrentAttributes[index]);
                std::swap(sortedSemanticIndices[0], sortedSemanticIndices[index]);
            }
        }
    }

    // Update the applied input layout by querying the cache.
    const d3d11::InputLayout *inputLayout = nullptr;
    ANGLE_TRY(mInputLayoutCache.getInputLayout(
        mRenderer, state, mCurrentAttributes, sortedSemanticIndices, drawCallParams, &inputLayout));
    setInputLayoutInternal(inputLayout);

    // Update the applied vertex buffers.
    ANGLE_TRY(applyVertexBuffers(context, drawCallParams));

    return gl::NoError();
}

gl::Error StateManager11::applyVertexBuffers(const gl::Context *context,
                                             const gl::DrawCallParams &drawCallParams)
{
    const gl::State &state = context->getGLState();
    gl::Program *program   = state.getProgram();
    ProgramD3D *programD3D = GetImplAs<ProgramD3D>(program);

    bool programUsesInstancedPointSprites =
        programD3D->usesPointSize() && programD3D->usesInstancedPointSpriteEmulation();
    bool instancedPointSpritesActive =
        programUsesInstancedPointSprites && (drawCallParams.mode() == GL_POINTS);

    // Note that if we use instance emulation, we reserve the first buffer slot.
    size_t reservedBuffers = GetReservedBufferCount(programUsesInstancedPointSprites);

    for (size_t attribIndex = 0; attribIndex < (gl::MAX_VERTEX_ATTRIBS - reservedBuffers);
         ++attribIndex)
    {
        ID3D11Buffer *buffer = nullptr;
        UINT vertexStride    = 0;
        UINT vertexOffset    = 0;

        if (attribIndex < mCurrentAttributes.size())
        {
            const TranslatedAttribute &attrib = *mCurrentAttributes[attribIndex];
            Buffer11 *bufferStorage = attrib.storage ? GetAs<Buffer11>(attrib.storage) : nullptr;

            // If indexed pointsprite emulation is active, then we need to take a less efficent code
            // path. Emulated indexed pointsprite rendering requires that the vertex buffers match
            // exactly to the indices passed by the caller.  This could expand or shrink the vertex
            // buffer depending on the number of points indicated by the index list or how many
            // duplicates are found on the index list.
            if (bufferStorage == nullptr)
            {
                ASSERT(attrib.vertexBuffer.get());
                buffer = GetAs<VertexBuffer11>(attrib.vertexBuffer.get())->getBuffer().get();
            }
            else if (instancedPointSpritesActive && drawCallParams.isDrawElements())
            {
                VertexArray11 *vao11 = GetImplAs<VertexArray11>(state.getVertexArray());
                ASSERT(vao11->isCachedIndexInfoValid());
                TranslatedIndexData indexInfo = vao11->getCachedIndexInfo();
                if (indexInfo.srcIndexData.srcBuffer != nullptr)
                {
                    const uint8_t *bufferData = nullptr;
                    ANGLE_TRY(indexInfo.srcIndexData.srcBuffer->getData(context, &bufferData));
                    ASSERT(bufferData != nullptr);

                    ptrdiff_t offset =
                        reinterpret_cast<ptrdiff_t>(indexInfo.srcIndexData.srcIndices);
                    indexInfo.srcIndexData.srcBuffer  = nullptr;
                    indexInfo.srcIndexData.srcIndices = bufferData + offset;
                }

                ANGLE_TRY_RESULT(
                    bufferStorage->getEmulatedIndexedBuffer(context, &indexInfo.srcIndexData,
                                                            attrib, drawCallParams.firstVertex()),
                    buffer);

                vao11->updateCachedIndexInfo(indexInfo);
            }
            else
            {
                ANGLE_TRY_RESULT(
                    bufferStorage->getBuffer(context, BUFFER_USAGE_VERTEX_OR_TRANSFORM_FEEDBACK),
                    buffer);
            }

            vertexStride = attrib.stride;
            ANGLE_TRY_RESULT(attrib.computeOffset(drawCallParams.firstVertex()), vertexOffset);
        }

        size_t bufferIndex = reservedBuffers + attribIndex;

        queueVertexBufferChange(bufferIndex, buffer, vertexStride, vertexOffset);
    }

    // Instanced PointSprite emulation requires two additional ID3D11Buffers. A vertex buffer needs
    // to be created and added to the list of current buffers, strides and offsets collections.
    // This buffer contains the vertices for a single PointSprite quad.
    // An index buffer also needs to be created and applied because rendering instanced data on
    // D3D11 FL9_3 requires DrawIndexedInstanced() to be used. Shaders that contain gl_PointSize and
    // used without the GL_POINTS rendering mode require a vertex buffer because some drivers cannot
    // handle missing vertex data and will TDR the system.
    if (programUsesInstancedPointSprites)
    {
        constexpr UINT kPointSpriteVertexStride = sizeof(float) * 5;

        if (!mPointSpriteVertexBuffer.valid())
        {
            static constexpr float kPointSpriteVertices[] = {
                // Position        | TexCoord
                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, /* v0 */
                -1.0f, 1.0f,  0.0f, 0.0f, 0.0f, /* v1 */
                1.0f,  1.0f,  0.0f, 1.0f, 0.0f, /* v2 */
                1.0f,  -1.0f, 0.0f, 1.0f, 1.0f, /* v3 */
                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, /* v4 */
                1.0f,  1.0f,  0.0f, 1.0f, 0.0f, /* v5 */
            };

            D3D11_SUBRESOURCE_DATA vertexBufferData = {kPointSpriteVertices, 0, 0};
            D3D11_BUFFER_DESC vertexBufferDesc;
            vertexBufferDesc.ByteWidth           = sizeof(kPointSpriteVertices);
            vertexBufferDesc.BindFlags           = D3D11_BIND_VERTEX_BUFFER;
            vertexBufferDesc.Usage               = D3D11_USAGE_IMMUTABLE;
            vertexBufferDesc.CPUAccessFlags      = 0;
            vertexBufferDesc.MiscFlags           = 0;
            vertexBufferDesc.StructureByteStride = 0;

            ANGLE_TRY(mRenderer->allocateResource(vertexBufferDesc, &vertexBufferData,
                                                  &mPointSpriteVertexBuffer));
        }

        // Set the stride to 0 if GL_POINTS mode is not being used to instruct the driver to avoid
        // indexing into the vertex buffer.
        UINT stride = instancedPointSpritesActive ? kPointSpriteVertexStride : 0;
        queueVertexBufferChange(0, mPointSpriteVertexBuffer.get(), stride, 0);

        if (!mPointSpriteIndexBuffer.valid())
        {
            // Create an index buffer and set it for pointsprite rendering
            static constexpr unsigned short kPointSpriteIndices[] = {
                0, 1, 2, 3, 4, 5,
            };

            D3D11_SUBRESOURCE_DATA indexBufferData = {kPointSpriteIndices, 0, 0};
            D3D11_BUFFER_DESC indexBufferDesc;
            indexBufferDesc.ByteWidth           = sizeof(kPointSpriteIndices);
            indexBufferDesc.BindFlags           = D3D11_BIND_INDEX_BUFFER;
            indexBufferDesc.Usage               = D3D11_USAGE_IMMUTABLE;
            indexBufferDesc.CPUAccessFlags      = 0;
            indexBufferDesc.MiscFlags           = 0;
            indexBufferDesc.StructureByteStride = 0;

            ANGLE_TRY(mRenderer->allocateResource(indexBufferDesc, &indexBufferData,
                                                  &mPointSpriteIndexBuffer));
        }

        if (instancedPointSpritesActive)
        {
            // The index buffer is applied here because Instanced PointSprite emulation uses the a
            // non-indexed rendering path in ANGLE (DrawArrays). This means that applyIndexBuffer()
            // on the renderer will not be called and setting this buffer here ensures that the
            // rendering path will contain the correct index buffers.
            syncIndexBuffer(mPointSpriteIndexBuffer.get(), DXGI_FORMAT_R16_UINT, 0);
        }
    }

    applyVertexBufferChanges();
    return gl::NoError();
}

gl::Error StateManager11::applyIndexBuffer(const gl::Context *context,
                                           const gl::DrawCallParams &params)
{
    const auto &glState  = context->getGLState();
    gl::VertexArray *vao = glState.getVertexArray();
    VertexArray11 *vao11 = GetImplAs<VertexArray11>(vao);

    if (!mIndexBufferIsDirty)
    {
        // No streaming or index buffer application necessary.
        return gl::NoError();
    }

    GLenum destElementType         = vao11->getCachedDestinationIndexType();
    gl::Buffer *elementArrayBuffer = vao->getElementArrayBuffer().get();

    TranslatedIndexData indexInfo;
    ANGLE_TRY(mIndexDataManager.prepareIndexData(context, params.type(), destElementType,
                                                 params.indexCount(), elementArrayBuffer,
                                                 params.indices(), &indexInfo));

    ID3D11Buffer *buffer = nullptr;
    DXGI_FORMAT bufferFormat =
        (indexInfo.indexType == GL_UNSIGNED_INT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;

    if (indexInfo.storage)
    {
        Buffer11 *storage = GetAs<Buffer11>(indexInfo.storage);
        ANGLE_TRY_RESULT(storage->getBuffer(context, BUFFER_USAGE_INDEX), buffer);
    }
    else
    {
        IndexBuffer11 *indexBuffer = GetAs<IndexBuffer11>(indexInfo.indexBuffer);
        buffer                     = indexBuffer->getBuffer().get();
    }

    // Track dirty indices in the index range cache.
    indexInfo.srcIndexData.srcIndicesChanged =
        syncIndexBuffer(buffer, bufferFormat, indexInfo.startOffset);

    mIndexBufferIsDirty = false;

    vao11->updateCachedIndexInfo(indexInfo);
    return gl::NoError();
}

void StateManager11::setIndexBuffer(ID3D11Buffer *buffer,
                                    DXGI_FORMAT indexFormat,
                                    unsigned int offset)
{
    if (syncIndexBuffer(buffer, indexFormat, offset))
    {
        invalidateIndexBuffer();
    }
}

bool StateManager11::syncIndexBuffer(ID3D11Buffer *buffer,
                                     DXGI_FORMAT indexFormat,
                                     unsigned int offset)
{
    if (buffer != mAppliedIB || indexFormat != mAppliedIBFormat || offset != mAppliedIBOffset)
    {
        mRenderer->getDeviceContext()->IASetIndexBuffer(buffer, indexFormat, offset);

        mAppliedIB       = buffer;
        mAppliedIBFormat = indexFormat;
        mAppliedIBOffset = offset;
        return true;
    }

    return false;
}

// Vertex buffer is invalidated outside this function.
gl::Error StateManager11::updateVertexOffsetsForPointSpritesEmulation(GLint startVertex,
                                                                      GLsizei emulatedInstanceId)
{
    size_t reservedBuffers = GetReservedBufferCount(true);
    for (size_t attribIndex = 0; attribIndex < mCurrentAttributes.size(); ++attribIndex)
    {
        const auto &attrib = *mCurrentAttributes[attribIndex];
        size_t bufferIndex = reservedBuffers + attribIndex;

        if (attrib.divisor > 0)
        {
            unsigned int offset = 0;
            ANGLE_TRY_RESULT(attrib.computeOffset(startVertex), offset);
            offset += (attrib.stride * (emulatedInstanceId / attrib.divisor));
            if (offset != mCurrentVertexOffsets[bufferIndex])
            {
                invalidateInputLayout();
                mDirtyVertexBufferRange.extend(static_cast<unsigned int>(bufferIndex));
                mCurrentVertexOffsets[bufferIndex] = offset;
            }
        }
    }

    applyVertexBufferChanges();
    return gl::NoError();
}

gl::Error StateManager11::generateSwizzle(const gl::Context *context, gl::Texture *texture)
{
    if (!texture)
    {
        return gl::NoError();
    }

    TextureD3D *textureD3D = GetImplAs<TextureD3D>(texture);
    ASSERT(textureD3D);

    TextureStorage *texStorage = nullptr;
    ANGLE_TRY(textureD3D->getNativeTexture(context, &texStorage));

    if (texStorage)
    {
        TextureStorage11 *storage11          = GetAs<TextureStorage11>(texStorage);
        const gl::TextureState &textureState = texture->getTextureState();
        ANGLE_TRY(storage11->generateSwizzles(context, textureState.getSwizzleState()));
    }

    return gl::NoError();
}

gl::Error StateManager11::generateSwizzlesForShader(const gl::Context *context, gl::ShaderType type)
{
    const auto &glState    = context->getGLState();
    ProgramD3D *programD3D = GetImplAs<ProgramD3D>(glState.getProgram());

    unsigned int samplerRange = programD3D->getUsedSamplerRange(type);

    for (unsigned int i = 0; i < samplerRange; i++)
    {
        gl::TextureType textureType = programD3D->getSamplerTextureType(type, i);
        GLint textureUnit  = programD3D->getSamplerMapping(type, i, context->getCaps());
        if (textureUnit != -1)
        {
            gl::Texture *texture = glState.getSamplerTexture(textureUnit, textureType);
            ASSERT(texture);
            if (texture->getTextureState().swizzleRequired())
            {
                ANGLE_TRY(generateSwizzle(context, texture));
            }
        }
    }

    return gl::NoError();
}

gl::Error StateManager11::generateSwizzles(const gl::Context *context)
{
    ANGLE_TRY(generateSwizzlesForShader(context, gl::ShaderType::Vertex));
    ANGLE_TRY(generateSwizzlesForShader(context, gl::ShaderType::Fragment));
    return gl::NoError();
}

gl::Error StateManager11::applyUniforms(ProgramD3D *programD3D)
{
    UniformStorage11 *vertexUniformStorage =
        GetAs<UniformStorage11>(&programD3D->getVertexUniformStorage());
    UniformStorage11 *fragmentUniformStorage =
        GetAs<UniformStorage11>(&programD3D->getFragmentUniformStorage());
    ASSERT(vertexUniformStorage);
    ASSERT(fragmentUniformStorage);

    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    const d3d11::Buffer *vertexConstantBuffer = nullptr;
    ANGLE_TRY(vertexUniformStorage->getConstantBuffer(mRenderer, &vertexConstantBuffer));
    const d3d11::Buffer *pixelConstantBuffer = nullptr;
    ANGLE_TRY(fragmentUniformStorage->getConstantBuffer(mRenderer, &pixelConstantBuffer));

    if (vertexUniformStorage->size() > 0 &&
        programD3D->areShaderUniformsDirty(gl::ShaderType::Vertex))
    {
        UpdateUniformBuffer(deviceContext, vertexUniformStorage, vertexConstantBuffer);
    }

    if (fragmentUniformStorage->size() > 0 &&
        programD3D->areShaderUniformsDirty(gl::ShaderType::Fragment))
    {
        UpdateUniformBuffer(deviceContext, fragmentUniformStorage, pixelConstantBuffer);
    }

    unsigned int slot = d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DEFAULT_UNIFORM_BLOCK;

    if (mCurrentConstantBufferVS[slot] != vertexConstantBuffer->getSerial())
    {
        deviceContext->VSSetConstantBuffers(slot, 1, vertexConstantBuffer->getPointer());
        mCurrentConstantBufferVS[slot]       = vertexConstantBuffer->getSerial();
        mCurrentConstantBufferVSOffset[slot] = 0;
        mCurrentConstantBufferVSSize[slot]   = 0;
    }

    if (mCurrentConstantBufferPS[slot] != pixelConstantBuffer->getSerial())
    {
        deviceContext->PSSetConstantBuffers(slot, 1, pixelConstantBuffer->getPointer());
        mCurrentConstantBufferPS[slot]       = pixelConstantBuffer->getSerial();
        mCurrentConstantBufferPSOffset[slot] = 0;
        mCurrentConstantBufferPSSize[slot]   = 0;
    }

    programD3D->markUniformsClean();

    return gl::NoError();
}

gl::Error StateManager11::applyDriverUniforms(const ProgramD3D &programD3D)
{
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    if (!mDriverConstantBufferVS.valid())
    {
        size_t requiredSize = mShaderConstants.getRequiredBufferSize(gl::ShaderType::Vertex);

        D3D11_BUFFER_DESC constantBufferDescription = {0};
        d3d11::InitConstantBufferDesc(&constantBufferDescription, requiredSize);
        ANGLE_TRY(mRenderer->allocateResource(constantBufferDescription, &mDriverConstantBufferVS));

        ID3D11Buffer *driverVSConstants = mDriverConstantBufferVS.get();
        deviceContext->VSSetConstantBuffers(d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DRIVER, 1,
                                            &driverVSConstants);
    }

    if (!mDriverConstantBufferPS.valid())
    {
        size_t requiredSize = mShaderConstants.getRequiredBufferSize(gl::ShaderType::Fragment);

        D3D11_BUFFER_DESC constantBufferDescription = {0};
        d3d11::InitConstantBufferDesc(&constantBufferDescription, requiredSize);
        ANGLE_TRY(mRenderer->allocateResource(constantBufferDescription, &mDriverConstantBufferPS));

        ID3D11Buffer *driverVSConstants = mDriverConstantBufferPS.get();
        deviceContext->PSSetConstantBuffers(d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DRIVER, 1,
                                            &driverVSConstants);
    }

    // Sampler metadata and driver constants need to coexist in the same constant buffer to conserve
    // constant buffer slots. We update both in the constant buffer if needed.
    ANGLE_TRY(mShaderConstants.updateBuffer(mRenderer, gl::ShaderType::Vertex, programD3D,
                                            mDriverConstantBufferVS));
    ANGLE_TRY(mShaderConstants.updateBuffer(mRenderer, gl::ShaderType::Fragment, programD3D,
                                            mDriverConstantBufferPS));

    // needed for the point sprite geometry shader
    // GSSetConstantBuffers triggers device removal on 9_3, so we should only call it for ES3.
    if (mRenderer->isES3Capable())
    {
        if (mCurrentGeometryConstantBuffer != mDriverConstantBufferPS.getSerial())
        {
            ASSERT(mDriverConstantBufferPS.valid());
            deviceContext->GSSetConstantBuffers(0, 1, mDriverConstantBufferPS.getPointer());
            mCurrentGeometryConstantBuffer = mDriverConstantBufferPS.getSerial();
        }
    }

    return gl::NoError();
}

gl::Error StateManager11::applyComputeUniforms(ProgramD3D *programD3D)
{
    UniformStorage11 *computeUniformStorage =
        GetAs<UniformStorage11>(&programD3D->getComputeUniformStorage());
    ASSERT(computeUniformStorage);

    const d3d11::Buffer *constantBuffer = nullptr;
    ANGLE_TRY(computeUniformStorage->getConstantBuffer(mRenderer, &constantBuffer));

    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    if (computeUniformStorage->size() > 0 &&
        programD3D->areShaderUniformsDirty(gl::ShaderType::Compute))
    {
        UpdateUniformBuffer(deviceContext, computeUniformStorage, constantBuffer);
        programD3D->markUniformsClean();
    }

    if (mCurrentComputeConstantBuffer != constantBuffer->getSerial())
    {
        deviceContext->CSSetConstantBuffers(
            d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DEFAULT_UNIFORM_BLOCK, 1,
            constantBuffer->getPointer());
        mCurrentComputeConstantBuffer = constantBuffer->getSerial();
    }

    if (!mDriverConstantBufferCS.valid())
    {
        size_t requiredSize = mShaderConstants.getRequiredBufferSize(gl::ShaderType::Compute);

        D3D11_BUFFER_DESC constantBufferDescription = {0};
        d3d11::InitConstantBufferDesc(&constantBufferDescription, requiredSize);
        ANGLE_TRY(mRenderer->allocateResource(constantBufferDescription, &mDriverConstantBufferCS));
        ID3D11Buffer *buffer = mDriverConstantBufferCS.get();
        deviceContext->CSSetConstantBuffers(d3d11::RESERVED_CONSTANT_BUFFER_SLOT_DRIVER, 1,
                                            &buffer);
    }

    ANGLE_TRY(mShaderConstants.updateBuffer(mRenderer, gl::ShaderType::Compute, *programD3D,
                                            mDriverConstantBufferCS));

    return gl::NoError();
}

gl::Error StateManager11::syncUniformBuffers(const gl::Context *context, ProgramD3D *programD3D)
{
    unsigned int reservedVertex   = mRenderer->getReservedVertexUniformBuffers();
    unsigned int reservedFragment = mRenderer->getReservedFragmentUniformBuffers();

    programD3D->updateUniformBufferCache(context->getCaps(), reservedVertex, reservedFragment);

    const auto &vertexUniformBuffers     = programD3D->getVertexUniformBufferCache();
    const auto &fragmentUniformBuffers   = programD3D->getFragmentUniformBufferCache();
    const auto &glState                  = context->getGLState();
    ID3D11DeviceContext *deviceContext   = mRenderer->getDeviceContext();
    ID3D11DeviceContext1 *deviceContext1 = mRenderer->getDeviceContext1IfSupported();

    mConstantBufferObserver.reset();

    for (size_t bufferIndex = 0; bufferIndex < vertexUniformBuffers.size(); bufferIndex++)
    {
        GLint binding = vertexUniformBuffers[bufferIndex];

        if (binding == -1)
        {
            continue;
        }

        const auto &uniformBuffer    = glState.getIndexedUniformBuffer(binding);
        GLintptr uniformBufferOffset = uniformBuffer.getOffset();
        GLsizeiptr uniformBufferSize = uniformBuffer.getSize();

        if (uniformBuffer.get() == nullptr)
        {
            continue;
        }

        Buffer11 *bufferStorage             = GetImplAs<Buffer11>(uniformBuffer.get());
        const d3d11::Buffer *constantBuffer = nullptr;
        UINT firstConstant                  = 0;
        UINT numConstants                   = 0;

        ANGLE_TRY(bufferStorage->getConstantBufferRange(context, uniformBufferOffset,
                                                        uniformBufferSize, &constantBuffer,
                                                        &firstConstant, &numConstants));

        ASSERT(constantBuffer);

        if (mCurrentConstantBufferVS[bufferIndex] == constantBuffer->getSerial() &&
            mCurrentConstantBufferVSOffset[bufferIndex] == uniformBufferOffset &&
            mCurrentConstantBufferVSSize[bufferIndex] == uniformBufferSize)
        {
            continue;
        }

        unsigned int appliedIndex = reservedVertex + static_cast<unsigned int>(bufferIndex);

        if (firstConstant != 0 && uniformBufferSize != 0)
        {
            ASSERT(numConstants != 0);
            deviceContext1->VSSetConstantBuffers1(appliedIndex, 1, constantBuffer->getPointer(),
                                                  &firstConstant, &numConstants);
        }
        else
        {
            deviceContext->VSSetConstantBuffers(appliedIndex, 1, constantBuffer->getPointer());
        }

        mCurrentConstantBufferVS[appliedIndex]       = constantBuffer->getSerial();
        mCurrentConstantBufferVSOffset[appliedIndex] = uniformBufferOffset;
        mCurrentConstantBufferVSSize[appliedIndex]   = uniformBufferSize;

        mConstantBufferObserver.bindVS(bufferIndex, bufferStorage);
    }

    for (size_t bufferIndex = 0; bufferIndex < fragmentUniformBuffers.size(); bufferIndex++)
    {
        GLint binding = fragmentUniformBuffers[bufferIndex];

        if (binding == -1)
        {
            continue;
        }

        const auto &uniformBuffer    = glState.getIndexedUniformBuffer(binding);
        GLintptr uniformBufferOffset = uniformBuffer.getOffset();
        GLsizeiptr uniformBufferSize = uniformBuffer.getSize();

        if (uniformBuffer.get() == nullptr)
        {
            continue;
        }

        Buffer11 *bufferStorage             = GetImplAs<Buffer11>(uniformBuffer.get());
        const d3d11::Buffer *constantBuffer = nullptr;
        UINT firstConstant                  = 0;
        UINT numConstants                   = 0;

        ANGLE_TRY(bufferStorage->getConstantBufferRange(context, uniformBufferOffset,
                                                        uniformBufferSize, &constantBuffer,
                                                        &firstConstant, &numConstants));

        ASSERT(constantBuffer);

        if (mCurrentConstantBufferPS[bufferIndex] == constantBuffer->getSerial() &&
            mCurrentConstantBufferPSOffset[bufferIndex] == uniformBufferOffset &&
            mCurrentConstantBufferPSSize[bufferIndex] == uniformBufferSize)
        {
            continue;
        }

        unsigned int appliedIndex = reservedFragment + static_cast<unsigned int>(bufferIndex);

        if (firstConstant != 0 && uniformBufferSize != 0)
        {
            deviceContext1->PSSetConstantBuffers1(appliedIndex, 1, constantBuffer->getPointer(),
                                                  &firstConstant, &numConstants);
        }
        else
        {
            deviceContext->PSSetConstantBuffers(appliedIndex, 1, constantBuffer->getPointer());
        }

        mCurrentConstantBufferPS[appliedIndex]       = constantBuffer->getSerial();
        mCurrentConstantBufferPSOffset[appliedIndex] = uniformBufferOffset;
        mCurrentConstantBufferPSSize[appliedIndex]   = uniformBufferSize;

        mConstantBufferObserver.bindPS(bufferIndex, bufferStorage);
    }

    return gl::NoError();
}

gl::Error StateManager11::syncTransformFeedbackBuffers(const gl::Context *context)
{
    const auto &glState = context->getGLState();

    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    // If transform feedback is not active, unbind all buffers
    if (!glState.isTransformFeedbackActiveUnpaused())
    {
        if (mAppliedTFSerial != mEmptySerial)
        {
            deviceContext->SOSetTargets(0, nullptr, nullptr);
            mAppliedTFSerial = mEmptySerial;
        }
        return gl::NoError();
    }

    gl::TransformFeedback *transformFeedback = glState.getCurrentTransformFeedback();
    TransformFeedback11 *tf11                = GetImplAs<TransformFeedback11>(transformFeedback);
    if (mAppliedTFSerial == tf11->getSerial() && !tf11->isDirty())
    {
        return gl::NoError();
    }

    const std::vector<ID3D11Buffer *> *soBuffers = nullptr;
    ANGLE_TRY_RESULT(tf11->getSOBuffers(context), soBuffers);
    const std::vector<UINT> &soOffsets = tf11->getSOBufferOffsets();

    deviceContext->SOSetTargets(tf11->getNumSOBuffers(), soBuffers->data(), soOffsets.data());

    mAppliedTFSerial = tf11->getSerial();
    tf11->onApply();

    return gl::NoError();
}

// ConstantBufferObserver implementation.
StateManager11::ConstantBufferObserver::ConstantBufferObserver()
{
    for (size_t vsIndex = 0; vsIndex < gl::IMPLEMENTATION_MAX_VERTEX_SHADER_UNIFORM_BUFFERS;
         ++vsIndex)
    {
        mBindingsVS.emplace_back(this, vsIndex);
    }

    for (size_t fsIndex = 0; fsIndex < gl::IMPLEMENTATION_MAX_FRAGMENT_SHADER_UNIFORM_BUFFERS;
         ++fsIndex)
    {
        mBindingsPS.emplace_back(this, fsIndex);
    }
}

StateManager11::ConstantBufferObserver::~ConstantBufferObserver()
{
}

void StateManager11::ConstantBufferObserver::onSubjectStateChange(const gl::Context *context,
                                                                  angle::SubjectIndex index,
                                                                  angle::SubjectMessage message)
{
    if (message == angle::SubjectMessage::STORAGE_CHANGED)
    {
        StateManager11 *stateManager =
            GetImplAs<Context11>(context)->getRenderer()->getStateManager();
        stateManager->invalidateProgramUniformBuffers();
    }
}

void StateManager11::ConstantBufferObserver::bindVS(size_t index, Buffer11 *buffer)
{
    ASSERT(buffer);
    ASSERT(index < mBindingsVS.size());
    mBindingsVS[index].bind(buffer);
}

void StateManager11::ConstantBufferObserver::bindPS(size_t index, Buffer11 *buffer)
{
    ASSERT(buffer);
    ASSERT(index < mBindingsPS.size());
    mBindingsPS[index].bind(buffer);
}

void StateManager11::ConstantBufferObserver::reset()
{
    for (angle::ObserverBinding &vsBinding : mBindingsVS)
    {
        vsBinding.bind(nullptr);
    }

    for (angle::ObserverBinding &psBinding : mBindingsPS)
    {
        psBinding.bind(nullptr);
    }
}

void StateManager11::syncPrimitiveTopology(const gl::State &glState,
                                           ProgramD3D *programD3D,
                                           GLenum currentDrawMode)
{
    D3D11_PRIMITIVE_TOPOLOGY primitiveTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    switch (currentDrawMode)
    {
        case GL_POINTS:
        {
            bool usesPointSize = programD3D->usesPointSize();

            // ProgramBinary assumes non-point rendering if gl_PointSize isn't written,
            // which affects varying interpolation. Since the value of gl_PointSize is
            // undefined when not written, just skip drawing to avoid unexpected results.
            if (!usesPointSize && !glState.isTransformFeedbackActiveUnpaused())
            {
                // Notify developers of risking undefined behavior.
                WARN() << "Point rendering without writing to gl_PointSize.";
                mCurrentMinimumDrawCount = std::numeric_limits<GLsizei>::max();
                return;
            }

            // If instanced pointsprites are enabled and the shader uses gl_PointSize, the topology
            // must be D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST.
            if (usesPointSize && mRenderer->getWorkarounds().useInstancedPointSpriteEmulation)
            {
                primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }
            else
            {
                primitiveTopology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            }
            mCurrentMinimumDrawCount = 1;
            break;
        }
        case GL_LINES:
            primitiveTopology        = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            mCurrentMinimumDrawCount = 2;
            break;
        case GL_LINE_LOOP:
            primitiveTopology        = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            mCurrentMinimumDrawCount = 2;
            break;
        case GL_LINE_STRIP:
            primitiveTopology        = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            mCurrentMinimumDrawCount = 2;
            break;
        case GL_TRIANGLES:
            primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            mCurrentMinimumDrawCount =
                CullsEverything(glState) ? std::numeric_limits<GLsizei>::max() : 3;
            break;
        case GL_TRIANGLE_STRIP:
            primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            mCurrentMinimumDrawCount =
                CullsEverything(glState) ? std::numeric_limits<GLsizei>::max() : 3;
            break;
        // emulate fans via rewriting index buffer
        case GL_TRIANGLE_FAN:
            primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            mCurrentMinimumDrawCount =
                CullsEverything(glState) ? std::numeric_limits<GLsizei>::max() : 3;
            break;
        default:
            UNREACHABLE();
            break;
    }

    setPrimitiveTopologyInternal(primitiveTopology);
}

}  // namespace rx
