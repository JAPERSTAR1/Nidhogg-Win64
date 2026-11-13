#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace ps4::ui {

void InitOverlay(ID3D11Device* device);
void ShutdownOverlay();
void FinishOnlineActions();

void OpenOverlayTab(int tabIndex);  // 0=Graphics,1=Display,2=Audio,3=Controls,4=Online,5=General
bool IsOverlayOpen();
uint32_t FilterGameMenuInput(int player,uint32_t buttons,uint64_t frame,uint8_t& lx,uint8_t& ly);
bool PausesSimulation();
void BeginMatchTransition();
bool OwnsScreen();
void RecordSimulationFrame();
bool OnWindowMessage(void* hwnd, uint32_t msg, uint64_t wp, int64_t lp);

// Records frame timing and draws HUD + Settings/Online UI onto the currently bound RTV.
void RenderOverlay(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
                   uint32_t targetWidth, uint32_t targetHeight, bool isNewSimFrame);

}  // namespace ps4::ui
