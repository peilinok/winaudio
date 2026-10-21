#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace wa {

// DX11 device pointers owned by main.cpp; set once after CreateDeviceD3D.
void setGuiGpu(ID3D11Device* device, ID3D11DeviceContext* context);
ID3D11Device* guiDevice();
ID3D11DeviceContext* guiDeviceContext();

}  // namespace wa