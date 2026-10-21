#include "HeatmapTexture.h"
#include "GuiGpu.h"
#include <d3d11.h>

namespace wa {

void HeatmapTexture::release() {
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

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(cols);
    desc.Height = static_cast<UINT>(rows);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex_);
    if (FAILED(hr) || !tex_) {
        release();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(tex_, &srvDesc, &srv_);
    if (FAILED(hr) || !srv_) {
        release();
        return false;
    }

    rows_ = rows;
    cols_ = cols;
    rgba_.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0u);
    return true;
}

void HeatmapTexture::upload() {
    ID3D11DeviceContext* ctx = guiDeviceContext();
    if (!ctx || !tex_ || rgba_.empty()) return;
    ctx->UpdateSubresource(tex_, 0, nullptr, rgba_.data(),
                           static_cast<UINT>(cols_ * sizeof(uint32_t)), 0);
}

bool HeatmapTexture::ensure(const float* values, int rows, int cols, uint64_t revision,
                            float scaleMin, float scaleMax, const uint32_t* lut256) {
    if (!values || !lut256 || rows <= 0 || cols <= 0) return false;

    if (srv_ && rows_ == rows && cols_ == cols && haveRevision_ && revision_ == revision)
        return true;  // dirty skip: unchanged spectrogram

    if (!srv_ || rows_ != rows || cols_ != cols) {
        if (!createTexture(rows, cols)) return false;
    }

    fillHeatmapRgba(values, rows, cols, scaleMin, scaleMax, lut256, rgba_.data());
    upload();
    revision_ = revision;
    haveRevision_ = true;
    return true;
}

}  // namespace wa