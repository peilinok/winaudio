#include "HeatmapTexture.h"
#include "GuiGpu.h"
#include "FrameProbe.h"
#include "Log.h"
#include <d3d11.h>
#include <algorithm>

namespace {
long createHeatmapResources(ID3D11Device* device, int rows, int cols,
                           ID3D11Texture2D** texture, ID3D11ShaderResourceView** view) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(cols);
    desc.Height = static_cast<UINT>(rows);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture);
    WA_LOG(wa::log::Level::Debug, "Heatmap", "CreateTexture2D",
           "rows=" + std::to_string(rows) + " cols=" + std::to_string(cols), wa::log::hrName(hr));
    if (FAILED(hr)) return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(*texture, &srvDesc, view);
    WA_LOG(wa::log::Level::Debug, "Heatmap", "CreateShaderResourceView", "", wa::log::hrName(hr));
    return hr;
}
}

namespace wa {

void HeatmapTexture::release() {
    if (resourceCounted_ && probe_)
        probe_->addWork("heatmap-release", 1, source_, direction_, channel_);
    resourceCounted_ = false;
    if (srv_ || tex_) WA_LOG(log::Level::Debug, "Heatmap", "release", "", "");
    if (srv_) {
        srv_->Release();
        srv_ = nullptr;
    }
    if (tex_) {
        tex_->Release();
        tex_ = nullptr;
    }
    rgba_.clear();
    rows_ = cols_ = 0;
    device_ = nullptr;
    revision_ = 0;
    haveRevision_ = false;
}

ImTextureID HeatmapTexture::textureId() const {
    return reinterpret_cast<ImTextureID>(srv_);
}

bool HeatmapTexture::createTexture(int rows, int cols) {
    release();
    ID3D11Device* device = guiDevice();
    if (!device || rows <= 0 || cols <= 0) return false;
    const auto create = creator_ ? creator_ : createHeatmapResources;
    const auto hr = create(device, rows, cols, &tex_, &srv_);
    if (FAILED(hr) || !tex_ || !srv_) {
        WA_LOG(log::Level::Warn, "Heatmap", "createTexture", "", log::hrName(hr));
        release();
        return false;
    }

    rows_ = rows;
    cols_ = cols;
    device_ = device;
    rgba_.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0u);
    return true;
}

void HeatmapTexture::upload() {
    ID3D11DeviceContext* ctx = guiDeviceContext();
    if (!ctx || !tex_ || rgba_.empty()) return;
    ctx->UpdateSubresource(tex_, 0, nullptr, rgba_.data(),
                           static_cast<UINT>(cols_ * sizeof(uint32_t)), 0);
    if (log::shouldLog(log::Level::Trace))
        log::emitTrace("Heatmap", "UpdateSubresource", static_cast<unsigned>(rgba_.size()), 0, S_OK);
}

bool HeatmapTexture::ensure(const float* values, int rows, int cols, uint64_t revision,
                            float scaleMin, float scaleMax, const uint32_t* lut256,
                            FrameProbe* probe, uint64_t source, int direction, int channel) {
    if (probe) {
        probe_ = probe;
        source_ = source;
        direction_ = direction;
        channel_ = channel;
    }
    const auto note = [&](const char* metric, uint64_t value) {
        if (probe) probe->addWork(metric, value, source, direction, channel);
    };
    if (!values || !lut256 || rows <= 0 || cols <= 0 || !guiDevice() || !guiDeviceContext())
        return false;

    if (srv_ && device_ == guiDevice() && rows_ == rows && cols_ == cols && haveRevision_ && revision_ == revision &&
        scaleMin_ == scaleMin && scaleMax_ == scaleMax && std::equal(lut_.begin(), lut_.end(), lut256)) {
        note("heatmap-skip", 1);
        return true;  // dirty skip: unchanged spectrogram
    }

    if (!srv_ || device_ != guiDevice() || rows_ != rows || cols_ != cols) {
        if (!createTexture(rows, cols)) return false;
        note("heatmap-create", 1);
        resourceCounted_ = true;
    }

    {
        FrameBracket fill(probe, FrameSlice::HeatmapFill);
        fillHeatmapRgba(values, rows, cols, scaleMin, scaleMax, lut256, rgba_.data());
    }
    note("heatmap-pixels", static_cast<uint64_t>(rows) * static_cast<uint64_t>(cols));
    {
        FrameBracket submit(probe, FrameSlice::HeatmapUpload);
        if (guiDeviceContext() && tex_ && !rgba_.empty()) {
            upload();
            note("heatmap-uploads", 1);
            note("heatmap-upload-bytes", rgba_.size() * sizeof(uint32_t));
        }
    }
    revision_ = revision;
    scaleMin_ = scaleMin;
    scaleMax_ = scaleMax;
    std::copy_n(lut256, lut_.size(), lut_.begin());
    haveRevision_ = true;
    return true;
}

}  // namespace wa
