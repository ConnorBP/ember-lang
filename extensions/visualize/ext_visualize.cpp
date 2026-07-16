// ext_visualize.cpp - bounded audio capture, radix-2 FFT, meters and exports.
#include "ext_visualize.hpp"

#include "ast.hpp"
#include "binding_builder.hpp"
#include "ext_array.hpp"
#include "ext_string.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace ember::ext_visualize {
namespace {
constexpr std::size_t kCapture = 2048;
constexpr float kPi = 3.14159265358979323846f;
struct Snapshot {
    std::array<std::atomic<float>, kCapture> samples {};
    std::atomic<std::size_t> count {0};
};
Snapshot g_snapshot;
struct ParamMetadata {
    std::vector<float> values, minimums, maximums;
    std::vector<std::string> names;
    std::string source_hash;
};
std::mutex g_meta_mutex;
ParamMetadata g_meta;

std::vector<float> snapshot() {
    const std::size_t n = std::min(g_snapshot.count.load(std::memory_order_acquire), kCapture);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = g_snapshot.samples[i].load(std::memory_order_relaxed);
    return out;
}

template<class Sample> void publish(Sample* const* channels, std::int64_t channelCount,
                                    std::int64_t frames) noexcept {
    if (!channels || channelCount <= 0 || frames <= 0) return;
    const std::size_t n = std::min<std::size_t>(std::size_t(frames), kCapture);
    const std::int64_t first = frames - static_cast<std::int64_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
        double sum = 0.0;
        std::int64_t used = 0;
        for (std::int64_t c = 0; c < channelCount; ++c) {
            if (channels[c]) { sum += channels[c][first + std::int64_t(i)]; ++used; }
        }
        g_snapshot.samples[i].store(
            used ? static_cast<float>(sum / double(used)) : 0.0f,
            std::memory_order_relaxed);
    }
    g_snapshot.count.store(n, std::memory_order_release);
}

std::size_t fft_size(std::size_t available) {
    for (std::size_t n : {std::size_t(1024), std::size_t(512), std::size_t(256)})
        if (available >= n) return n;
    return 0;
}
std::vector<float> spectrum(std::int64_t requested) {
    requested = std::clamp<std::int64_t>(requested, 1, 512);
    const auto input = snapshot();
    const std::size_t n = fft_size(input.size());
    std::vector<float> output(std::size_t(requested), 0.0f);
    if (!n) return output;
    std::vector<std::complex<float>> data(n);
    const std::size_t offset = input.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        const float window = 0.5f - 0.5f * std::cos(2.0f * kPi * float(i) / float(n - 1));
        data[i] = std::complex<float>(input[offset + i] * window, 0.0f);
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::complex<float> step(std::cos(-2.0f*kPi/float(len)), std::sin(-2.0f*kPi/float(len)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t j = 0; j < len/2; ++j) {
                const auto u = data[i+j], v = data[i+j+len/2] * w;
                data[i+j] = u+v; data[i+j+len/2] = u-v; w *= step;
            }
        }
    }
    const std::size_t sourceBins = n / 2;
    for (std::size_t b = 0; b < output.size(); ++b) {
        const std::size_t begin = b * sourceBins / output.size();
        const std::size_t end = std::max(begin + 1, (b + 1) * sourceBins / output.size());
        float peak = 0.0f;
        for (std::size_t i = begin; i < std::min(end, sourceBins); ++i)
            peak = std::max(peak, std::abs(data[i]) * (2.0f / float(n)));
        output[b] = peak;
    }
    return output;
}
std::vector<float> waveform(std::int64_t requested) {
    requested = std::clamp<std::int64_t>(requested, 1, 2048);
    const auto input = snapshot();
    std::vector<float> output(std::size_t(requested), 0.0f);
    if (input.empty()) return output;
    for (std::size_t i = 0; i < output.size(); ++i) {
        const std::size_t index = std::min(input.size()-1, i * input.size() / output.size());
        output[i] = input[index];
    }
    return output;
}
float rms() {
    const auto v = snapshot(); if (v.empty()) return 0.0f;
    double sum = 0.0; for (float x : v) sum += double(x)*x;
    return float(std::sqrt(sum / double(v.size())));
}
float peak() {
    const auto v = snapshot();
    float result = 0.0f;
    for (float x : v) result = std::max(result, std::abs(x));
    return result;
}
float zcr() {
    const auto v = snapshot(); if (v.size() < 2) return 0.0f;
    std::size_t crossings = 0;
    for (std::size_t i=1;i<v.size();++i) if ((v[i-1] < 0.0f) != (v[i] < 0.0f)) ++crossings;
    return float(crossings) / float(v.size()-1);
}
int64_t array_handle(const std::vector<float>& values) {
    return ext_array::alloc_f32(values.data(), static_cast<int64_t>(values.size()));
}
std::string csv(const std::vector<float>& values) {
    std::ostringstream out; out.precision(5);
    for (std::size_t i=0;i<values.size();++i) { if (i) out << ','; out << values[i]; }
    return out.str();
}
int64_t n_viz_get_spectrum(int64_t bins) { return array_handle(spectrum(bins)); }
int64_t n_viz_get_waveform(int64_t samples) { return array_handle(waveform(samples)); }
float n_viz_get_rms() { return rms(); }
float n_viz_get_peak() { return peak(); }
int64_t n_llm_export_spectrum(int64_t bins) { return ext_string::alloc(csv(spectrum(bins))); }
int64_t n_llm_export_waveform(int64_t samples) { return ext_string::alloc(csv(waveform(samples))); }
int64_t n_llm_export_param_summary() {
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    std::ostringstream out; out.precision(6);
    for (std::size_t i=0;i<g_meta.values.size();++i) {
        if (i) out << ';';
        out << g_meta.names[i] << '=' << g_meta.values[i]
            << '[' << g_meta.minimums[i] << ',' << g_meta.maximums[i] << ']';
    }
    return ext_string::alloc(out.str());
}
int64_t n_llm_export_state() {
    std::ostringstream out; out.precision(5);
    std::string hash;
    {
        std::lock_guard<std::mutex> lock(g_meta_mutex);
        hash = g_meta.source_hash;
        out << "{\"params\":{\"summary\":\"";
        for (std::size_t i=0;i<g_meta.values.size();++i) {
            if (i) out << ',';
            out << g_meta.names[i] << ':' << g_meta.values[i];
        }
    }
    out << "\"},\"spectrum\":[" << csv(spectrum(32)) << "],\"rms\":" << rms()
        << ",\"peak\":" << peak() << ",\"zcr\":" << zcr()
        << ",\"source_hash\":\"" << hash << "\"}";
    return ext_string::alloc(out.str());
}
} // namespace

void publish_f32(float* const* c, std::int64_t n, std::int64_t f) noexcept { publish(c,n,f); }
void publish_f64(double* const* c, std::int64_t n, std::int64_t f) noexcept { publish(c,n,f); }
void copy_spectrum(float* output, std::size_t bins) {
    if (!output) return;
    const auto values=spectrum(static_cast<int64_t>(bins));
    std::copy(values.begin(), values.end(), output);
}
void copy_waveform(float* output, std::size_t samples) {
    if (!output) return;
    const auto values=waveform(static_cast<int64_t>(samples));
    std::copy(values.begin(), values.end(), output);
}
float rms_value() { return rms(); }
float peak_value() { return peak(); }
void set_parameter_metadata(const float* values, const float* minimums,
                            const float* maximums, const char* const* names,
                            std::size_t count, std::string source_hash) {
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    g_meta.values.assign(values, values+count);
    g_meta.minimums.assign(minimums, minimums+count);
    g_meta.maximums.assign(maximums, maximums+count);
    g_meta.names.clear(); for (std::size_t i=0;i<count;++i) g_meta.names.emplace_back(names[i]);
    g_meta.source_hash = std::move(source_hash);
}
void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder b; const Type i64=type_i64(), f32=type_f32(), str=bind_handle("string");
    b.add("viz_get_spectrum",i64,{i64},(void*)&n_viz_get_spectrum,PERM_FFI);
    b.add("viz_get_waveform",i64,{i64},(void*)&n_viz_get_waveform,PERM_FFI);
    b.add("viz_get_rms",f32,{},(void*)&n_viz_get_rms,PERM_FFI);
    b.add("viz_get_peak",f32,{},(void*)&n_viz_get_peak,PERM_FFI);
    b.add("llm_export_state",str,{},(void*)&n_llm_export_state,PERM_FFI);
    b.add("llm_export_spectrum",str,{i64},(void*)&n_llm_export_spectrum,PERM_FFI);
    b.add("llm_export_waveform",str,{i64},(void*)&n_llm_export_waveform,PERM_FFI);
    b.add("llm_export_param_summary",str,{},(void*)&n_llm_export_param_summary,PERM_FFI);
    NativeTable table=b.build(); for(auto& item:table.natives)natives[item.first]=std::move(item.second);
}
} // namespace ember::ext_visualize
