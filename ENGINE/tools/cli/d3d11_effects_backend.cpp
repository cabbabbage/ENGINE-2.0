#include "d3d11_effects_backend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace imgcache {

namespace {

static inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

constexpr UINT kThreadGroupSize = 16;
constexpr UINT kMaxBlurWeights = 256;

struct ColorParamsCB {
    float brightness = 0.0f;
    float contrast = 0.0f;
    float hue_offset = 0.0f;
    float do_color = 0.0f;
    float sat_r = 0.0f;
    float sat_g = 0.0f;
    float sat_b = 0.0f;
    float pad0 = 0.0f;
    UINT width = 0;
    UINT height = 0;
    UINT pad1 = 0;
    UINT pad2 = 0;
};

struct BlurParamsCB {
    UINT width = 0;
    UINT height = 0;
    UINT radius = 0;
    UINT horizontal = 0;
    float weights[kMaxBlurWeights] = {};
};

struct UnsharpParamsCB {
    UINT width = 0;
    UINT height = 0;
    float scale = 1.0f;
    float threshold = 0.0f;
};

struct DimsParamsCB {
    UINT width = 0;
    UINT height = 0;
    float factor = 1.0f;
    float pad0 = 0.0f;
};

static_assert((sizeof(ColorParamsCB) % 16) == 0, "ColorParamsCB must be 16-byte aligned");
static_assert((sizeof(BlurParamsCB) % 16) == 0, "BlurParamsCB must be 16-byte aligned");
static_assert((sizeof(UnsharpParamsCB) % 16) == 0, "UnsharpParamsCB must be 16-byte aligned");
static_assert((sizeof(DimsParamsCB) % 16) == 0, "DimsParamsCB must be 16-byte aligned");

struct GpuTexture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    UINT w = 0;
    UINT h = 0;

    bool valid() const { return tex != nullptr && srv != nullptr && uav != nullptr; }
};

constexpr const char* kColorShader = R"(
cbuffer ColorParams : register(b0)
{
    float brightness;
    float contrast;
    float hue_offset;
    float do_color;
    float sat_r;
    float sat_g;
    float sat_b;
    float _pad0;
    uint width;
    uint height;
    uint _pad1;
    uint _pad2;
};

Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

float sat_factor(float s) {
    return clamp(1.0 + 2.0 * s, 0.0, 3.0);
}

void rgb_to_hsv(float3 rgb, out float h, out float s, out float v) {
    float maxc = max(rgb.r, max(rgb.g, rgb.b));
    float minc = min(rgb.r, min(rgb.g, rgb.b));
    v = maxc;
    float delta = maxc - minc;
    s = (maxc > 0.0) ? (delta / maxc) : 0.0;
    h = 0.0;
    if (delta > 1e-6) {
        if (maxc == rgb.r) {
            float hr = (rgb.g - rgb.b) / max(delta, 1e-6);
            hr = fmod(hr, 6.0);
            if (hr < 0.0) hr += 6.0;
            h = hr / 6.0;
        } else if (maxc == rgb.g) {
            h = (((rgb.b - rgb.r) / max(delta, 1e-6)) + 2.0) / 6.0;
        } else {
            h = (((rgb.r - rgb.g) / max(delta, 1e-6)) + 4.0) / 6.0;
        }
        if (h < 0.0) h += 1.0;
        if (h >= 1.0) h -= floor(h);
    }
}

float3 hsv_to_rgb(float h, float s, float v) {
    float h6 = h * 6.0;
    float c = v * s;
    float x = c * (1.0 - abs(fmod(h6, 2.0) - 1.0));
    float m = v - c;
    float3 rgb = float3(0.0, 0.0, 0.0);
    if (0.0 <= h6 && h6 < 1.0) rgb = float3(c, x, 0.0);
    else if (1.0 <= h6 && h6 < 2.0) rgb = float3(x, c, 0.0);
    else if (2.0 <= h6 && h6 < 3.0) rgb = float3(0.0, c, x);
    else if (3.0 <= h6 && h6 < 4.0) rgb = float3(0.0, x, c);
    else if (4.0 <= h6 && h6 < 5.0) rgb = float3(x, 0.0, c);
    else rgb = float3(c, 0.0, x);
    return rgb + float3(m, m, m);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    int2 pxy = int2(tid.xy);
    float4 px = gInput[pxy];
    if (px.a > 0.0 && do_color > 0.5) {
        float3 rgb = px.rgb;
        rgb = clamp(rgb + float3(brightness, brightness, brightness), 0.0, 1.0);
        float c = 1.0 + contrast;
        rgb = clamp((rgb - 0.5) * c + 0.5, 0.0, 1.0);

        float h, s, v;
        rgb_to_hsv(rgb, h, s, v);
        h = fmod(h + hue_offset, 1.0);
        if (h < 0.0) h += 1.0;
        rgb = hsv_to_rgb(h, s, v);

        float gray = (rgb.r + rgb.g + rgb.b) / 3.0;
        rgb.r = clamp(gray + (rgb.r - gray) * sat_factor(sat_r), 0.0, 1.0);
        rgb.g = clamp(gray + (rgb.g - gray) * sat_factor(sat_g), 0.0, 1.0);
        rgb.b = clamp(gray + (rgb.b - gray) * sat_factor(sat_b), 0.0, 1.0);
        px.rgb = rgb;
    }
    gOutput[pxy] = px;
}
)";

constexpr const char* kGaussianShader = R"(
cbuffer BlurParams : register(b0)
{
    uint width;
    uint height;
    uint radius;
    uint horizontal;
    float weights[256];
};

Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int k = -int(radius); k <= int(radius); ++k) {
        int sx = int(tid.x) + ((horizontal != 0) ? k : 0);
        int sy = int(tid.y) + ((horizontal == 0) ? k : 0);
        sx = clamp(sx, 0, int(width) - 1);
        sy = clamp(sy, 0, int(height) - 1);
        float w = weights[k + int(radius)];
        acc += gInput[int2(sx, sy)] * w;
    }
    gOutput[int2(tid.xy)] = acc;
}
)";

constexpr const char* kUnsharpShader = R"(
cbuffer UnsharpParams : register(b0)
{
    uint width;
    uint height;
    float scale;
    float threshold;
};

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gBlur : register(t1);
RWTexture2D<float4> gOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    int2 pxy = int2(tid.xy);
    float4 s = gSource[pxy];
    float4 b = gBlur[pxy];
    float4 o = s;
    float3 d = s.rgb - b.rgb;
    o.r = (abs(d.r) < threshold) ? s.r : saturate(s.r + d.r * scale);
    o.g = (abs(d.g) < threshold) ? s.g : saturate(s.g + d.g * scale);
    o.b = (abs(d.b) < threshold) ? s.b : saturate(s.b + d.b * scale);
    o.a = s.a;
    gOutput[pxy] = o;
}
)";

constexpr const char* kMaskShader = R"(
cbuffer DimsParams : register(b0)
{
    uint width;
    uint height;
    float factor;
    float _pad0;
};

Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    int2 pxy = int2(tid.xy);
    float4 p = gInput[pxy];
    float lum = 0.299 * p.r + 0.587 * p.g + 0.114 * p.b;
    int L = int(round(lum * 255.0));
    int m = (L < 170) ? 0 : min(255, (L - 170) * 3);
    float mf = float(m) / 255.0;
    gOutput[pxy] = float4(mf, mf, mf, 1.0);
}
)";

constexpr const char* kBrightenShader = R"(
cbuffer DimsParams : register(b0)
{
    uint width;
    uint height;
    float factor;
    float _pad0;
};

Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    int2 pxy = int2(tid.xy);
    float4 p = gInput[pxy];
    p.rgb = saturate(p.rgb * factor);
    gOutput[pxy] = p;
}
)";

constexpr const char* kCompositeShader = R"(
cbuffer DimsParams : register(b0)
{
    uint width;
    uint height;
    float factor;
    float _pad0;
};

Texture2D<float4> gA : register(t0);
Texture2D<float4> gB : register(t1);
Texture2D<float4> gMask : register(t2);
RWTexture2D<float4> gOutput : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    int2 pxy = int2(tid.xy);
    float4 a = gA[pxy];
    float4 b = gB[pxy];
    float m = saturate(gMask[pxy].r);
    gOutput[pxy] = a * m + b * (1.0 - m);
}
)";

static inline std::vector<float> build_gaussian_kernel_cached(float sigma) {
    sigma = std::max(0.01f, sigma);
    const int key = static_cast<int>(std::lround(sigma * 1000.0f));
    static std::mutex mu;
    static std::unordered_map<int, std::vector<float>> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    const int radius = static_cast<int>(std::ceil(3.0f * sigma));
    const int size = radius * 2 + 1;
    std::vector<float> k(size, 0.0f);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float x = static_cast<float>(i);
        const float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        k[static_cast<size_t>(i + radius)] = w;
        sum += w;
    }
    if (sum < 1e-8f) sum = 1.0f;
    for (float& v : k) v /= sum;

    {
        std::lock_guard<std::mutex> lk(mu);
        cache.emplace(key, k);
    }
    return k;
}

class D3D11BackendState final {
public:
    bool IsAvailable(std::string& reason) {
        std::lock_guard<std::mutex> lk(mu_);
        ensure_initialized_locked();
        reason = init_reason_;
        return available_;
    }

    bool ApplyEffects(const ImageRGBA& expanded_src,
                      const EffectsParams& fx,
                      EffectLayerMode mode,
                      ImageRGBA& out,
                      std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        ensure_initialized_locked();
        if (!available_) {
            err = init_reason_.empty() ? "D3D11 backend unavailable" : init_reason_;
            return false;
        }
        return apply_locked(expanded_src, fx, mode, out, err);
    }

private:
    void ensure_initialized_locked() {
        if (init_attempted_) return;
        init_attempted_ = true;
        init_reason_.clear();

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level_out = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &level_out,
            context_.GetAddressOf());
        if (FAILED(hr) || !device_ || !context_) {
            init_reason_ = "Failed to create D3D11 hardware device.";
            available_ = false;
            return;
        }

        if (!create_compute_shader(kColorShader, cs_color_, "color", init_reason_) ||
            !create_compute_shader(kGaussianShader, cs_gaussian_, "gaussian", init_reason_) ||
            !create_compute_shader(kUnsharpShader, cs_unsharp_, "unsharp", init_reason_) ||
            !create_compute_shader(kMaskShader, cs_mask_, "mask", init_reason_) ||
            !create_compute_shader(kBrightenShader, cs_brighten_, "brighten", init_reason_) ||
            !create_compute_shader(kCompositeShader, cs_composite_, "composite", init_reason_)) {
            available_ = false;
            return;
        }

        if (!create_constant_buffer(sizeof(ColorParamsCB), cb_color_, init_reason_) ||
            !create_constant_buffer(sizeof(BlurParamsCB), cb_blur_, init_reason_) ||
            !create_constant_buffer(sizeof(UnsharpParamsCB), cb_unsharp_, init_reason_) ||
            !create_constant_buffer(sizeof(DimsParamsCB), cb_dims_, init_reason_)) {
            available_ = false;
            return;
        }

        available_ = true;
        init_reason_.clear();
    }

    bool create_constant_buffer(UINT byte_width, ComPtr<ID3D11Buffer>& out, std::string& err) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = (byte_width + 15u) & ~15u;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = 0;
        HRESULT hr = device_->CreateBuffer(&desc, nullptr, out.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            err = "Failed to create constant buffer.";
            return false;
        }
        return true;
    }

    bool create_compute_shader(const char* source,
                               ComPtr<ID3D11ComputeShader>& out,
                               const char* debug_name,
                               std::string& err) {
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr = D3DCompile(
            source,
            std::strlen(source),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            flags,
            0,
            blob.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(hr) || !blob) {
            err = std::string("Failed to compile D3D11 compute shader '") + debug_name + "'";
            if (errors && errors->GetBufferPointer()) {
                err += ": ";
                err += static_cast<const char*>(errors->GetBufferPointer());
            }
            return false;
        }
        hr = device_->CreateComputeShader(blob->GetBufferPointer(),
                                          blob->GetBufferSize(),
                                          nullptr,
                                          out.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !out) {
            err = std::string("Failed to create D3D11 compute shader '") + debug_name + "'";
            return false;
        }
        return true;
    }

    bool create_gpu_texture(UINT w,
                            UINT h,
                            const std::uint8_t* initial_rgba,
                            GpuTexture& out,
                            std::string& err) {
        out = {};

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA init_data{};
        D3D11_SUBRESOURCE_DATA* init_ptr = nullptr;
        if (initial_rgba) {
            init_data.pSysMem = initial_rgba;
            init_data.SysMemPitch = static_cast<UINT>(w * 4u);
            init_ptr = &init_data;
        }

        HRESULT hr = device_->CreateTexture2D(&desc, init_ptr, out.tex.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !out.tex) {
            err = "Failed to create D3D11 texture.";
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(out.tex.Get(), &srv_desc, out.srv.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !out.srv) {
            err = "Failed to create D3D11 texture SRV.";
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = desc.Format;
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uav_desc.Texture2D.MipSlice = 0;
        hr = device_->CreateUnorderedAccessView(out.tex.Get(), &uav_desc, out.uav.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !out.uav) {
            err = "Failed to create D3D11 texture UAV.";
            return false;
        }

        out.w = w;
        out.h = h;
        return true;
    }

    bool readback_texture(const GpuTexture& src, ImageRGBA& out, std::string& err) {
        if (!src.valid()) {
            err = "Invalid GPU source texture for readback.";
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        src.tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        if (FAILED(hr) || !staging) {
            err = "Failed to create D3D11 staging texture for readback.";
            return false;
        }

        context_->CopyResource(staging.Get(), src.tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            err = "Failed to map staging texture for readback.";
            return false;
        }

        out.w = static_cast<int>(src.w);
        out.h = static_cast<int>(src.h);
        out.pixels.assign(static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 4u, 0);
        const size_t dst_pitch = static_cast<size_t>(src.w) * 4u;
        const auto* src_bytes = static_cast<const std::uint8_t*>(mapped.pData);
        for (UINT y = 0; y < src.h; ++y) {
            std::memcpy(out.pixels.data() + static_cast<size_t>(y) * dst_pitch,
                        src_bytes + static_cast<size_t>(y) * mapped.RowPitch,
                        dst_pitch);
        }

        context_->Unmap(staging.Get(), 0);
        return out.valid();
    }

    void clear_bindings() {
        ID3D11ShaderResourceView* null_srvs[3] = {nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView* null_uavs[1] = {nullptr};
        context_->CSSetShaderResources(0, 3, null_srvs);
        context_->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
        context_->CSSetShader(nullptr, nullptr, 0);
    }

    bool dispatch_1to1(ID3D11ComputeShader* shader,
                       ID3D11Buffer* cb,
                       const std::array<ID3D11ShaderResourceView*, 3>& srvs,
                       UINT srv_count,
                       ID3D11UnorderedAccessView* uav,
                       UINT width,
                       UINT height,
                       std::string& err) {
        if (!shader || !uav) {
            err = "Invalid D3D11 dispatch resources.";
            return false;
        }
        context_->CSSetShader(shader, nullptr, 0);
        if (cb) {
            context_->CSSetConstantBuffers(0, 1, &cb);
        }
        context_->CSSetShaderResources(0, srv_count, srvs.data());
        context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

        const UINT gx = (width + kThreadGroupSize - 1u) / kThreadGroupSize;
        const UINT gy = (height + kThreadGroupSize - 1u) / kThreadGroupSize;
        context_->Dispatch(gx, gy, 1);
        clear_bindings();
        return true;
    }

    bool run_color(const GpuTexture& in_tex,
                   const EffectsParams& fx,
                   bool do_color,
                   GpuTexture& out_tex,
                   std::string& err) {
        ColorParamsCB params{};
        params.brightness = clampf(fx.brightness, -1.0f, 1.0f);
        params.contrast = clampf(fx.contrast, -1.0f, 1.0f);
        params.hue_offset = clampf(fx.hue_shift, -180.0f, 180.0f) / 360.0f;
        params.do_color = do_color ? 1.0f : 0.0f;
        params.sat_r = clampf(fx.saturation_r, -1.0f, 1.0f);
        params.sat_g = clampf(fx.saturation_g, -1.0f, 1.0f);
        params.sat_b = clampf(fx.saturation_b, -1.0f, 1.0f);
        params.width = in_tex.w;
        params.height = in_tex.h;
        context_->UpdateSubresource(cb_color_.Get(), 0, nullptr, &params, 0, 0);

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        srvs[0] = in_tex.srv.Get();
        return dispatch_1to1(cs_color_.Get(),
                             cb_color_.Get(),
                             srvs,
                             1,
                             out_tex.uav.Get(),
                             in_tex.w,
                             in_tex.h,
                             err);
    }

    bool run_gaussian(const GpuTexture& in_tex, float sigma, GpuTexture& out_tex, std::string& err) {
        std::vector<float> kernel = build_gaussian_kernel_cached(sigma);
        if (kernel.empty() || kernel.size() > kMaxBlurWeights) {
            err = "Gaussian kernel size out of supported range for D3D11 backend.";
            return false;
        }

        GpuTexture temp;
        if (!create_gpu_texture(in_tex.w, in_tex.h, nullptr, temp, err)) return false;

        BlurParamsCB params{};
        params.width = in_tex.w;
        params.height = in_tex.h;
        params.radius = static_cast<UINT>(kernel.size() / 2u);
        for (size_t i = 0; i < kernel.size(); ++i) {
            params.weights[i] = kernel[i];
        }

        params.horizontal = 1;
        context_->UpdateSubresource(cb_blur_.Get(), 0, nullptr, &params, 0, 0);
        {
            std::array<ID3D11ShaderResourceView*, 3> srvs{};
            srvs[0] = in_tex.srv.Get();
            if (!dispatch_1to1(cs_gaussian_.Get(),
                               cb_blur_.Get(),
                               srvs,
                               1,
                               temp.uav.Get(),
                               in_tex.w,
                               in_tex.h,
                               err)) {
                return false;
            }
        }

        params.horizontal = 0;
        context_->UpdateSubresource(cb_blur_.Get(), 0, nullptr, &params, 0, 0);
        {
            std::array<ID3D11ShaderResourceView*, 3> srvs{};
            srvs[0] = temp.srv.Get();
            if (!dispatch_1to1(cs_gaussian_.Get(),
                               cb_blur_.Get(),
                               srvs,
                               1,
                               out_tex.uav.Get(),
                               in_tex.w,
                               in_tex.h,
                               err)) {
                return false;
            }
        }

        return true;
    }

    bool run_unsharp(const GpuTexture& source_tex,
                     const GpuTexture& blur_tex,
                     float scale,
                     float threshold,
                     GpuTexture& out_tex,
                     std::string& err) {
        UnsharpParamsCB params{};
        params.width = source_tex.w;
        params.height = source_tex.h;
        params.scale = std::max(0.0f, scale);
        params.threshold = std::max(0.0f, threshold);
        context_->UpdateSubresource(cb_unsharp_.Get(), 0, nullptr, &params, 0, 0);

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        srvs[0] = source_tex.srv.Get();
        srvs[1] = blur_tex.srv.Get();
        return dispatch_1to1(cs_unsharp_.Get(),
                             cb_unsharp_.Get(),
                             srvs,
                             2,
                             out_tex.uav.Get(),
                             source_tex.w,
                             source_tex.h,
                             err);
    }

    bool run_mask(const GpuTexture& source_tex, GpuTexture& out_tex, std::string& err) {
        DimsParamsCB params{};
        params.width = source_tex.w;
        params.height = source_tex.h;
        params.factor = 1.0f;
        context_->UpdateSubresource(cb_dims_.Get(), 0, nullptr, &params, 0, 0);

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        srvs[0] = source_tex.srv.Get();
        return dispatch_1to1(cs_mask_.Get(),
                             cb_dims_.Get(),
                             srvs,
                             1,
                             out_tex.uav.Get(),
                             source_tex.w,
                             source_tex.h,
                             err);
    }

    bool run_brighten(const GpuTexture& source_tex, float factor, GpuTexture& out_tex, std::string& err) {
        DimsParamsCB params{};
        params.width = source_tex.w;
        params.height = source_tex.h;
        params.factor = factor;
        context_->UpdateSubresource(cb_dims_.Get(), 0, nullptr, &params, 0, 0);

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        srvs[0] = source_tex.srv.Get();
        return dispatch_1to1(cs_brighten_.Get(),
                             cb_dims_.Get(),
                             srvs,
                             1,
                             out_tex.uav.Get(),
                             source_tex.w,
                             source_tex.h,
                             err);
    }

    bool run_composite(const GpuTexture& a_tex,
                       const GpuTexture& b_tex,
                       const GpuTexture& mask_tex,
                       GpuTexture& out_tex,
                       std::string& err) {
        DimsParamsCB params{};
        params.width = a_tex.w;
        params.height = a_tex.h;
        params.factor = 1.0f;
        context_->UpdateSubresource(cb_dims_.Get(), 0, nullptr, &params, 0, 0);

        std::array<ID3D11ShaderResourceView*, 3> srvs{};
        srvs[0] = a_tex.srv.Get();
        srvs[1] = b_tex.srv.Get();
        srvs[2] = mask_tex.srv.Get();
        return dispatch_1to1(cs_composite_.Get(),
                             cb_dims_.Get(),
                             srvs,
                             3,
                             out_tex.uav.Get(),
                             a_tex.w,
                             a_tex.h,
                             err);
    }

    bool apply_locked(const ImageRGBA& expanded_src,
                      const EffectsParams& fx,
                      EffectLayerMode mode,
                      ImageRGBA& out,
                      std::string& err) {
        err.clear();
        if (!expanded_src.valid()) {
            err = "D3D11 ApplyEffects: invalid source image.";
            return false;
        }

        const UINT w = static_cast<UINT>(expanded_src.w);
        const UINT h = static_cast<UINT>(expanded_src.h);

        GpuTexture input_tex;
        if (!create_gpu_texture(w, h, expanded_src.pixels.data(), input_tex, err)) {
            return false;
        }

        const float brightness = clampf(fx.brightness, -1.0f, 1.0f);
        const float contrast = clampf(fx.contrast, -1.0f, 1.0f);
        const float sat_r = clampf(fx.saturation_r, -1.0f, 1.0f);
        const float sat_g = clampf(fx.saturation_g, -1.0f, 1.0f);
        const float sat_b = clampf(fx.saturation_b, -1.0f, 1.0f);
        const float hue = clampf(fx.hue_shift, -180.0f, 180.0f);
        const bool do_color = (std::fabs(brightness) >= 1e-6f) ||
                              (std::fabs(contrast) >= 1e-6f) ||
                              (std::fabs(sat_r) >= 1e-6f) ||
                              (std::fabs(sat_g) >= 1e-6f) ||
                              (std::fabs(sat_b) >= 1e-6f) ||
                              (std::fabs(hue) >= 1e-6f);

        GpuTexture color_tex;
        if (!create_gpu_texture(w, h, nullptr, color_tex, err)) return false;
        if (!run_color(input_tex, fx, do_color, color_tex, err)) return false;

        const float blur_val = clampf(fx.blur_radius, -1.0f, 1.0f);
        if (std::fabs(blur_val) < 1e-3f) {
            return readback_texture(color_tex, out, err);
        }

        const bool is_foreground = (mode == EffectLayerMode::Foreground);
        if (blur_val > 0.0f) {
            const float max_radius = 20.0f;
            float base_radius = blur_val * max_radius;
            if (base_radius < 1.0f) base_radius = 1.0f;

            if (is_foreground) {
                const float radius = base_radius * 2.0f;
                GpuTexture blurred;
                if (!create_gpu_texture(w, h, nullptr, blurred, err)) return false;
                if (!run_gaussian(color_tex, radius, blurred, err)) return false;

                const float ring_radius = std::max(1.0f, radius * 0.5f);
                GpuTexture ring_blur;
                if (!create_gpu_texture(w, h, nullptr, ring_blur, err)) return false;
                if (!run_gaussian(blurred, ring_radius, ring_blur, err)) return false;

                GpuTexture ring_out;
                if (!create_gpu_texture(w, h, nullptr, ring_out, err)) return false;
                if (!run_unsharp(blurred, ring_blur, 0.8f, 3.0f / 255.0f, ring_out, err)) return false;

                return readback_texture(ring_out, out, err);
            }

            const float radius = base_radius * 1.3f;
            GpuTexture blurred;
            if (!create_gpu_texture(w, h, nullptr, blurred, err)) return false;
            if (!run_gaussian(color_tex, radius, blurred, err)) return false;

            GpuTexture mask_tex;
            if (!create_gpu_texture(w, h, nullptr, mask_tex, err)) return false;
            if (!run_mask(color_tex, mask_tex, err)) return false;

            const float mask_sigma = std::max(1.0f, radius * 0.8f);
            GpuTexture mask_blur;
            if (!create_gpu_texture(w, h, nullptr, mask_blur, err)) return false;
            if (!run_gaussian(mask_tex, mask_sigma, mask_blur, err)) return false;

            GpuTexture bright_tex;
            if (!create_gpu_texture(w, h, nullptr, bright_tex, err)) return false;
            if (!run_brighten(blurred, 1.4f, bright_tex, err)) return false;

            GpuTexture out_tex;
            if (!create_gpu_texture(w, h, nullptr, out_tex, err)) return false;
            if (!run_composite(bright_tex, blurred, mask_blur, out_tex, err)) return false;

            return readback_texture(out_tex, out, err);
        }

        const float strength = -blur_val;
        const float radius = 0.7f + strength * 3.3f;
        const float percent = 80.0f + strength * 220.0f;

        GpuTexture blur_tex;
        if (!create_gpu_texture(w, h, nullptr, blur_tex, err)) return false;
        if (!run_gaussian(color_tex, radius, blur_tex, err)) return false;

        GpuTexture out_tex;
        if (!create_gpu_texture(w, h, nullptr, out_tex, err)) return false;
        if (!run_unsharp(color_tex, blur_tex, percent / 100.0f, 0.0f, out_tex, err)) return false;
        return readback_texture(out_tex, out, err);
    }

private:
    std::mutex mu_;
    bool init_attempted_ = false;
    bool available_ = false;
    std::string init_reason_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    ComPtr<ID3D11ComputeShader> cs_color_;
    ComPtr<ID3D11ComputeShader> cs_gaussian_;
    ComPtr<ID3D11ComputeShader> cs_unsharp_;
    ComPtr<ID3D11ComputeShader> cs_mask_;
    ComPtr<ID3D11ComputeShader> cs_brighten_;
    ComPtr<ID3D11ComputeShader> cs_composite_;

    ComPtr<ID3D11Buffer> cb_color_;
    ComPtr<ID3D11Buffer> cb_blur_;
    ComPtr<ID3D11Buffer> cb_unsharp_;
    ComPtr<ID3D11Buffer> cb_dims_;
};

static D3D11BackendState& backend_state() {
    static D3D11BackendState state;
    return state;
}

#endif // _WIN32

} // namespace

D3D11EffectsBackend::D3D11EffectsBackend() = default;
D3D11EffectsBackend::~D3D11EffectsBackend() = default;

D3D11EffectsBackend& D3D11EffectsBackend::Instance() {
    static D3D11EffectsBackend instance;
    return instance;
}

bool D3D11EffectsBackend::IsAvailable(std::string& reason) {
#ifdef _WIN32
    return backend_state().IsAvailable(reason);
#else
    reason = "D3D11 backend is only available on Windows.";
    return false;
#endif
}

bool D3D11EffectsBackend::ApplyEffects(const ImageRGBA& expanded_src,
                                       const EffectsParams& fx,
                                       EffectLayerMode mode,
                                       ImageRGBA& out,
                                       std::string& err) {
#ifdef _WIN32
    return backend_state().ApplyEffects(expanded_src, fx, mode, out, err);
#else
    (void)expanded_src;
    (void)fx;
    (void)mode;
    (void)out;
    err = "D3D11 backend is only available on Windows.";
    return false;
#endif
}

} // namespace imgcache
