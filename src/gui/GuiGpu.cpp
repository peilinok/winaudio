#include "GuiGpu.h"

namespace wa {
namespace {
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
}  // namespace

void setGuiGpu(ID3D11Device* device, ID3D11DeviceContext* context) {
    g_device = device;
    g_context = context;
}

ID3D11Device* guiDevice() { return g_device; }
ID3D11DeviceContext* guiDeviceContext() { return g_context; }

}  // namespace wa