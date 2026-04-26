#include "rendering/render/engine_renderer.hpp"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <utility>
#include <vector>

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#ifdef _WIN32
#include <dxgi.h>
#include <windows.h>
#endif

namespace {
struct SystemGpuInfo {
    std::string name;
    std::uint64_t dedicated_video_memory = 0;
    std::uint64_t shared_system_memory = 0;
    std::uint32_t vendor_id = 0;
    bool is_software = false;
    bool likely_dedicated = false;
};

struct GpuSelectionContext {
    std::vector<SystemGpuInfo> detected_gpus;
    std::vector<std::string> available_gpu_drivers;
    std::string preferred_gpu_name;
    bool has_dedicated_preference = false;
};

std::string safe_string(const char* value) {
    return value ? std::string(value) : std::string();
}

std::string to_lower_copy(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const unsigned char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

std::string normalize_gpu_id(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized;
}

bool contains_token(const std::string& value, const char* token) {
    return value.find(token) != std::string::npos;
}

bool looks_like_integrated_name(const std::string& name) {
    const std::string lowered = to_lower_copy(name);
    return contains_token(lowered, "intel") ||
           contains_token(lowered, "uhd") ||
           contains_token(lowered, "iris") ||
           contains_token(lowered, "radeon(tm) graphics") ||
           contains_token(lowered, "vega") ||
           contains_token(lowered, "apu");
}

bool looks_like_dedicated_name(const std::string& name) {
    const std::string lowered = to_lower_copy(name);
    return contains_token(lowered, "nvidia") ||
           contains_token(lowered, "geforce") ||
           contains_token(lowered, "rtx") ||
           contains_token(lowered, "gtx") ||
           contains_token(lowered, "quadro") ||
           contains_token(lowered, "radeon rx") ||
           contains_token(lowered, "radeon pro") ||
           contains_token(lowered, "intel arc") ||
           contains_token(lowered, "arc a") ||
           contains_token(lowered, "arc b");
}

bool names_match_relaxed(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    const std::string normalized_lhs = normalize_gpu_id(lhs);
    const std::string normalized_rhs = normalize_gpu_id(rhs);
    if (normalized_lhs.empty() || normalized_rhs.empty()) {
        return false;
    }
    if (normalized_lhs == normalized_rhs) {
        return true;
    }
    return normalized_lhs.find(normalized_rhs) != std::string::npos ||
           normalized_rhs.find(normalized_lhs) != std::string::npos;
}

bool is_likely_dedicated_gpu(const std::string& name, std::uint64_t dedicated_video_memory, std::uint32_t vendor_id) {
    constexpr std::uint64_t kMb = 1024ull * 1024ull;
    constexpr std::uint64_t kGb = 1024ull * 1024ull * 1024ull;

    if (vendor_id == 0x10DE) {  // NVIDIA
        return dedicated_video_memory >= (1ull * kGb);
    }
    if (vendor_id == 0x8086) {  // Intel
        return looks_like_dedicated_name(name) && dedicated_video_memory >= (1ull * kGb);
    }
    if (vendor_id == 0x1002) {  // AMD
        if (looks_like_integrated_name(name) && dedicated_video_memory < (2ull * kGb)) {
            return false;
        }
        if (looks_like_dedicated_name(name) && dedicated_video_memory >= (1ull * kGb)) {
            return true;
        }
    }

    if (looks_like_integrated_name(name) && dedicated_video_memory < (2ull * kGb)) {
        return false;
    }
    if (dedicated_video_memory >= (2ull * kGb)) {
        return true;
    }
    return looks_like_dedicated_name(name) && dedicated_video_memory >= (512ull * kMb);
}

bool is_better_gpu_candidate(const SystemGpuInfo& candidate, const SystemGpuInfo& current_best) {
    if (candidate.likely_dedicated != current_best.likely_dedicated) {
        return candidate.likely_dedicated;
    }
    if (candidate.dedicated_video_memory != current_best.dedicated_video_memory) {
        return candidate.dedicated_video_memory > current_best.dedicated_video_memory;
    }
    if (candidate.shared_system_memory != current_best.shared_system_memory) {
        return candidate.shared_system_memory > current_best.shared_system_memory;
    }
    return candidate.name < current_best.name;
}

std::string format_memory_mb(std::uint64_t bytes) {
    std::ostringstream oss;
    oss << (bytes / (1024ull * 1024ull)) << "MB";
    return oss.str();
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* input) {
    if (!input || !input[0]) {
        return {};
    }
    const int required_size = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
    if (required_size <= 1) {
        return {};
    }
    std::string output(static_cast<size_t>(required_size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input, -1, output.data(), required_size, nullptr, nullptr);
    output.pop_back();  // הסר תו NUL מסיים.
    return output;
}

std::vector<SystemGpuInfo> detect_windows_gpus() {
    std::vector<SystemGpuInfo> gpus;

    HMODULE dxgi_module = LoadLibraryA("dxgi.dll");
    if (!dxgi_module) {
        return gpus;
    }

    using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto create_dxgi_factory = reinterpret_cast<CreateDXGIFactory1Fn>(
        GetProcAddress(dxgi_module, "CreateDXGIFactory1"));
    if (!create_dxgi_factory) {
        FreeLibrary(dxgi_module);
        return gpus;
    }

    IDXGIFactory1* factory = nullptr;
    const HRESULT hr = create_dxgi_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        FreeLibrary(dxgi_module);
        return gpus;
    }

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT enum_hr = factory->EnumAdapters1(index, &adapter);
        if (enum_hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enum_hr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            SystemGpuInfo info{};
            info.name = wide_to_utf8(desc.Description);
            info.dedicated_video_memory = desc.DedicatedVideoMemory;
            info.shared_system_memory = desc.SharedSystemMemory;
            info.vendor_id = desc.VendorId;
            info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.likely_dedicated = !info.is_software &&
                                    is_likely_dedicated_gpu(info.name, info.dedicated_video_memory, info.vendor_id);
            gpus.push_back(std::move(info));
        }
        adapter->Release();
    }

    factory->Release();
    FreeLibrary(dxgi_module);
    return gpus;
}
#endif

std::vector<std::string> enumerate_gpu_drivers() {
    std::vector<std::string> drivers;
    const int count = SDL_GetNumGPUDrivers();
    for (int i = 0; i < count; ++i) {
        if (const char* name = SDL_GetGPUDriver(i)) {
            drivers.emplace_back(name);
        }
    }
    return drivers;
}

bool has_driver(const std::vector<std::string>& drivers, const char* target_driver) {
    if (!target_driver || !target_driver[0]) {
        return false;
    }
    const std::string target = to_lower_copy(target_driver);
    for (const std::string& entry : drivers) {
        if (to_lower_copy(entry) == target) {
            return true;
        }
    }
    return false;
}

std::string query_renderer_gpu_name(SDL_Renderer* renderer) {
    if (!renderer) {
        return {};
    }

    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (!props) {
        return safe_string(SDL_GetRendererName(renderer));
    }

    auto* gpu_device = static_cast<SDL_GPUDevice*>(
        SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
    if (gpu_device) {
        SDL_PropertiesID gpu_props = SDL_GetGPUDeviceProperties(gpu_device);
        if (gpu_props) {
            if (const char* gpu_name = SDL_GetStringProperty(gpu_props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr)) {
                return std::string(gpu_name);
            }
        }
    }

    return safe_string(SDL_GetRendererName(renderer));
}

GpuSelectionContext detect_gpu_selection_context() {
    GpuSelectionContext context{};
    context.available_gpu_drivers = enumerate_gpu_drivers();

#ifdef _WIN32
    context.detected_gpus = detect_windows_gpus();
    int best_index = -1;
    for (size_t i = 0; i < context.detected_gpus.size(); ++i) {
        const SystemGpuInfo& gpu = context.detected_gpus[i];
        if (gpu.is_software) {
            continue;
        }
        if (best_index < 0 || is_better_gpu_candidate(gpu, context.detected_gpus[static_cast<size_t>(best_index)])) {
            best_index = static_cast<int>(i);
        }
    }
    if (best_index >= 0) {
        const SystemGpuInfo& selected = context.detected_gpus[static_cast<size_t>(best_index)];
        context.preferred_gpu_name = selected.name;
        context.has_dedicated_preference = selected.likely_dedicated;
    }
#endif

    return context;
}

void log_gpu_selection_context(const GpuSelectionContext& context) {
    std::ostringstream drivers;
    drivers << "[EngineRenderer] דרייברי GPU זמינים:";
    if (context.available_gpu_drivers.empty()) {
        drivers << " אין";
    } else {
        for (const std::string& driver : context.available_gpu_drivers) {
            drivers << ' ' << driver;
        }
    }
    vibble::log::info(drivers.str());

#ifdef _WIN32
    if (context.detected_gpus.empty()) {
        vibble::log::warn("[EngineRenderer] זיהוי GPU דרך DXGI לא מצא מתאמים.");
    } else {
        for (size_t i = 0; i < context.detected_gpus.size(); ++i) {
            const SystemGpuInfo& gpu = context.detected_gpus[i];
            std::ostringstream line;
            line << "[EngineRenderer] GPU[" << i << "] "
                 << (gpu.name.empty() ? std::string("<unknown>") : gpu.name)
                 << " ספק=0x" << std::hex << gpu.vendor_id << std::dec
                 << " ייעודי=" << format_memory_mb(gpu.dedicated_video_memory)
                 << " משותף=" << format_memory_mb(gpu.shared_system_memory)
                 << " סוג=" << (gpu.is_software ? "תוכנה" : (gpu.likely_dedicated ? "ייעודי" : "משולב"));
            vibble::log::info(line.str());
        }
    }

    if (!context.preferred_gpu_name.empty()) {
        vibble::log::info("[EngineRenderer] מועמד GPU מועדף: " + context.preferred_gpu_name +
                          (context.has_dedicated_preference ? " [ייעודי]" : " [משולב]"));
    }
#endif
}

std::vector<const char*> gpu_driver_attempt_order(const GpuSelectionContext& context) {
    std::vector<const char*> order;
#ifdef _WIN32
    if (context.has_dedicated_preference) {
        if (has_driver(context.available_gpu_drivers, "direct3d12")) {
            order.push_back("direct3d12");
        }
        if (has_driver(context.available_gpu_drivers, "vulkan")) {
            order.push_back("vulkan");
        }
    }
#endif
    order.push_back(nullptr);  // תן ל-SDL לבחור.
    return order;
}

std::string driver_hint_label(const char* driver_hint) {
    return (driver_hint && driver_hint[0]) ? std::string(driver_hint) : std::string("auto");
}

bool should_retry_for_dedicated_gpu(const GpuSelectionContext& context, const std::string& selected_gpu_name) {
    if (!context.has_dedicated_preference) {
        return false;
    }
    if (selected_gpu_name.empty()) {
        return false;
    }
    if (names_match_relaxed(selected_gpu_name, context.preferred_gpu_name)) {
        return false;
    }
    return !looks_like_dedicated_name(selected_gpu_name);
}

std::string join_failure_reasons(const std::vector<std::string>& reasons) {
    std::ostringstream oss;
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i != 0) {
            oss << " | ";
        }
        oss << reasons[i];
    }
    return oss.str();
}

SDL_Renderer* create_renderer_with_properties(SDL_PropertiesID props, const char* context) {
    SDL_Renderer* renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    if (!renderer) {
        vibble::log::error(std::string("[EngineRenderer] יצירת renderer נכשלה (") +
                           (context ? context : "לא ידוע") + "): " + SDL_GetError());
    }
    return renderer;
}

bool probe_render_target_support(SDL_Renderer* renderer) {
    SDL_Texture* probe = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           4, 4);
    if (!probe) {
        return false;
    }
    SDL_DestroyTexture(probe);
    return true;
}

bool probe_texture_scale_mode(SDL_Renderer* renderer) {
    SDL_Texture* probe = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_STATIC,
                                           2, 2);
    if (!probe) {
        return false;
    }
    const bool ok = SDL_SetTextureScaleMode(probe, SDL_SCALEMODE_LINEAR);
    SDL_DestroyTexture(probe);
    return ok;
}

RenderBackendType classify_backend(SDL_Renderer* renderer, RenderBackendType hinted) {
    if (!renderer) {
        return hinted;
    }
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (!props) {
        return hinted;
    }
    if (SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr)) {
        return RenderBackendType::GPU;
    }
    if (SDL_GetPointerProperty(props, SDL_PROP_RENDERER_SURFACE_POINTER, nullptr)) {
        return RenderBackendType::Software;
    }
    return hinted;
}
} // namespace

EngineRenderer::EngineRenderer(SDL_Renderer* renderer, RenderCaps caps, RenderQualityTier tier, SDL_Window* window)
    : renderer_(renderer), window_(window), caps_(std::move(caps)), quality_tier_(tier) {
    present_mode_name_ = caps_.vsync_enabled ? "vsync" : "immediate";

    if (caps_.backend_type == RenderBackendType::GPU && renderer_) {
        SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
        SDL_GPUDevice* gpu_device = props
            ? static_cast<SDL_GPUDevice*>(SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr))
            : nullptr;
        if (gpu_device) {
            std::string format_error;
            has_gpu_format_policy_ = GpuFormatPolicyResolver::Resolve(gpu_device,
                                                                       false,
                                                                       gpu_format_policy_,
                                                                       format_error);
            if (has_gpu_format_policy_) {
                vibble::log::info("[EngineRenderer] GPU format policy: albedo=" +
                                  std::to_string(static_cast<int>(gpu_format_policy_.albedo_format)) +
                                  " light=" + std::to_string(static_cast<int>(gpu_format_policy_.light_accumulation_format)) +
                                  " mask=" + std::to_string(static_cast<int>(gpu_format_policy_.mask_format)) +
                                  " depth=" + std::to_string(static_cast<int>(gpu_format_policy_.depth_format)) +
                                  " samples=" + std::to_string(static_cast<int>(gpu_format_policy_.sample_count)));
            } else {
                vibble::log::error("[EngineRenderer] GPU format policy probe failed: " + format_error);
            }
        }
    }
}

EngineRenderer::~EngineRenderer() {
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
}

std::unique_ptr<EngineRenderer> EngineRenderer::Create(SDL_Window* window, bool prefer_vsync) {
    if (!window) {
        vibble::log::error("[EngineRenderer] אי אפשר ליצור renderer: החלון הוא null.");
        return nullptr;
    }

    const GpuSelectionContext gpu_context = detect_gpu_selection_context();
    log_gpu_selection_context(gpu_context);

    AttemptResult gpu_attempt{};
    AttemptResult deferred_gpu_attempt{};
    bool has_deferred_gpu_attempt = false;
    std::vector<std::string> gpu_failures;

    for (const char* driver_hint : gpu_driver_attempt_order(gpu_context)) {
        AttemptResult attempt = try_create_gpu(window, prefer_vsync, driver_hint);
        const std::string hint_label = driver_hint_label(driver_hint);
        if (!attempt.renderer) {
            if (!attempt.failure_reason.empty()) {
                gpu_failures.push_back("[" + hint_label + "] " + attempt.failure_reason);
            }
            continue;
        }

        const std::string selected_gpu_name = query_renderer_gpu_name(attempt.renderer);
        if (should_retry_for_dedicated_gpu(gpu_context, selected_gpu_name)) {
            vibble::log::warn("[EngineRenderer] backend ה-GPU " + hint_label +
                              " בחר '" + selected_gpu_name +
                              "' במקום ה-GPU הייעודי המועדף '" + gpu_context.preferred_gpu_name +
                              "'. מנסה backend הבא.");
            if (!has_deferred_gpu_attempt) {
                deferred_gpu_attempt = attempt;
                has_deferred_gpu_attempt = true;
            } else if (attempt.renderer) {
                SDL_DestroyRenderer(attempt.renderer);
            }
            continue;
        }

        gpu_attempt = attempt;
        break;
    }

    if (!gpu_attempt.renderer && has_deferred_gpu_attempt) {
        gpu_attempt = deferred_gpu_attempt;
        vibble::log::warn("[EngineRenderer] מעבר ל-renderer ה-GPU הזמין הטוב ביותר למרות העדפה לייעודי.");
    }

    if (!gpu_attempt.renderer && !gpu_failures.empty()) {
        gpu_attempt.failure_reason = join_failure_reasons(gpu_failures);
    }

    if (gpu_attempt.renderer && has_deferred_gpu_attempt &&
        deferred_gpu_attempt.renderer && deferred_gpu_attempt.renderer != gpu_attempt.renderer) {
        SDL_DestroyRenderer(deferred_gpu_attempt.renderer);
    }

    if (gpu_attempt.renderer) {
        auto tier = choose_quality_tier(gpu_attempt.caps);
        log_caps(gpu_attempt.caps);
        std::unique_ptr<EngineRenderer> candidate(
            new EngineRenderer(gpu_attempt.renderer, gpu_attempt.caps, tier, window));
        if (!candidate->has_gpu_format_policy_) {
            vibble::log::error("[EngineRenderer] GPU renderer failed strict startup format policy checks. Falling back.");
            candidate.reset();
        } else {
            return candidate;
        }
    }
    if (!gpu_attempt.failure_reason.empty()) {
        vibble::log::warn("[EngineRenderer] renderer של GPU לא זמין, מעבר ל-2D מואץ. סיבה: " +
                          gpu_attempt.failure_reason);
    }

    AttemptResult accel_attempt{};
#ifdef _WIN32
    if (gpu_context.has_dedicated_preference) {
        AttemptResult d3d12_attempt = try_create_accelerated(window, prefer_vsync, "direct3d12");
        if (d3d12_attempt.renderer) {
            accel_attempt = d3d12_attempt;
        } else if (!d3d12_attempt.failure_reason.empty()) {
            accel_attempt.failure_reason = d3d12_attempt.failure_reason;
        }
    }
#endif
    if (!accel_attempt.renderer) {
        AttemptResult auto_attempt = try_create_accelerated(window, prefer_vsync, nullptr);
        if (auto_attempt.renderer) {
            accel_attempt = auto_attempt;
        } else if (!auto_attempt.failure_reason.empty()) {
            if (!accel_attempt.failure_reason.empty()) {
                accel_attempt.failure_reason += " | ";
            }
            accel_attempt.failure_reason += auto_attempt.failure_reason;
        }
    }
    if (accel_attempt.renderer) {
        auto tier = choose_quality_tier(accel_attempt.caps);
        log_caps(accel_attempt.caps);
        return std::unique_ptr<EngineRenderer>(new EngineRenderer(accel_attempt.renderer, accel_attempt.caps, tier, window));
    }
    if (!accel_attempt.failure_reason.empty()) {
        vibble::log::warn("[EngineRenderer] renderer מואץ לא זמין, מעבר לתוכנה. סיבה: " +
                          accel_attempt.failure_reason);
    }

    AttemptResult software_attempt = try_create_software(window);
    if (software_attempt.renderer) {
        auto tier = choose_quality_tier(software_attempt.caps);
        log_caps(software_attempt.caps);
        return std::unique_ptr<EngineRenderer>(new EngineRenderer(software_attempt.renderer, software_attempt.caps, tier, window));
    }

    vibble::log::error("[EngineRenderer] יצירת כל renderer נכשלה. כשל GPU: " +
                       gpu_attempt.failure_reason +
                       " | כשל מואץ: " + accel_attempt.failure_reason +
                       " | כשל תוכנה: " + software_attempt.failure_reason);
    return nullptr;
}

void EngineRenderer::begin_frame(const SDL_Color& clear_color) {
    if (!renderer_) return;
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    SDL_RenderClear(renderer_);
}

void EngineRenderer::end_frame() {
    // מקום שמור ל-flush ייעודי ל-backend אם יידרש בהמשך.
}

void EngineRenderer::present() {
    if (renderer_) {
        const std::uint64_t perf_frequency = SDL_GetPerformanceFrequency();
        const std::uint64_t present_begin = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer_);
        const std::uint64_t present_end = SDL_GetPerformanceCounter();

        const double present_block_ms =
            (perf_frequency > 0 && present_end >= present_begin)
                ? (static_cast<double>(present_end - present_begin) * 1000.0 /
                   static_cast<double>(perf_frequency))
                : 0.0;

        bool interval_known = false;
        double present_interval_ms = 0.0;
        if (perf_frequency > 0 && last_present_counter_ != 0 && present_begin >= last_present_counter_) {
            present_interval_ms =
                (static_cast<double>(present_begin - last_present_counter_) * 1000.0) /
                static_cast<double>(perf_frequency);
            interval_known = true;
        }
        last_present_counter_ = present_end;
        render_diagnostics::set_present_pacing(present_block_ms, present_interval_ms, interval_known);
    }
}

void EngineRenderer::set_scale(float scale_x, float scale_y) {
    if (renderer_) {
        SDL_SetRenderScale(renderer_, scale_x, scale_y);
    }
}

void EngineRenderer::set_viewport(const SDL_Rect& rect) {
    if (renderer_) {
        SDL_SetRenderViewport(renderer_, &rect);
    }
}

void EngineRenderer::clear_viewport() {
    if (renderer_) {
        SDL_SetRenderViewport(renderer_, nullptr);
    }
}

SDL_Texture* EngineRenderer::create_texture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h) const {
    if (!renderer_) return nullptr;
    return SDL_CreateTexture(renderer_, format, access, w, h);
}

SDL_Texture* EngineRenderer::create_texture_from_surface(SDL_Surface* surface) const {
    if (!renderer_ || !surface) return nullptr;
    return SDL_CreateTextureFromSurface(renderer_, surface);
}

EngineRenderer::AttemptResult EngineRenderer::try_create_gpu(SDL_Window* window,
                                                             bool prefer_vsync,
                                                             const char* gpu_driver_hint) {
    AttemptResult attempt{};

    // כפה העדפת ביצועים גבוהה לפני כל ניסיון ליצור renderer של GPU.
    SDL_SetHint(SDL_HINT_RENDER_GPU_LOW_POWER, "0");
    if (gpu_driver_hint && gpu_driver_hint[0]) {
        SDL_SetHint(SDL_HINT_GPU_DRIVER, gpu_driver_hint);
    } else {
        SDL_ResetHint(SDL_HINT_GPU_DRIVER);
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props ||
        !SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window) ||
        !SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER) ||
        !SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, prefer_vsync ? 1 : 0)) {
        attempt.failure_reason = "Failed to configure GPU renderer properties: " + safe_string(SDL_GetError());
        if (props) SDL_DestroyProperties(props);
        return attempt;
    }

    const std::string context_label = std::string("gpu/") + driver_hint_label(gpu_driver_hint);
    attempt.renderer = create_renderer_with_properties(props, context_label.c_str());
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    SDL_SetDefaultTextureScaleMode(attempt.renderer, SDL_SCALEMODE_LINEAR);
    attempt.caps = build_caps(attempt.renderer, RenderBackendType::GPU);
    return attempt;
}

EngineRenderer::AttemptResult EngineRenderer::try_create_accelerated(SDL_Window* window,
                                                                     bool prefer_vsync,
                                                                     const char* renderer_name_hint) {
    AttemptResult attempt{};

    // העדף בחירת vsync מפורשת דרך מאפיינים; אפשר ל-SDL לבחור את הדרייבר.
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props ||
        !SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window) ||
        !SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, prefer_vsync ? 1 : 0)) {
        attempt.failure_reason = "Failed to configure accelerated renderer properties: " + safe_string(SDL_GetError());
        if (props) SDL_DestroyProperties(props);
        return attempt;
    }

    if (renderer_name_hint && renderer_name_hint[0] &&
        !SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, renderer_name_hint)) {
        attempt.failure_reason = "Failed to set accelerated renderer hint: " + safe_string(SDL_GetError());
        SDL_DestroyProperties(props);
        return attempt;
    }

    const std::string context_label = std::string("accelerated/") + driver_hint_label(renderer_name_hint);
    attempt.renderer = create_renderer_with_properties(props, context_label.c_str());
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    SDL_SetDefaultTextureScaleMode(attempt.renderer, SDL_SCALEMODE_LINEAR);
    attempt.caps = build_caps(attempt.renderer, RenderBackendType::Render2D);
    return attempt;
}

EngineRenderer::AttemptResult EngineRenderer::try_create_software(SDL_Window* window) {
    AttemptResult attempt{};

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);
    if (!window_surface) {
        attempt.failure_reason = "[software] SDL_GetWindowSurface failed: " + safe_string(SDL_GetError());
        return attempt;
    }

    attempt.renderer = SDL_CreateSoftwareRenderer(window_surface);
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    attempt.caps = build_caps(attempt.renderer, RenderBackendType::Software);
    return attempt;
}

RenderCaps EngineRenderer::build_caps(SDL_Renderer* renderer, RenderBackendType backend_type) {
    RenderCaps caps{};
    caps.backend_type = classify_backend(renderer, backend_type);
    caps.is_software = caps.backend_type == RenderBackendType::Software;

    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (props) {
        caps.max_texture_size = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
        caps.vsync_enabled = SDL_GetNumberProperty(props, SDL_PROP_RENDERER_VSYNC_NUMBER, 0) != 0;
        caps.renderer_name = safe_string(SDL_GetRendererName(renderer));
    } else {
        caps.renderer_name = safe_string(SDL_GetRendererName(renderer));
    }

    caps.supports_render_targets = probe_render_target_support(renderer);
    caps.supports_texture_scale_modes = probe_texture_scale_mode(renderer);
    caps.supports_blend_modes = true; // renderers של SDL תומכים במצבי ערבוב; הדגל נשמר לשלמות.

    return caps;
}

RenderQualityTier EngineRenderer::choose_quality_tier(const RenderCaps& caps) {
    if (caps.backend_type == RenderBackendType::GPU) {
        return RenderQualityTier::GPU;
    }
    if (caps.backend_type == RenderBackendType::Software || caps.is_software) {
        return RenderQualityTier::Software;
    }
    return RenderQualityTier::Accelerated;
}

void EngineRenderer::log_caps(const RenderCaps& caps) {
    std::ostringstream oss;
    oss << "[EngineRenderer] Backend=";
    switch (caps.backend_type) {
    case RenderBackendType::GPU:      oss << "GPU"; break;
    case RenderBackendType::Render2D: oss << "Render2D"; break;
    case RenderBackendType::Software: oss << "Software"; break;
    }
    oss << " שם=" << caps.renderer_name
        << " מקס_טקסטורה=" << caps.max_texture_size
        << " RT=" << (caps.supports_render_targets ? "כן" : "לא")
        << " מצב_סקייל=" << (caps.supports_texture_scale_modes ? "כן" : "לא")
        << " VSync=" << (caps.vsync_enabled ? "פועל" : "כבוי")
        << " תוכנה=" << (caps.is_software ? "כן" : "לא");
    vibble::log::info(oss.str());
}
