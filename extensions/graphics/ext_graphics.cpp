// ext_graphics.cpp - Win32 HWND + D3D11 full-screen shader rendering for Ember.
#include "ext_graphics.hpp"

#include "ast.hpp"
#include "binding_builder.hpp"
#include "ext_array.hpp"
#include "ext_string.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace ember::ext_graphics {
namespace {

constexpr size_t kMaxWindows = 16;
constexpr size_t kMaxPrograms = 64;
constexpr size_t kMaxHlslBytes = 64 * 1024;
constexpr size_t kMaxConstantFloats = 256;
constexpr UINT kConstantBytes = UINT(kMaxConstantFloats * sizeof(float));
constexpr wchar_t kWindowClassName[] = L"EmberD3D11GraphicsWindow";

// Handles pack {generation:32,index+1:32}. Raw HWND/COM pointers never cross
// the native boundary, and stale handles fail after slot reuse.
constexpr int64_t make_handle(uint32_t index, uint32_t generation) {
    return int64_t((uint64_t(generation) << 32) | uint64_t(index + 1));
}
bool split_handle(int64_t handle, uint32_t& index, uint32_t& generation) {
    const uint64_t value = uint64_t(handle);
    const uint32_t low = uint32_t(value);
    generation = uint32_t(value >> 32);
    if (low == 0 || generation == 0) return false;
    index = low - 1;
    return true;
}

template <typename T> void release_com(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

struct WindowSlot {
    uint32_t generation = 1;
    bool live = false;
    bool should_close = false;
    bool minimized = false;
    HWND hwnd = nullptr;
    UINT width = 0;
    UINT height = 0;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
};

struct ProgramSlot {
    uint32_t generation = 1;
    bool live = false;
    uint32_t window_index = 0;
    uint32_t window_generation = 0;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11Buffer* constants = nullptr;
};

std::array<WindowSlot, kMaxWindows> g_windows;
std::array<ProgramSlot, kMaxPrograms> g_programs;
std::recursive_mutex g_mutex;
std::string g_last_error;
ATOM g_window_class = 0;
HINSTANCE g_instance = nullptr;

void set_error(std::string message) { g_last_error = std::move(message); }
void clear_error() { g_last_error.clear(); }

std::string hr_text(const char* operation, HRESULT hr) {
    char code[32];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    std::string out(operation);
    out += " failed (";
    out += code;
    out += ")";
    return out;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    if (text.size() > size_t(std::numeric_limits<int>::max())) return std::wstring();
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    int(text.size()), nullptr, 0);
    if (count <= 0) return std::wstring();
    std::wstring result(size_t(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            int(text.size()), result.data(), count) != count)
        return std::wstring();
    return result;
}

WindowSlot* get_window(int64_t handle, uint32_t* out_index = nullptr) {
    uint32_t index = 0, generation = 0;
    if (!split_handle(handle, index, generation) || index >= g_windows.size()) return nullptr;
    WindowSlot& window = g_windows[index];
    if (!window.live || window.generation != generation) return nullptr;
    if (out_index) *out_index = index;
    return &window;
}

ProgramSlot* get_program(int64_t handle, uint32_t* out_index = nullptr) {
    uint32_t index = 0, generation = 0;
    if (!split_handle(handle, index, generation) || index >= g_programs.size()) return nullptr;
    ProgramSlot& program = g_programs[index];
    if (!program.live || program.generation != generation) return nullptr;
    if (out_index) *out_index = index;
    return &program;
}

void bump_generation(uint32_t& generation) {
    ++generation;
    if (generation == 0) generation = 1;
}

void release_program(ProgramSlot& program) {
    // Reverse dependency order: buffer/shaders before the owning device.
    release_com(program.constants);
    release_com(program.pixel_shader);
    release_com(program.vertex_shader);
    program.live = false;
    program.window_index = 0;
    program.window_generation = 0;
    bump_generation(program.generation);
}

void release_window_resources(WindowSlot& window) {
    if (window.context) {
        window.context->OMSetRenderTargets(0, nullptr, nullptr);
        window.context->ClearState();
        window.context->Flush();
    }
    release_com(window.render_target);
    release_com(window.swap_chain);
    release_com(window.context);
    release_com(window.device);
}

void release_window(uint32_t index, bool destroy_hwnd) {
    WindowSlot& window = g_windows[index];
    if (!window.live) return;
    for (ProgramSlot& program : g_programs) {
        if (program.live && program.window_index == index &&
            program.window_generation == window.generation)
            release_program(program);
    }
    release_window_resources(window);
    HWND hwnd = window.hwnd;
    window.hwnd = nullptr;
    window.live = false;
    window.should_close = true;
    window.width = window.height = 0;
    window.minimized = false;
    bump_generation(window.generation);
    if (destroy_hwnd && hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
}

bool create_render_target(WindowSlot& window) {
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = window.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr)) { set_error(hr_text("IDXGISwapChain::GetBuffer", hr)); return false; }
    hr = window.device->CreateRenderTargetView(back_buffer, nullptr, &window.render_target);
    back_buffer->Release();
    if (FAILED(hr)) { set_error(hr_text("CreateRenderTargetView", hr)); return false; }
    return true;
}

bool resize_window(WindowSlot& window, UINT width, UINT height) {
    window.width = width;
    window.height = height;
    window.minimized = (width == 0 || height == 0);
    if (window.minimized || !window.swap_chain) return true;
    if (window.context) window.context->OMSetRenderTargets(0, nullptr, nullptr);
    release_com(window.render_target);
    HRESULT hr = window.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        set_error("D3D11 device was removed while resizing; recreate the window");
        window.should_close = true;
        return false;
    }
    if (FAILED(hr)) { set_error(hr_text("IDXGISwapChain::ResizeBuffers", hr)); return false; }
    return create_render_target(window);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* window = reinterpret_cast<WindowSlot*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        window = static_cast<WindowSlot*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        if (window) window->hwnd = hwnd;
    }
    switch (message) {
    case WM_CLOSE:
        if (window) window->should_close = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        if (window) { window->should_close = true; window->hwnd = nullptr; }
        bool another_window = false;
        for (const WindowSlot& candidate : g_windows) {
            if (candidate.live && candidate.hwnd) { another_window = true; break; }
        }
        if (!another_window) PostQuitMessage(0);
        return 0;
    }
    case WM_SIZE:
        if (window && window->live) {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            resize_window(*window, LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool ensure_window_class() {
    if (g_window_class) return true;
    g_instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    g_window_class = RegisterClassExW(&wc);
    if (!g_window_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        set_error("RegisterClassExW failed");
        return false;
    }
    if (!g_window_class) g_window_class = 1; // class owned by an earlier registration
    return true;
}

bool initialize_d3d(WindowSlot& window, UINT width, UINT height) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window.hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL actual{};
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        requested, UINT(std::size(requested)), D3D11_SDK_VERSION, &desc,
        &window.swap_chain, &window.device, &actual, &window.context);
#ifdef _DEBUG
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            requested, UINT(std::size(requested)), D3D11_SDK_VERSION, &desc,
            &window.swap_chain, &window.device, &actual, &window.context);
    }
#endif
    if (hr == E_INVALIDARG) { // Windows 7 runtime does not know FL 11.1.
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            requested + 1, UINT(std::size(requested) - 1), D3D11_SDK_VERSION, &desc,
            &window.swap_chain, &window.device, &actual, &window.context);
    }
    if (FAILED(hr)) {
        release_window_resources(window);
        set_error(hr_text("D3D11CreateDeviceAndSwapChain", hr));
        return false;
    }
    return create_render_target(window);
}

bool read_string(int64_t handle, const char* label, std::string& out) {
    if (!ext_string::copy(handle, out)) {
        set_error(std::string(label) + " is not a valid string handle");
        return false;
    }
    return true;
}

ID3DBlob* compile_shader(const std::string& source, const char* entry,
                         const char* target, const char* label) {
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(source.data(), source.size(), label, nullptr,
                            nullptr, entry, target,
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    if (FAILED(hr)) {
        std::string message = std::string(label) + " compilation failed";
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize()) {
            message += ": ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()),
                           errors->GetBufferSize());
        } else {
            message += " (" + hr_text("D3DCompile", hr) + ")";
        }
        set_error(std::move(message));
        release_com(errors);
        release_com(bytecode);
        return nullptr;
    }
    release_com(errors);
    return bytecode;
}

int64_t n_window_create(int64_t width64, int64_t height64, int64_t title_handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    clear_error();
    if (width64 < 1 || height64 < 1 || width64 > 16384 || height64 > 16384) {
        set_error("window dimensions must be in the range 1..16384");
        return 0;
    }
    std::string title;
    if (!read_string(title_handle, "window title", title)) return 0;
    std::wstring wide_title = utf8_to_wide(title);
    if (!title.empty() && wide_title.empty()) { set_error("window title is not valid UTF-8"); return 0; }
    if (!ensure_window_class()) return 0;

    uint32_t index = uint32_t(g_windows.size());
    for (uint32_t i = 0; i < g_windows.size(); ++i)
        if (!g_windows[i].live) { index = i; break; }
    if (index == g_windows.size()) { set_error("graphics window limit reached (16)"); return 0; }

    WindowSlot& window = g_windows[index];
    window.live = true;
    window.should_close = false;
    window.minimized = false;
    window.width = UINT(width64);
    window.height = UINT(height64);

    RECT rect{0, 0, LONG(width64), LONG(height64)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);
    HWND hwnd = CreateWindowExW(0, kWindowClassName, wide_title.c_str(), style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, g_instance, &window);
    if (!hwnd) {
        window.live = false;
        set_error("CreateWindowExW failed");
        return 0;
    }
    window.hwnd = hwnd;
    if (!initialize_d3d(window, UINT(width64), UINT(height64))) {
        release_window(index, true);
        return 0;
    }
    return make_handle(index, window.generation);
}

int64_t n_window_show(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window || !window->hwnd) { set_error("window_show: invalid window handle"); return 0; }
    ShowWindow(window->hwnd, SW_SHOW);
    UpdateWindow(window->hwnd);
    clear_error();
    return 1;
}

int64_t n_window_poll_events(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window) { set_error("window_poll_events: invalid window handle"); return 0; }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) window->should_close = true;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    clear_error();
    return window->should_close ? 0 : 1;
}

int64_t n_window_should_close(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window) { set_error("window_should_close: invalid window handle"); return 1; }
    clear_error();
    return window->should_close ? 1 : 0;
}

int64_t n_window_width(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window) { set_error("window_width: invalid window handle"); return 0; }
    clear_error(); return window->width;
}
int64_t n_window_height(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window) { set_error("window_height: invalid window handle"); return 0; }
    clear_error(); return window->height;
}

int64_t n_window_set_title(int64_t handle, int64_t title_handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window || !window->hwnd) { set_error("window_set_title: invalid window handle"); return 0; }
    std::string title;
    if (!read_string(title_handle, "window title", title)) return 0;
    std::wstring wide_title = utf8_to_wide(title);
    if (!title.empty() && wide_title.empty()) { set_error("window title is not valid UTF-8"); return 0; }
    if (!SetWindowTextW(window->hwnd, wide_title.c_str())) { set_error("SetWindowTextW failed"); return 0; }
    clear_error(); return 1;
}

void n_window_destroy(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t index = 0;
    WindowSlot* window = get_window(handle, &index);
    if (!window) { set_error("window_destroy: invalid window handle"); return; }
    release_window(index, true);
    clear_error();
}

int64_t n_shader_create(int64_t window_handle, int64_t vs_handle, int64_t ps_handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    clear_error();
    uint32_t window_index = 0;
    WindowSlot* window = get_window(window_handle, &window_index);
    if (!window) { set_error("shader_create: invalid window handle"); return 0; }
    std::string vs_source, ps_source;
    if (!read_string(vs_handle, "vertex HLSL", vs_source) ||
        !read_string(ps_handle, "pixel HLSL", ps_source)) return 0;
    if (vs_source.empty() || ps_source.empty() ||
        vs_source.size() > kMaxHlslBytes || ps_source.size() > kMaxHlslBytes) {
        set_error("HLSL source must be non-empty and at most 64 KiB per stage");
        return 0;
    }
    uint32_t index = uint32_t(g_programs.size());
    for (uint32_t i = 0; i < g_programs.size(); ++i)
        if (!g_programs[i].live) { index = i; break; }
    if (index == g_programs.size()) { set_error("graphics shader-program limit reached (64)"); return 0; }

    ID3DBlob* vs_blob = compile_shader(vs_source, "vs_main", "vs_5_0", "Ember vertex shader");
    if (!vs_blob) return 0;
    ID3DBlob* ps_blob = compile_shader(ps_source, "ps_main", "ps_5_0", "Ember pixel shader");
    if (!ps_blob) { release_com(vs_blob); return 0; }

    ProgramSlot& program = g_programs[index];
    HRESULT hr = window->device->CreateVertexShader(vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(), nullptr, &program.vertex_shader);
    if (SUCCEEDED(hr)) hr = window->device->CreatePixelShader(ps_blob->GetBufferPointer(),
        ps_blob->GetBufferSize(), nullptr, &program.pixel_shader);
    release_com(ps_blob);
    release_com(vs_blob);
    if (FAILED(hr)) {
        release_com(program.pixel_shader);
        release_com(program.vertex_shader);
        set_error(hr_text("D3D11 shader creation", hr));
        return 0;
    }

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = kConstantBytes;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = window->device->CreateBuffer(&buffer_desc, nullptr, &program.constants);
    if (FAILED(hr)) {
        release_com(program.pixel_shader);
        release_com(program.vertex_shader);
        set_error(hr_text("CreateBuffer(constant buffer)", hr));
        return 0;
    }
    program.live = true;
    program.window_index = window_index;
    program.window_generation = window->generation;
    return make_handle(index, program.generation);
}

void n_shader_destroy(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ProgramSlot* program = get_program(handle);
    if (!program) { set_error("shader_destroy: invalid program handle"); return; }
    release_program(*program);
    clear_error();
}

int64_t n_shader_set_constants(int64_t handle, int64_t array_handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ProgramSlot* program = get_program(handle);
    if (!program) { set_error("shader_set_constants: invalid program handle"); return 0; }
    WindowSlot& window = g_windows[program->window_index];
    if (!window.live || window.generation != program->window_generation) {
        set_error("shader_set_constants: owning window is no longer valid"); return 0;
    }
    std::array<float, kMaxConstantFloats> values{};
    int64_t count = 0;
    if (!ext_array::copy_f32(array_handle, values.data(),
                             int64_t(kMaxConstantFloats), &count)) {
        set_error("shader constants must be an array<f32> of at most 256 floats"); return 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = window.context->Map(program->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { set_error(hr_text("Map(constant buffer)", hr)); return 0; }
    std::memset(mapped.pData, 0, kConstantBytes);
    if (count > 0) std::memcpy(mapped.pData, values.data(), size_t(count) * sizeof(float));
    window.context->Unmap(program->constants, 0);
    clear_error(); return 1;
}

int64_t n_shader_draw_fullscreen(int64_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ProgramSlot* program = get_program(handle);
    if (!program) { set_error("shader_draw_fullscreen: invalid program handle"); return 0; }
    WindowSlot& window = g_windows[program->window_index];
    if (!window.live || window.generation != program->window_generation) {
        set_error("shader_draw_fullscreen: owning window is no longer valid"); return 0;
    }
    if (window.minimized || !window.render_target || window.width == 0 || window.height == 0) {
        // A minimized/zero-sized window is recoverable: skip this frame.
        clear_error(); return 1;
    }
    D3D11_VIEWPORT viewport{};
    viewport.Width = float(window.width);
    viewport.Height = float(window.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    window.context->RSSetViewports(1, &viewport);
    window.context->OMSetRenderTargets(1, &window.render_target, nullptr);
    window.context->IASetInputLayout(nullptr);
    window.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    window.context->VSSetShader(program->vertex_shader, nullptr, 0);
    window.context->PSSetShader(program->pixel_shader, nullptr, 0);
    window.context->VSSetConstantBuffers(0, 1, &program->constants);
    window.context->PSSetConstantBuffers(0, 1, &program->constants);
    window.context->Draw(3, 0);
    clear_error(); return 1;
}

int64_t n_graphics_clear(int64_t handle, float r, float g, float b, float a) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window) { set_error("graphics_clear: invalid window handle"); return 0; }
    if (window->minimized || !window->render_target) { clear_error(); return 1; }
    const float color[4] = {r, g, b, a};
    window->context->ClearRenderTargetView(window->render_target, color);
    clear_error(); return 1;
}

int64_t n_graphics_present(int64_t handle, int64_t vsync) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    WindowSlot* window = get_window(handle);
    if (!window) { set_error("graphics_present: invalid window handle"); return 0; }
    if (window->minimized || !window->swap_chain) { Sleep(10); clear_error(); return 1; }
    HRESULT hr = window->swap_chain->Present(vsync != 0 ? 1u : 0u, 0);
    if (hr == DXGI_STATUS_OCCLUDED) { Sleep(10); clear_error(); return 1; }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HRESULT reason = window->device ? window->device->GetDeviceRemovedReason() : hr;
        set_error(hr_text("D3D11 device removed during Present", reason));
        window->should_close = true;
        return 0;
    }
    if (FAILED(hr)) { set_error(hr_text("IDXGISwapChain::Present", hr)); return 0; }
    clear_error(); return 1;
}

int64_t n_graphics_last_error() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return ext_string::alloc(g_last_error);
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder b;
    const Type i64 = type_i64();
    const Type f32 = type_f32();
    const Type boolean = type_bool();
    const Type string = bind_handle("string");
    b.add("window_create", i64, {i64, i64, string}, (void*)&n_window_create, PERM_FFI);
    b.add("window_show", boolean, {i64}, (void*)&n_window_show, PERM_FFI);
    b.add("window_poll_events", boolean, {i64}, (void*)&n_window_poll_events, PERM_FFI);
    b.add("window_should_close", boolean, {i64}, (void*)&n_window_should_close, PERM_FFI);
    b.add("window_width", i64, {i64}, (void*)&n_window_width, PERM_FFI);
    b.add("window_height", i64, {i64}, (void*)&n_window_height, PERM_FFI);
    b.add("window_set_title", boolean, {i64, string}, (void*)&n_window_set_title, PERM_FFI);
    b.add("window_destroy", type_void(), {i64}, (void*)&n_window_destroy, PERM_FFI);
    b.add("shader_create", i64, {i64, string, string}, (void*)&n_shader_create, PERM_FFI);
    b.add("shader_destroy", type_void(), {i64}, (void*)&n_shader_destroy, PERM_FFI);
    // Ember's dynamic array<T> is represented by its opaque i64 host handle.
    b.add("shader_set_constants", boolean, {i64, i64}, (void*)&n_shader_set_constants, PERM_FFI);
    b.add("shader_draw_fullscreen", boolean, {i64}, (void*)&n_shader_draw_fullscreen, PERM_FFI);
    b.add("graphics_clear", boolean, {i64, f32, f32, f32, f32}, (void*)&n_graphics_clear, PERM_FFI);
    b.add("graphics_present", boolean, {i64, i64}, (void*)&n_graphics_present, PERM_FFI);
    b.add("graphics_last_error", string, {}, (void*)&n_graphics_last_error, PERM_FFI);
    NativeTable table = b.build();
    for (auto& item : table.natives) natives[item.first] = std::move(item.second);
}

void register_overloads(OpOverloadTable&) {}

void reset() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (ProgramSlot& program : g_programs) if (program.live) release_program(program);
    for (uint32_t i = 0; i < g_windows.size(); ++i) if (g_windows[i].live) release_window(i, true);
    g_last_error.clear();
}

} // namespace ember::ext_graphics
