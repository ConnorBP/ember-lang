#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <atomic>

struct HWND__;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;

namespace EmberVst3 {
class EmberProcessor;

class EmberVst3Editor final : public Steinberg::IPlugView {
public:
    explicit EmberVst3Editor(EmberProcessor& processor);
    ~EmberVst3Editor();

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onWheel(float distance) override;
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    void render();
    void resize(unsigned width, unsigned height);
    static long long __stdcall windowProc(HWND__*, unsigned, unsigned long long, long long);

private:
    bool createDevice();
    bool createRenderTarget();
    void destroyRenderTarget();
    void destroyDevice();

    std::atomic<Steinberg::uint32> refs_ {1};
    EmberProcessor& processor_;
    Steinberg::IPlugFrame* frame_ {nullptr};
    Steinberg::ViewRect rect_ {0, 0, 720, 520};
    HWND__* hwnd_ {nullptr};
    ID3D11Device* device_ {nullptr};
    ID3D11DeviceContext* context_ {nullptr};
    IDXGISwapChain* swap_chain_ {nullptr};
    ID3D11RenderTargetView* render_target_ {nullptr};
    bool imgui_ready_ {false};
};
} // namespace EmberVst3
