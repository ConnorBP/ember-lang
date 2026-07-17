// ext_render.hpp - stub-backed shader/render command extension.
#pragma once

#include "ast.hpp"
#include "sema.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember::ext_render {

enum class ResourceType : uint8_t {
    VertexShader, PixelShader, InputLayout, VertexBuffer, IndexBuffer,
    ConstantBuffer
};

struct Resource {
    ResourceType type = ResourceType::VertexShader;
    std::string text;
    std::vector<float> floats;
    std::vector<int32_t> indices;
    int64_t stride = 0;
    int64_t size = 0;
};

// The Prism-style stub store is the authoritative data model. A real renderer
// may inspect snapshots after native calls and mirror handles to GPU objects.
struct ShaderStore {
    int64_t next_handle = 1;
    int64_t draw_calls = 0;
    int64_t indexed_draw_calls = 0;
    int64_t clear_calls = 0;
    int64_t present_calls = 0;
    std::unordered_map<int64_t, Resource> resources;
    std::unordered_map<std::string, int32_t> constants;
    int64_t vertex_shader = 0;
    int64_t pixel_shader = 0;
    int64_t input_layout = 0;
    int64_t vertex_buffer = 0;
    int64_t index_buffer = 0;
    std::unordered_map<int64_t, int64_t> constant_buffers;
    float clear_color[4] = {0, 0, 0, 1};
};

// Optional non-owning command sink. The extension remains fully usable as a
// stub when this is null. Callbacks receive snapshots and never execute while
// the store lock is held, allowing a host backend to inspect the store safely.
class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual void resource_created(int64_t, const Resource&) {}
    virtual void resource_updated(int64_t, const Resource&) {}
    virtual void resource_destroyed(int64_t) {}
    virtual void state_changed(const ShaderStore&) {}
    virtual void draw(bool /*indexed*/, int64_t /*count*/) {}
    virtual void clear(float, float, float, float) {}
    virtual void present() {}
};

inline constexpr int32_t TOPO_TRIANGLE_LIST = 0;
inline constexpr int32_t TOPO_TRIANGLE_STRIP = 1;
inline constexpr int32_t TOPO_LINE_LIST = 2;
inline constexpr int32_t TOPO_LINE_STRIP = 3;
inline constexpr int32_t TOPO_POINT_LIST = 4;

void register_natives(std::unordered_map<std::string, NativeSig>& natives);

// Add TOPO_* as synthetic `global const i32` declarations before sema. This
// is idempotent and leaves a source-declared name untouched.
void inject_constants(Program& program);

void set_backend(RenderBackend* backend);
ShaderStore snapshot();
void reset();

} // namespace ember::ext_render
