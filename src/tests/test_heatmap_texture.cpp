#include <gtest/gtest.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include "GuiGpu.h"
#include "HeatmapTexture.h"
#include "SpectrogramVisual.h"
#include "FrameProbe.h"

using Microsoft::WRL::ComPtr;

namespace {
class HeatmapTextureTest : public testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<uint32_t, 256> lut{};
    void SetUp() override {
        ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
        for (size_t i = 0; i < lut.size(); ++i) lut[i] = 0xff000000u | static_cast<uint32_t>(i);
        wa::setGuiGpu(device.Get(), context.Get());
    }
    void TearDown() override { wa::setGuiGpu(nullptr, nullptr); }
    void expectPixels(ImTextureID id, uint32_t expected) {
        auto* view = reinterpret_cast<ID3D11ShaderResourceView*>(id);
        ASSERT_NE(view, nullptr);
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        ASSERT_TRUE(SUCCEEDED(resource.As(&texture)));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < desc.Width; ++x) EXPECT_EQ(row[x], expected);
        }
        context->Unmap(staging.Get(), 0);
    }
};
}

TEST_F(HeatmapTextureTest, MissingContextDoesNotCommitAndRecoveryUploadsTheSameRevision) {
    wa::HeatmapTexture texture;
    const float values[] = {0, 0, 0, 0};
    wa::setGuiGpu(nullptr, nullptr);
    EXPECT_FALSE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
    wa::setGuiGpu(device.Get(), nullptr);
    EXPECT_FALSE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
    wa::setGuiGpu(device.Get(), context.Get());
    ASSERT_TRUE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
    expectPixels(texture.textureId(), lut[255]);
}

TEST_F(HeatmapTextureTest, ColorMappingContentInvalidatesAnUnchangedHistory) {
    wa::HeatmapTexture texture;
    const float values[] = {0, 0, 0, 0};
    ASSERT_TRUE(texture.ensure(values, 2, 2, 1, -96, 0, lut.data()));
    expectPixels(texture.textureId(), lut[255]);
    lut[255] = 0xff00ff00u;
    ASSERT_TRUE(texture.ensure(values, 2, 2, 1, -96, 0, lut.data()));
    expectPixels(texture.textureId(), 0xff00ff00u);
    ASSERT_TRUE(texture.ensure(values, 2, 2, 1, 0, 96, lut.data()));
    expectPixels(texture.textureId(), lut[0]);
}

TEST_F(HeatmapTextureTest, DeviceReplacementRequiresAResourceFromTheNewDevice) {
    wa::HeatmapTexture texture;
    const float values[] = {0, 0, 0, 0};
    ASSERT_TRUE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
    ComPtr<ID3D11Device> nextDevice;
    ComPtr<ID3D11DeviceContext> nextContext;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &nextDevice, nullptr, &nextContext)));
    wa::setGuiGpu(nextDevice.Get(), nextContext.Get());
    ASSERT_TRUE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
    ComPtr<ID3D11Device> owner;
    reinterpret_cast<ID3D11ShaderResourceView*>(texture.textureId())->GetDevice(&owner);
    EXPECT_EQ(owner.Get(), nextDevice.Get());
}

namespace {
std::unique_ptr<wa::Spectrogram> history(float floor, int rows = 2, int cols = 2) {
    return std::make_unique<wa::Spectrogram>(rows, cols, 20, 24000, 48000, floor);
}
uint64_t workTotal(const wa::FrameProbe& probe, const char* name) {
    uint64_t sum = 0;
    char line[512];
    for (size_t i = 0; i < probe.workCount(); ++i) {
        if (probe.formatWork(i, line, sizeof(line)) > 0 &&
            std::string(line).find(std::string("metric=") + name + " ") != std::string::npos) {
            const auto* value = strstr(line, "value=");
            if (value) sum += strtoull(value + 6, nullptr, 10);
        }
    }
    return sum;
}
}

TEST_F(HeatmapTextureTest, PairedHistoriesKeepTheirQueuedImagesUntilConsumption) {
    wa::SpectrogramVisual a, b;
    a.reset(history(0));
    b.reset(history(-96));
    ASSERT_EQ(a.get()->revision(), b.get()->revision());
    auto* first = a.ensure(lut.data());
    ASSERT_NE(first, nullptr);
    const auto queuedA = first->textureId();
    auto* second = b.ensure(lut.data());
    ASSERT_NE(second, nullptr);
    const auto queuedB = second->textureId();
    // Read only after both sources have been submitted, as RenderDrawData does.
    expectPixels(queuedA, lut[255]);
    expectPixels(queuedB, lut[0]);
    b.get()->pushColumn(std::vector<float>(3, -48));
    b.get()->pushColumn(std::vector<float>(3, -48));
    ASSERT_NE(b.ensure(lut.data()), nullptr);
    expectPixels(queuedA, lut[255]);
    expectPixels(queuedB, lut[127]);
}

TEST_F(HeatmapTextureTest, ReplacementClearResizeAndReleasePreserveSiblingContent) {
    std::vector<std::unique_ptr<wa::SpectrogramVisual>> visuals;
    for (float floor : {0.f, -96.f, 0.f}) {
        auto visual = std::make_unique<wa::SpectrogramVisual>();
        visual->reset(history(floor));
        ASSERT_NE(visual->ensure(lut.data()), nullptr);
        visuals.push_back(std::move(visual));
    }
    const auto sibling = visuals[2]->ensure(lut.data())->textureId();
    visuals.erase(visuals.begin() + 1);
    visuals.reserve(32);
    expectPixels(sibling, lut[255]);
    auto& a = *visuals.front();
    a.reset(history(-96));  // identical dimensions and revision, different content
    ASSERT_NE(a.ensure(lut.data()), nullptr);
    expectPixels(a.ensure(lut.data())->textureId(), lut[0]);
    a.get()->pushColumn(std::vector<float>(3, 0));
    a.get()->pushColumn(std::vector<float>(3, 0));
    expectPixels(a.ensure(lut.data())->textureId(), lut[255]);
    a.get()->clear();
    expectPixels(a.ensure(lut.data())->textureId(), lut[0]);
    a.reset(history(-96, 3, 4));
    expectPixels(a.ensure(lut.data())->textureId(), lut[0]);
    a.reset();
    EXPECT_EQ(a.ensure(lut.data()), nullptr);
    expectPixels(sibling, lut[255]);
    // The shutdown path applies releaseGpu to visible and hidden owners alike.
    for (auto& visual : visuals) visual->releaseGpu();
    wa::setGuiGpu(nullptr, nullptr);
    EXPECT_EQ(visuals[1]->ensure(lut.data()), nullptr);
    wa::setGuiGpu(device.Get(), context.Get());
    expectPixels(visuals[1]->ensure(lut.data())->textureId(), lut[255]);
}

TEST_F(HeatmapTextureTest, OffscreenHopsUploadOnlyTheFinalHistoryAndThenSkip) {
    wa::SpectrogramVisual visual;
    visual.reset(history(-96));
    // No GPU resource is requested during the CPU-only offscreen updates.
    for (int i = 0; i < 8; ++i) visual.get()->pushColumn(std::vector<float>(3, 0));
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.beginFrame();
    ASSERT_NE(visual.ensure(lut.data(), &probe), nullptr);
    EXPECT_EQ(workTotal(probe, "heatmap-uploads"), 1u);
    EXPECT_EQ(workTotal(probe, "heatmap-pixels"), 4u);
    expectPixels(visual.ensure(lut.data())->textureId(), lut[255]);
    probe.endFrame();
    probe.beginFrame();
    ASSERT_NE(visual.ensure(lut.data(), &probe), nullptr);
    EXPECT_EQ(workTotal(probe, "heatmap-uploads"), 0u);
    EXPECT_EQ(workTotal(probe, "heatmap-pixels"), 0u);
    EXPECT_EQ(workTotal(probe, "heatmap-skip"), 1u);
    probe.endFrame();
}

namespace {
int createFailure = 0;
long controlledCreate(ID3D11Device* device, int rows, int cols,
                      ID3D11Texture2D** texture, ID3D11ShaderResourceView** view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = createFailure == 1 ? 0u : static_cast<UINT>(cols);
    desc.Height = static_cast<UINT>(rows);
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture);
    if (FAILED(hr)) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = createFailure == 2 ? DXGI_FORMAT_D32_FLOAT : desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    return device->CreateShaderResourceView(*texture, &srv, view);
}
}

TEST_F(HeatmapTextureTest, FailedTextureAndViewCreationRetryWithoutCommitting) {
    for (int failure : {1, 2}) {
        wa::HeatmapTexture texture(controlledCreate);
        const float values[] = {0, 0, 0, 0};
        createFailure = failure;
        ASSERT_FALSE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
        EXPECT_FALSE(texture.ready());
        createFailure = 0;
        ASSERT_TRUE(texture.ensure(values, 2, 2, 7, -96, 0, lut.data()));
        expectPixels(texture.textureId(), lut[255]);
    }
}

TEST_F(HeatmapTextureTest, ReleasingAHiddenOwnerDropsItsViewReference) {
    wa::SpectrogramVisual hidden;
    hidden.reset(history(0));
    auto* texture = hidden.ensure(lut.data());
    ASSERT_NE(texture, nullptr);
    ComPtr<ID3D11ShaderResourceView> observed =
        reinterpret_cast<ID3D11ShaderResourceView*>(texture->textureId());
    hidden.releaseGpu();
    // COM's diagnostic reference count observes the system resource boundary.
    // This external reference is now the sole owner, even while the CPU history remains.
    EXPECT_EQ(observed->AddRef(), 2u);
    observed->Release();
    EXPECT_NE(hidden.get(), nullptr);
    expectPixels(reinterpret_cast<ImTextureID>(observed.Get()), lut[255]);
}

TEST_F(HeatmapTextureTest, ValidPairedSubmissionsHaveNoDebugLayerErrorsWhenAvailable) {
    ComPtr<ID3D11Device> debugDevice;
    ComPtr<ID3D11DeviceContext> debugContext;
    const HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
        &debugDevice, nullptr, &debugContext);
    if (created == DXGI_ERROR_SDK_COMPONENT_MISSING) GTEST_SKIP() << "DX11 debug layer is not installed";
    ASSERT_TRUE(SUCCEEDED(created));
    ComPtr<ID3D11InfoQueue> messages;
    ASSERT_TRUE(SUCCEEDED(debugDevice.As(&messages)));
    wa::setGuiGpu(debugDevice.Get(), debugContext.Get());
    {
        wa::SpectrogramVisual a, b;
        a.reset(history(0));
        b.reset(history(-96));
        ASSERT_NE(a.ensure(lut.data()), nullptr);
        ASSERT_NE(b.ensure(lut.data()), nullptr);
        a.releaseGpu();
        b.releaseGpu();
    }
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T bytes = 0;
        ASSERT_TRUE(SUCCEEDED(messages->GetMessage(i, nullptr, &bytes)));
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        ASSERT_TRUE(SUCCEEDED(messages->GetMessage(i, message, &bytes)));
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
        EXPECT_NE(message->Severity, D3D11_MESSAGE_SEVERITY_ERROR) << message->pDescription;
    }
}
