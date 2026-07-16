#include "vst3_ember_editor.h"
#include "vst3_ember_processor.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "AngleKnob.hpp"
#include "retro_neon_theme.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace EmberVst3 {
namespace {
constexpr wchar_t kClassName[] = L"EmberVst3ImGuiEditor";
ATOM registerClass() {
    static ATOM atom = [] {
        WNDCLASSEXW wc {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&EmberVst3Editor::windowProc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc);
    }();
    return atom;
}
template<class T> void releaseCom(T*& value) { if (value) { value->Release(); value=nullptr; } }
}

EmberVst3Editor::EmberVst3Editor(EmberProcessor& p) : processor_(p) {}
EmberVst3Editor::~EmberVst3Editor() { removed(); if (frame_) frame_->release(); }
Steinberg::tresult PLUGIN_API EmberVst3Editor::queryInterface(const Steinberg::TUID iid, void** obj) {
    if (!obj) return Steinberg::kInvalidArgument;
    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugView::iid)) {
        *obj=static_cast<Steinberg::IPlugView*>(this); addRef(); return Steinberg::kResultOk;
    }
    *obj=nullptr; return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API EmberVst3Editor::addRef() { return ++refs_; }
Steinberg::uint32 PLUGIN_API EmberVst3Editor::release() { const auto n=--refs_; if(!n) delete this; return n; }
Steinberg::tresult PLUGIN_API EmberVst3Editor::isPlatformTypeSupported(Steinberg::FIDString type) {
    return type && std::strcmp(type, Steinberg::kPlatformTypeHWND)==0 ? Steinberg::kResultTrue : Steinberg::kResultFalse;
}
Steinberg::tresult PLUGIN_API EmberVst3Editor::attached(void* parent, Steinberg::FIDString type) {
    if (hwnd_ || !parent || isPlatformTypeSupported(type)!=Steinberg::kResultTrue || !registerClass()) return Steinberg::kResultFalse;
    hwnd_=CreateWindowExW(0,kClassName,L"Ember",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
        0,0,rect_.getWidth(),rect_.getHeight(),static_cast<HWND>(parent),nullptr,GetModuleHandleW(nullptr),this);
    if (!hwnd_ || !createDevice()) { removed(); return Steinberg::kResultFalse; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    themes::retro_neon::ApplyTheme();
    if (!ImGui_ImplWin32_Init(hwnd_) || !ImGui_ImplDX11_Init(device_,context_)) { removed(); return Steinberg::kResultFalse; }
    imgui_ready_=true; SetTimer(hwnd_,1,16,nullptr); render(); return Steinberg::kResultOk;
}
Steinberg::tresult PLUGIN_API EmberVst3Editor::removed() {
    if (hwnd_) KillTimer(hwnd_,1);
    if (imgui_ready_) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); imgui_ready_=false; }
    destroyDevice();
    if (hwnd_ && IsWindow(hwnd_)) { HWND h=hwnd_; hwnd_=nullptr; DestroyWindow(h); }
    return Steinberg::kResultOk;
}
Steinberg::tresult PLUGIN_API EmberVst3Editor::onWheel(float d) { if(imgui_ready_) ImGui::GetIO().AddMouseWheelEvent(0,d); return Steinberg::kResultTrue; }
Steinberg::tresult PLUGIN_API EmberVst3Editor::onKeyDown(Steinberg::char16,Steinberg::int16,Steinberg::int16){return Steinberg::kResultFalse;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::onKeyUp(Steinberg::char16,Steinberg::int16,Steinberg::int16){return Steinberg::kResultFalse;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::getSize(Steinberg::ViewRect* s){if(!s)return Steinberg::kInvalidArgument;*s=rect_;return Steinberg::kResultOk;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::onSize(Steinberg::ViewRect* s){if(!s)return Steinberg::kInvalidArgument;rect_=*s;if(hwnd_)SetWindowPos(hwnd_,nullptr,0,0,s->getWidth(),s->getHeight(),SWP_NOZORDER|SWP_NOMOVE);return Steinberg::kResultOk;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::onFocus(Steinberg::TBool state){if(hwnd_&&state)SetFocus(hwnd_);return Steinberg::kResultOk;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::setFrame(Steinberg::IPlugFrame* f){if(frame_)frame_->release();frame_=f;if(frame_)frame_->addRef();return Steinberg::kResultOk;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::canResize(){return Steinberg::kResultTrue;}
Steinberg::tresult PLUGIN_API EmberVst3Editor::checkSizeConstraint(Steinberg::ViewRect* r){if(!r)return Steinberg::kInvalidArgument;r->right=r->left+std::max<Steinberg::int32>(480,r->getWidth());r->bottom=r->top+std::max<Steinberg::int32>(320,r->getHeight());return Steinberg::kResultTrue;}

bool EmberVst3Editor::createDevice(){
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hwnd_; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level; const D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested,2,D3D11_SDK_VERSION,&sd,&swap_chain_,&device_,&level,&context_)))
        return false;
    return createRenderTarget();
}
bool EmberVst3Editor::createRenderTarget(){ID3D11Texture2D* back=nullptr;if(!swap_chain_||FAILED(swap_chain_->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back)))return false;const HRESULT hr=device_->CreateRenderTargetView(back,nullptr,&render_target_);back->Release();return SUCCEEDED(hr);}
void EmberVst3Editor::destroyRenderTarget(){releaseCom(render_target_);}
void EmberVst3Editor::destroyDevice(){destroyRenderTarget();releaseCom(swap_chain_);releaseCom(context_);releaseCom(device_);}
void EmberVst3Editor::resize(unsigned w,unsigned h){if(!swap_chain_||!w||!h)return;destroyRenderTarget();swap_chain_->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0);createRenderTarget();}
void EmberVst3Editor::render(){
    if(!imgui_ready_||!render_target_)return;
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    if(!processor_.renderScriptUi()) {
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Ember",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
        ImGui::TextColored(ImVec4(.29f,.965f,.149f,1),"EMBER // LIVE DSP"); ImGui::Separator();
        for(std::size_t i=0;i<processor_.parameterCount();++i){float value=processor_.parameterValue(i);if(ImGui::SimpleKnob(processor_.parameterName(i),&value,processor_.parameterMinimum(i),processor_.parameterMaximum(i)))processor_.setParameterFromEditor(i,value);if(i+1<processor_.parameterCount())ImGui::SameLine();}
        processor_.drawDefaultVisualizations(); ImGui::End();
    }
    ImGui::Render(); const float clear[4]={.03f,.04f,.025f,1}; context_->OMSetRenderTargets(1,&render_target_,nullptr); context_->ClearRenderTargetView(render_target_,clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); swap_chain_->Present(1,0);
}
long long __stdcall EmberVst3Editor::windowProc(HWND__* raw,unsigned msg,unsigned long long wp,long long lp){HWND hwnd=(HWND)raw;auto* self=reinterpret_cast<EmberVst3Editor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(lp);self=static_cast<EmberVst3Editor*>(cs->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(self&&self->imgui_ready_&&ImGui_ImplWin32_WndProcHandler(hwnd,msg,(WPARAM)wp,(LPARAM)lp))return 1;if(self){if(msg==WM_TIMER||msg==WM_PAINT){PAINTSTRUCT ps;BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);self->render();return 0;}if(msg==WM_SIZE&&wp!=SIZE_MINIMIZED){self->resize(LOWORD(lp),HIWORD(lp));return 0;}if(msg==WM_ERASEBKGND)return 1;}return DefWindowProcW(hwnd,msg,(WPARAM)wp,(LPARAM)lp);}
} // namespace EmberVst3
