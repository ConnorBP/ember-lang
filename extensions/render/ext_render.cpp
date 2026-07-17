// ext_render.cpp - Prism-style shader/render store with an optional backend.
#include "ext_render.hpp"

#include "binding_builder.hpp"
#include "ext_array.hpp"
#include "ext_string.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace ember::ext_render {
namespace {

constexpr std::size_t kMaxResources = 65536;
constexpr int64_t kMaxArrayElements = 1024 * 1024;
constexpr int64_t kMaxShaderBytes = 1024 * 1024;
ShaderStore make_store() {
    ShaderStore store;
    store.constants = {
        {"TOPO_TRIANGLE_LIST", TOPO_TRIANGLE_LIST},
        {"TOPO_TRIANGLE_STRIP", TOPO_TRIANGLE_STRIP},
        {"TOPO_LINE_LIST", TOPO_LINE_LIST},
        {"TOPO_LINE_STRIP", TOPO_LINE_STRIP},
        {"TOPO_POINT_LIST", TOPO_POINT_LIST},
    };
    return store;
}
ShaderStore g_store = make_store();
RenderBackend* g_backend = nullptr;
std::mutex g_mutex;

bool string_value(int64_t handle, std::string& out) {
    return ext_string::copy(handle, out);
}

bool has_resource(int64_t handle, ResourceType type) {
    auto it = g_store.resources.find(handle);
    return it != g_store.resources.end() && it->second.type == type;
}

int64_t create_resource(Resource value) {
    if (g_store.resources.size() >= kMaxResources || g_store.next_handle <= 0 ||
        g_store.next_handle == std::numeric_limits<int64_t>::max()) return 0;
    const int64_t handle = g_store.next_handle++;
    try {
        g_store.resources.emplace(handle, std::move(value));
        return handle;
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}

int64_t n_render_create_vertex_shader(int64_t source_handle) {
    Resource snapshot_value; int64_t handle = 0; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string source;
        if (!string_value(source_handle, source) || source.empty() ||
            source.size() > std::size_t(kMaxShaderBytes)) return 0;
        Resource r; r.type = ResourceType::VertexShader; r.text = std::move(source);
        handle = create_resource(std::move(r));
        if (handle) { snapshot_value = g_store.resources.at(handle); backend = g_backend; }
    }
    if (backend) backend->resource_created(handle, snapshot_value);
    return handle;
}

int64_t n_render_create_pixel_shader(int64_t source_handle) {
    Resource snapshot_value; int64_t handle = 0; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string source;
        if (!string_value(source_handle, source) || source.empty() ||
            source.size() > std::size_t(kMaxShaderBytes)) return 0;
        Resource r; r.type = ResourceType::PixelShader; r.text = std::move(source);
        handle = create_resource(std::move(r));
        if (handle) { snapshot_value = g_store.resources.at(handle); backend = g_backend; }
    }
    if (backend) backend->resource_created(handle, snapshot_value);
    return handle;
}

int64_t n_render_create_input_layout(int64_t elements_handle) {
    Resource snapshot_value; int64_t handle = 0; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string elements;
        if (!string_value(elements_handle, elements) || elements.empty() ||
            elements.size() > std::size_t(kMaxShaderBytes)) return 0;
        Resource r; r.type = ResourceType::InputLayout; r.text = std::move(elements);
        handle = create_resource(std::move(r));
        if (handle) { snapshot_value = g_store.resources.at(handle); backend = g_backend; }
    }
    if (backend) backend->resource_created(handle, snapshot_value);
    return handle;
}

int64_t n_render_create_vertex_buffer(int64_t data_handle, int64_t stride) {
    if (stride <= 0 || stride > (1 << 20)) return 0;
    std::vector<float> values(static_cast<std::size_t>(kMaxArrayElements), 0.0f); int64_t count = 0;
    if (!ext_array::copy_f32(data_handle, values.data(), kMaxArrayElements, &count)) return 0;
    values.resize(std::size_t(count));
    Resource snapshot_value; int64_t handle = 0; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Resource r; r.type = ResourceType::VertexBuffer; r.floats = std::move(values); r.stride = stride;
        handle = create_resource(std::move(r));
        if (handle) { snapshot_value = g_store.resources.at(handle); backend = g_backend; }
    }
    if (backend) backend->resource_created(handle, snapshot_value);
    return handle;
}

int64_t n_render_create_index_buffer(int64_t data_handle) {
    std::vector<int32_t> values(static_cast<std::size_t>(kMaxArrayElements), 0); int64_t count = 0;
    if (!ext_array::copy_i32(data_handle, values.data(), kMaxArrayElements, &count)) return 0;
    values.resize(std::size_t(count));
    Resource snapshot_value; int64_t handle = 0; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Resource r; r.type = ResourceType::IndexBuffer; r.indices = std::move(values);
        handle = create_resource(std::move(r));
        if (handle) { snapshot_value = g_store.resources.at(handle); backend = g_backend; }
    }
    if (backend) backend->resource_created(handle, snapshot_value);
    return handle;
}

int64_t n_render_create_constant_buffer(int64_t size) {
    if (size <= 0 || size > (1 << 20)) return 0;
    Resource snapshot_value; int64_t handle = 0; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Resource r; r.type = ResourceType::ConstantBuffer;
        r.size = (size + 15) & ~int64_t(15);
        r.floats.assign(std::size_t((r.size + 3) / 4), 0.0f);
        handle = create_resource(std::move(r));
        if (handle) { snapshot_value = g_store.resources.at(handle); backend = g_backend; }
    }
    if (backend) backend->resource_created(handle, snapshot_value);
    return handle;
}

void n_render_update_constant_buffer(int64_t handle, int64_t data_handle) {
    std::vector<float> values(static_cast<std::size_t>(kMaxArrayElements), 0.0f); int64_t count = 0;
    if (!ext_array::copy_f32(data_handle, values.data(), kMaxArrayElements, &count)) return;
    Resource updated; RenderBackend* backend = nullptr; bool ok = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_store.resources.find(handle);
        if (it == g_store.resources.end() || it->second.type != ResourceType::ConstantBuffer ||
            uint64_t(count) * sizeof(float) > uint64_t(it->second.size)) return;
        std::fill(it->second.floats.begin(), it->second.floats.end(), 0.0f);
        std::copy_n(values.begin(), std::size_t(count), it->second.floats.begin());
        updated = it->second; backend = g_backend; ok = true;
    }
    if (ok && backend) backend->resource_updated(handle, updated);
}

void set_state(int64_t handle, ResourceType type, int64_t ShaderStore::*member) {
    ShaderStore state; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (handle != 0 && !has_resource(handle, type)) return;
        g_store.*member = handle; state = g_store; backend = g_backend;
    }
    if (backend) backend->state_changed(state);
}
void n_render_set_vertex_shader(int64_t h) { set_state(h, ResourceType::VertexShader, &ShaderStore::vertex_shader); }
void n_render_set_pixel_shader(int64_t h) { set_state(h, ResourceType::PixelShader, &ShaderStore::pixel_shader); }
void n_render_set_input_layout(int64_t h) { set_state(h, ResourceType::InputLayout, &ShaderStore::input_layout); }
void n_render_set_vertex_buffer(int64_t h) { set_state(h, ResourceType::VertexBuffer, &ShaderStore::vertex_buffer); }
void n_render_set_index_buffer(int64_t h) { set_state(h, ResourceType::IndexBuffer, &ShaderStore::index_buffer); }

void n_render_set_constant_buffer(int64_t slot, int64_t handle) {
    if (slot < 0 || slot > 63) return;
    ShaderStore state; RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (handle != 0 && !has_resource(handle, ResourceType::ConstantBuffer)) return;
        if (handle) g_store.constant_buffers[slot] = handle;
        else g_store.constant_buffers.erase(slot);
        state = g_store; backend = g_backend;
    }
    if (backend) backend->state_changed(state);
}

void n_render_draw(int64_t count) {
    RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (count < 0) return;
        ++g_store.draw_calls; backend = g_backend;
    }
    if (backend) backend->draw(false, count);
}
void n_render_draw_indexed(int64_t count) {
    RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (count < 0) return;
        ++g_store.draw_calls; ++g_store.indexed_draw_calls; backend = g_backend;
    }
    if (backend) backend->draw(true, count);
}

void n_render_destroy_resource(int64_t handle) {
    RenderBackend* backend = nullptr; bool removed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        removed = g_store.resources.erase(handle) != 0;
        if (!removed) return;
        if (g_store.vertex_shader == handle) g_store.vertex_shader = 0;
        if (g_store.pixel_shader == handle) g_store.pixel_shader = 0;
        if (g_store.input_layout == handle) g_store.input_layout = 0;
        if (g_store.vertex_buffer == handle) g_store.vertex_buffer = 0;
        if (g_store.index_buffer == handle) g_store.index_buffer = 0;
        for (auto it = g_store.constant_buffers.begin(); it != g_store.constant_buffers.end(); )
            if (it->second == handle) it = g_store.constant_buffers.erase(it); else ++it;
        backend = g_backend;
    }
    if (backend) backend->resource_destroyed(handle);
}

int64_t n_render_get_draw_calls() {
    std::lock_guard<std::mutex> lock(g_mutex); return g_store.draw_calls;
}
void n_render_clear(float r, float g, float b, float a) {
    RenderBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_store.clear_color[0] = r; g_store.clear_color[1] = g;
        g_store.clear_color[2] = b; g_store.clear_color[3] = a;
        ++g_store.clear_calls; backend = g_backend;
    }
    if (backend) backend->clear(r, g, b, a);
}
void n_render_present() {
    RenderBackend* backend = nullptr;
    { std::lock_guard<std::mutex> lock(g_mutex); ++g_store.present_calls; backend = g_backend; }
    if (backend) backend->present();
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder b;
    const Type i64 = type_i64(), f32 = type_f32(), string = bind_handle("string");
    const uint32_t p = PERM_FFI;
    b.add("render_create_vertex_shader", i64, {string}, (void*)&n_render_create_vertex_shader, p);
    b.add("render_create_pixel_shader", i64, {string}, (void*)&n_render_create_pixel_shader, p);
    b.add("render_create_input_layout", i64, {string}, (void*)&n_render_create_input_layout, p);
    b.add("render_create_vertex_buffer", i64, {i64,i64}, (void*)&n_render_create_vertex_buffer, p);
    b.add("render_create_index_buffer", i64, {i64}, (void*)&n_render_create_index_buffer, p);
    b.add("render_create_constant_buffer", i64, {i64}, (void*)&n_render_create_constant_buffer, p);
    b.add("render_update_constant_buffer", type_void(), {i64,i64}, (void*)&n_render_update_constant_buffer, p);
    b.add("render_set_vertex_shader", type_void(), {i64}, (void*)&n_render_set_vertex_shader, p);
    b.add("render_set_pixel_shader", type_void(), {i64}, (void*)&n_render_set_pixel_shader, p);
    b.add("render_set_input_layout", type_void(), {i64}, (void*)&n_render_set_input_layout, p);
    b.add("render_set_vertex_buffer", type_void(), {i64}, (void*)&n_render_set_vertex_buffer, p);
    b.add("render_set_index_buffer", type_void(), {i64}, (void*)&n_render_set_index_buffer, p);
    b.add("render_set_constant_buffer", type_void(), {i64,i64}, (void*)&n_render_set_constant_buffer, p);
    b.add("render_draw", type_void(), {i64}, (void*)&n_render_draw, p);
    b.add("render_draw_indexed", type_void(), {i64}, (void*)&n_render_draw_indexed, p);
    b.add("render_destroy_resource", type_void(), {i64}, (void*)&n_render_destroy_resource, p);
    b.add("render_get_draw_calls", i64, {}, (void*)&n_render_get_draw_calls, p);
    b.add("render_clear", type_void(), {f32,f32,f32,f32}, (void*)&n_render_clear, p);
    b.add("render_present", type_void(), {}, (void*)&n_render_present, p);
    NativeTable table = b.build();
    for (auto& item : table.natives) natives[item.first] = std::move(item.second);
}

void inject_constants(Program& program) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& constant : g_store.constants) {
        bool exists = false;
        for (const GlobalDecl& global : program.globals)
            if (global.ns.empty() && global.name == constant.first) { exists = true; break; }
        if (exists) continue;
        GlobalDecl global;
        global.name = constant.first;
        global.ty = std::make_shared<Type>(make_prim(Prim::I32));
        auto literal = std::make_unique<IntLit>(); literal->v = constant.second;
        global.init = std::move(literal); global.is_const = true;
        program.globals.push_back(std::move(global));
    }
}

void set_backend(RenderBackend* backend) {
    std::lock_guard<std::mutex> lock(g_mutex); g_backend = backend;
}
ShaderStore snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex); return g_store;
}
void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    RenderBackend* backend = g_backend;
    g_store = make_store();
    g_backend = backend;
}

} // namespace ember::ext_render
