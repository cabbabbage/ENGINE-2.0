#include "utils/frame_stats_recorder.hpp"

#include "utils/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace runtime_stats {
namespace {

struct FrameSnapshot {
    std::uint64_t frame_id = 0;
    std::unordered_map<std::string, std::string> values;
};

struct RecorderState {
    std::mutex mutex;
    std::filesystem::path output_path;
    std::ofstream output_stream;
    std::vector<std::string> metric_order;
    std::unordered_set<std::string> known_metrics;
    FrameSnapshot current_frame;
    bool started = false;
    bool frame_open = false;
    bool header_written = false;
    bool write_failure_reported = false;
    Uint64 last_frame_begin_counter = 0;
    Uint64 last_frame_end_counter = 0;
};

struct WatchdogState {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    bool running = false;
    bool stop_requested = false;
    bool heartbeat_active = false;
    bool idle_wait = false;
    bool stall_active = false;
    Uint64 last_counter = 0;
    Uint64 stall_begin_counter = 0;
    Uint64 last_report_counter = 0;
    std::uint64_t frame_id = 0;
    std::string stage = "inactive";
};

RecorderState& state() {
    static RecorderState s;
    return s;
}

WatchdogState& watchdog_state() {
    static WatchdogState s;
    return s;
}

constexpr double kFreezeWatchdogThresholdMs = 500.0;
constexpr double kFreezeWatchdogIdleThresholdMs = 2000.0;
constexpr double kFreezeWatchdogReportIntervalMs = 1000.0;
constexpr auto kFreezeWatchdogPollInterval = std::chrono::milliseconds(100);

std::filesystem::path default_output_path() {
#if defined(PROJECT_ROOT)
    return std::filesystem::path(PROJECT_ROOT) / "runtime_frame_stats.csv";
#else
    return std::filesystem::path("ENGINE") / "runtime_frame_stats.csv";
#endif
}

std::vector<std::string> default_metric_order() {
    return {
        "main.telemetry_schema",
        "main.frame_gap_ms",
        "main.frame_begin_interval_ms",
        "main.raw_frame_total_ms",
        "main.last_completed_stage",
        "main.event_count",
        "main.event_poll_ms",
        "main.keyboard_sync_ms",
        "main.resize_sync_ms",
        "main.sync_output_ms",
        "main.assets_update_ms",
        "assets.phase.active_set_refresh.assets_touched",
        "assets.phase.world_update.assets_touched",
        "assets.phase.world_update.components_mutated",
        "assets.phase.runtime_effects.assets_touched",
        "assets.phase.runtime_effects.components_mutated",
        "assets.phase.render_handoff.assets_touched",
        "assets.phase.refreshes_triggered",
        "main.input_update_ms",
        "main.idle_pacing_requested_ms",
        "main.idle_pacing_delay_ms",
        "main.menu_active",
        "main.quit_requested",
        "main.frame_total_ms",
        "input.keyboard_reconciled",
        "input.keyboard_reconciled_changed_count",
        "input.keyboard_focus_active",
        "input.focus_loss_cleared",
        "input.live.w",
        "input.live.a",
        "input.live.s",
        "input.live.d",
        "input.live.space",
        "input.stored.w",
        "input.stored.a",
        "input.stored.s",
        "input.stored.d",
        "input.stored.space",
        "codex_playtest.input_driver",
        "codex_playtest.phase",
        "codex_playtest.segment_index",
        "codex_playtest.segment_kind",
        "codex_playtest.segment_length_frames",
        "codex_playtest.segment_frame",
        "codex_playtest.mouse_x",
        "codex_playtest.mouse_y",
        "movement.player_input_x",
        "movement.player_input_y",
        "movement.player_world_intent_x",
        "movement.player_world_intent_y",
        "movement.player_has_intent",
        "movement.player_input_delta_raw_x",
        "movement.player_input_delta_raw_y",
        "movement.player_input_delta_clamped_x",
        "movement.player_input_delta_clamped_y",
        "movement.player_input_delta_clamped",
        "movement.player_dash_active",
        "movement.player_delta_world_x",
        "movement.player_delta_world_z",
        "movement.player_final_delta_world_x",
        "movement.player_final_delta_world_z",
        "movement.player_path_blocked_checks",
        "movement.player_path_blocked_ms",
        "movement.player_direct_blocked",
        "movement.player_fallback_used",
        "movement.player_fallback_probes",
        "movement.player_fallback_ms",
        "movement.player_fallback_probe_cap_hit",
        "movement.player_fallback_shortened_used",
        "movement.player_slide_used",
        "movement.player_unstick_used",
        "assets.frame_id",
        "assets.frame_dt_raw_seconds",
        "assets.frame_dt_clamped_seconds",
        "assets.frame_dt_seconds",
        "assets.idle_frame",
        "assets.dev_mode",
        "assets.runtime_updates_enabled",
        "assets.active_count",
        "assets.filtered_active_count",
        "assets.total_count",
        "assets.visibility_traversal_count",
        "assets.active_candidate_count",
        "assets.active_publish_count",
        "assets.active_fail_open_applied",
        "assets.active_fail_open_frames",
        "assets.active_fail_open_reason",
        "assets.world_ms",
        "assets.visibility_ms",
        "assets.visible_scaling_refreshed",
        "assets.runtime_effects_ms",
        "assets.filtered_refresh_ms",
        "assets.render_ms",
        "assets.dev_sync_ms",
        "dev.update_total_ms",
        "dev.room_editor_update_ms",
        "dev.room_editor_ui_ms",
        "dev.other_settings_ms",
        "dev.map_mode_ui_ms",
        "dev.camera_panel_ms",
        "dev.layout_ms",
        "dev.save_ms",
        "dev.active_assets_sync_ms",
        "dev.current_room_sync_ms",
        "dev.active_assets_generation",
        "dev.filtered_assets_generation",
        "dev.set_active_assets_called",
        "dev.current_room_changed",
        "dev.layout_dirty",
        "assets.slow_frame",
        "assets.slow_frame_threshold_ms",
        "assets.startup_safety_active",
        "assets.world_total_ms",
        "assets.world_player_ms",
        "assets.world_non_player_ms",
        "assets.world_movement_flush_ms",
        "assets.world_camera_ms",
        "assets.world_camera_state_changed",
        "assets.world_camera_rebuild_requested",
        "assets.world_max_dimensions_ms",
        "assets.world_dev_sync_ms",
        "assets.world_pending_assets_ms",
        "assets.world_empty_points_ms",
        "assets.maintenance_budget_ms",
        "assets.maintenance_pending_static",
        "assets.maintenance_pending_empty_points",
        "assets.world_full_updates",
        "assets.world_skipped_static_updates",
        "assets.world_trap_candidates",
        "assets.world_active_count",
        "assets.collision_context_rebuilt",
        "assets.collision_context_rebuild_ms",
        "assets.collision_context_entries",
        "assets.slow_world_update",
        "assets.slow_world_update_threshold_ms",
        "assets.runtime_effects_skipped_startup",
        "assets.runtime_convergence_trace_enabled",
        "assets.runtime_convergence_iterations",
        "assets.runtime_convergence_converged",
        "assets.runtime_convergence_waves",
        "assets.runtime_convergence_children_considered",
        "assets.runtime_convergence_children_updated",
        "assets.runtime_convergence_traversal_refreshes",
        "assets.runtime_convergence_pass_ms",
        "assets.runtime_convergence_refresh_ms",
        "assets.runtime_convergence_stage_ms",
        "assets.slow_runtime_effects",
        "assets.slow_runtime_effects_threshold_ms",
        "assets.runtime_convergence_cap_reached",
        "assets.runtime_convergence_iteration_cap",
        "assets.trap_escape_skipped_startup",
        "assets.trap_escape_skipped_no_movement_assets",
        "assets.trap_escape_ms",
        "assets.trap_escape_processed",
        "assets.trap_escape_skipped",
        "assets.trap_escape_queries",
        "assets.trap_escape_unstuck",
        "assets.trap_escape_candidates",
        "assets.slow_trap_escape",
        "assets.slow_trap_escape_threshold_ms",
        "assets.render_should_render",
        "assets.render_suppressed",
        "assets.render_has_opengl_renderer",
        "assets.render_ui_overlay_texture",
        "assets.render_ui_overlay_active",
        "assets.render_ui_overlay_prepare_ms",
        "assets.render_active_count",
        "assets.render_submit_succeeded",
        "assets.frame_rebuild_requests",
        "assets.frame_rebuild_executions",
        "assets.frame_rebuild_coalesced",
        "assets.slow_runtime_asset_pass",
        "assets.slow_runtime_asset_name",
        "assets.slow_runtime_asset_ms",
        "assets.slow_runtime_asset_geometry_refresh",
        "assets.slow_runtime_asset_camera_only",
        "assets.slow_runtime_asset_frame_index",
        "dynamic_spawn.active",
        "dynamic_spawn.suspended",
        "dynamic_spawn.planned_cells",
        "dynamic_spawn.spawned",
        "dynamic_spawn.reused",
        "dynamic_spawn.deleted",
        "dynamic_spawn.suspended_this_sync",
        "dynamic_spawn.sync_ms",
        "dynamic_spawn.cells_processed_this_frame",
        "dynamic_spawn.deferred_cells_remaining",
        "dynamic_spawn.movement_throttling_applied",
        "audio.update_ms",
        "camera.projection_w",
        "camera.projection_h",
        "camera.center_x",
        "camera.center_y",
        "camera.target_center_x",
        "camera.target_center_y",
        "camera.velocity_x",
        "camera.velocity_y",
        "camera.state_version",
        "camera.visible_left",
        "camera.visible_top",
        "camera.visible_right",
        "camera.visible_bottom",
        "camera.frustum_min_z",
        "camera.frustum_max_z",
        "camera.depth_culled",
        "camera.nodes_visited",
        "camera.branches_skipped",
        "camera.transition_state",
        "camera.transition_target_x",
        "camera.transition_target_y",
        "camera.transition_velocity_x",
        "camera.transition_velocity_y",
        "camera.transition_blend_factor",
        "camera.transition_settle_time_remaining",
        "render.loop_label",
        "render.diagnostics_frame",
        "render.window_w",
        "render.window_h",
        "render.window_px_w",
        "render.window_px_h",
        "render.output_w",
        "render.output_h",
        "render.target_w",
        "render.target_h",
        "render.target_is_backbuffer",
        "render.viewport_x",
        "render.viewport_y",
        "render.viewport_w",
        "render.viewport_h",
        "render.clip_x",
        "render.clip_y",
        "render.clip_w",
        "render.clip_h",
        "render.postprocess_target",
        "render.frame_cpu_ms",
        "render.render_thread_cpu_ms",
        "render.draw_submission_ms",
        "render.draw_submission_packet_build_sort_ms",
        "render.draw_submission_resource_create_ms",
        "render.draw_submission_pipeline_bind_ms",
        "render.draw_submission_submit_handoff_ms",
        "render.draw_submission_packet_build_count",
        "render.draw_submission_resource_create_count",
        "render.draw_submission_pipeline_bind_count",
        "render.draw_submission_submit_handoff_count",
        "render.present_block_ms",
        "render.present_interval_ms",
        "render.ui_overlay_cache_hit",
        "render.ui_overlay_content_dirty",
        "render.ui_overlay_redraw_reason",
        "render.present_interval_known",
        "render.present_calls",
        "render.pass_count",
        "render.copy_pass_count",
        "render.compute_pass_count",
        "render.draw_calls",
        "render.target_switches",
        "render.texture_create_count",
        "render.texture_destroy_count",
        "render.gpu_buffer_create_count",
        "render.gpu_buffer_destroy_count",
        "render.cpu_light_gather_ms",
        "render.cpu_light_mask_ms",
        "render.gpu_light_tiles",
        "render.gpu_light_naive",
        "render.gpu_light_tiled",
        "render.pipeline_cache_hits",
        "render.pipeline_cache_misses",
        "render.pipeline_cache_hit_rate",
        "render.sdl_target_calls",
        "render.sdl_draw_calls",
        "render.gpu_failed_frames",
        "render.renderer_path",
        "render.backend",
        "render.present_mode",
        "render.texture_memory_known",
        "render.texture_memory_mb",
        "render.floor_packet_count",
        "render.xy_sprite_packet_count",
        "render.held_scene_frame",
        "render.held_scene_reason",
        "render.active_depth_layer_count",
        "render.blur_pass_count",
        "render.skipped_texture_count",
        "render.failed_texture_names",
        "render.packets_per_depth_layer",
        "render.blur_strength_per_layer",
        "render.composite_layers_submitted",
        "render.stage_timings",
        "render.ui_overlay_active",
        "render.ui_overlay_redrawn",
        "render.ui_overlay_prepare_ms",
        "render.submit_succeeded",
        "render.projection_calls_total",
        "render.projection_calls_saved_early",
        "render.assets_stageA_reject",
        "render.assets_stageC_entered",
        "render.projection_recompute_budget",
        "render.projection_points_deferred",
        "render.projection_points_updated",
        "render.creation_budget_limit",
        "render.creation_budget_ms_limit",
        "render.creation_attempted_this_frame",
        "render.creation_executed_this_frame",
        "render.creation_deferred_count",
        "render.creation_queue_depth_start",
        "render.creation_queue_depth_end",
        "render.creation_queue_age_max",
        "render.creation_retried_count",
        "render.creation_permanent_failures",
        "render.warn_target_half_output",
        "render.warn_viewport_y_nonzero",
        "render.warn_clip_y_nonzero",
        "render.warn_projection_output_mismatch",
        "scale_trace.asset_name",
        "scale_trace.asset_frame_id",
        "scale_trace.asset_source",
        "scale_trace.asset_perspective",
        "scale_trace.asset_base",
        "scale_trace.asset_scale",
        "scale_trace.asset_delta",
        "scale_trace.anchor_asset_name",
        "scale_trace.anchor_camera_state",
        "scale_trace.anchor_source",
        "scale_trace.anchor_resolver",
        "scale_trace.anchor_render",
        "scale_trace.anchor_delta",
        "scale_trace.mode_toggle_mode",
        "scale_trace.mode_toggle_camera_state",
        "scale_trace.mode_toggle_active_generation",
        "extra_metrics",
    };
}

std::string escape_csv(const std::string& value) {
    const bool needs_quotes =
        value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
        if (ch == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

std::string number_to_string(double value) {
    if (!std::isfinite(value)) {
        return {};
    }
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(6) << value;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out.empty() ? std::string{"0"} : out;
}

double counter_delta_ms(Uint64 begin_counter, Uint64 end_counter) {
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0 || begin_counter == 0 || end_counter <= begin_counter) {
        return 0.0;
    }
    return (static_cast<double>(end_counter - begin_counter) * 1000.0) /
           static_cast<double>(frequency);
}

void log_watchdog_stall(const char* kind,
                        double stall_ms,
                        const std::string& stage,
                        std::uint64_t frame_id,
                        bool recovered) {
    std::ostringstream out;
    out << "[FreezeWatchdog] " << kind
        << " stall_ms=" << number_to_string(stall_ms)
        << " stage=" << stage
        << " frame=" << frame_id
        << " recovered=" << (recovered ? 1 : 0);
    vibble::log::warn(out.str());
}

void watchdog_loop() {
    WatchdogState& w = watchdog_state();
    std::unique_lock<std::mutex> lock(w.mutex);
    while (!w.stop_requested) {
        w.cv.wait_for(lock, kFreezeWatchdogPollInterval);
        if (w.stop_requested || !w.heartbeat_active || w.last_counter == 0) {
            continue;
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        const double idle_ms = counter_delta_ms(w.last_counter, now);
        const double threshold_ms = w.idle_wait
            ? kFreezeWatchdogIdleThresholdMs
            : kFreezeWatchdogThresholdMs;

        if (idle_ms >= threshold_ms) {
            const std::string stage = w.stage;
            const std::uint64_t frame_id = w.frame_id;
            if (!w.stall_active) {
                w.stall_active = true;
                w.stall_begin_counter = w.last_counter;
                w.last_report_counter = now;
                lock.unlock();
                log_watchdog_stall("start", idle_ms, stage, frame_id, false);
                lock.lock();
            } else if (counter_delta_ms(w.last_report_counter, now) >= kFreezeWatchdogReportIntervalMs) {
                w.last_report_counter = now;
                lock.unlock();
                log_watchdog_stall("update", idle_ms, stage, frame_id, false);
                lock.lock();
            }
            continue;
        }

        if (w.stall_active) {
            const double stall_ms = counter_delta_ms(w.stall_begin_counter, now);
            const std::string stage = w.stage;
            const std::uint64_t frame_id = w.frame_id;
            w.stall_active = false;
            w.stall_begin_counter = 0;
            w.last_report_counter = 0;
            lock.unlock();
            log_watchdog_stall("recovered", stall_ms, stage, frame_id, true);
            lock.lock();
        }
    }
}

void start_watchdog() {
    WatchdogState& w = watchdog_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    if (w.running) {
        return;
    }
    w.stop_requested = false;
    w.heartbeat_active = false;
    w.idle_wait = false;
    w.stall_active = false;
    w.last_counter = 0;
    w.stall_begin_counter = 0;
    w.last_report_counter = 0;
    w.frame_id = 0;
    w.stage = "inactive";
    w.thread = std::thread(watchdog_loop);
    w.running = true;
}

void stop_watchdog() {
    WatchdogState& w = watchdog_state();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        if (!w.running) {
            return;
        }
        w.stop_requested = true;
        w.heartbeat_active = false;
        thread = std::move(w.thread);
    }
    w.cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        w.running = false;
        w.stop_requested = false;
        w.stall_active = false;
        w.stage = "inactive";
    }
}

void update_watchdog_heartbeat(std::uint64_t frame_id, const char* stage, bool idle_wait) {
    WatchdogState& w = watchdog_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    if (!w.running || w.stop_requested) {
        return;
    }
    w.heartbeat_active = true;
    w.idle_wait = idle_wait;
    w.last_counter = SDL_GetPerformanceCounter();
    w.frame_id = frame_id;
    w.stage = (stage && stage[0] != '\0') ? stage : "unknown";
}

std::string extra_metrics_for_frame(const FrameSnapshot& frame,
                                    const std::unordered_set<std::string>& known_metrics) {
    std::vector<std::string> extras;
    extras.reserve(frame.values.size());
    for (const auto& [metric, value] : frame.values) {
        if (known_metrics.find(metric) == known_metrics.end()) {
            extras.push_back(metric + "=" + value);
        }
    }
    std::sort(extras.begin(), extras.end());
    std::ostringstream out;
    for (std::size_t i = 0; i < extras.size(); ++i) {
        if (i > 0) {
            out << ';';
        }
        out << extras[i];
    }
    return out.str();
}

void write_frame_row(std::ostream& out,
                     const FrameSnapshot& frame,
                     const std::vector<std::string>& metric_order,
                     const std::unordered_set<std::string>& known_metrics) {
    out << frame.frame_id;
    for (const std::string& metric : metric_order) {
        out << ',';
        if (metric == "extra_metrics") {
            out << escape_csv(extra_metrics_for_frame(frame, known_metrics));
            continue;
        }
        const auto it = frame.values.find(metric);
        if (it != frame.values.end()) {
            out << escape_csv(it->second);
        }
    }
    out << '\n';
}

bool prepare_output_parent_locked(RecorderState& s) {
    if (!s.started || s.output_path.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = s.output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        ec.clear();
    }
    return true;
}

bool report_write_failure_once_locked(RecorderState& s, const char* action) {
    if (!s.write_failure_reported) {
        std::cerr << "[FrameStatsRecorder] Failed to " << action
                  << " runtime stats CSV: " << s.output_path.string() << "\n";
        s.write_failure_reported = true;
    }
    return false;
}

bool write_header_locked(RecorderState& s) {
    if (!prepare_output_parent_locked(s)) {
        return false;
    }
    if (s.output_stream.is_open()) {
        s.output_stream.close();
    }
    s.output_stream.clear();
    s.output_stream.open(s.output_path, std::ios::out | std::ios::trunc);
    if (!s.output_stream.good()) {
        return report_write_failure_once_locked(s, "create");
    }
    s.output_stream << "frame_id";
    for (const std::string& metric : s.metric_order) {
        s.output_stream << ',' << escape_csv(metric);
    }
    s.output_stream << '\n';
    s.output_stream.flush();
    if (!s.output_stream.good()) {
        return report_write_failure_once_locked(s, "write header to");
    }
    s.header_written = true;
    s.write_failure_reported = false;
    return true;
}

bool append_frame_locked(RecorderState& s, const FrameSnapshot& frame) {
    if (!s.header_written && !write_header_locked(s)) {
        return false;
    }
    if (!s.output_stream.is_open()) {
        s.output_stream.clear();
        s.output_stream.open(s.output_path, std::ios::out | std::ios::app);
    }
    if (!s.output_stream.good()) {
        return report_write_failure_once_locked(s, "append to");
    }
    write_frame_row(s.output_stream, frame, s.metric_order, s.known_metrics);
    s.output_stream.flush();
    if (!s.output_stream.good()) {
        return report_write_failure_once_locked(s, "write row to");
    }
    s.write_failure_reported = false;
    return true;
}

void set_locked(RecorderState& s, const std::string& metric, std::string value) {
    if (!s.started || !s.frame_open || metric.empty()) {
        return;
    }
    s.current_frame.values[metric] = std::move(value);
}

} // namespace

FrameStatsRecorder::ScopedTimer::ScopedTimer(std::string metric)
    : metric_(std::move(metric)) {
    if (!FrameStatsRecorder::instance().enabled() || metric_.empty()) {
        return;
    }
    begin_counter_ = SDL_GetPerformanceCounter();
    active_ = true;
}

FrameStatsRecorder::ScopedTimer::~ScopedTimer() {
    if (!active_) {
        return;
    }
    const double ms = FrameStatsRecorder::elapsed_ms(begin_counter_, SDL_GetPerformanceCounter());
    FrameStatsRecorder::instance().add(metric_, ms);
}

FrameStatsRecorder& FrameStatsRecorder::instance() {
    static FrameStatsRecorder recorder;
    return recorder;
}

bool FrameStatsRecorder::enabled() const {
    return kRuntimeFrameStatsDebugEnabled;
}

void FrameStatsRecorder::begin_run() {
    begin_run(default_output_path());
}

void FrameStatsRecorder::begin_run(const std::filesystem::path& output_path) {
    if (!enabled()) {
        return;
    }

    start_watchdog();

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.output_path = output_path;
    s.metric_order = default_metric_order();
    s.known_metrics.clear();
    for (const std::string& metric : s.metric_order) {
        if (metric != "extra_metrics") {
            s.known_metrics.insert(metric);
        }
    }
    s.current_frame = FrameSnapshot{};
    s.started = true;
    s.frame_open = false;
    s.header_written = false;
    s.write_failure_reported = false;
    s.last_frame_begin_counter = 0;
    s.last_frame_end_counter = 0;

    write_header_locked(s);
}

void FrameStatsRecorder::shutdown() {
    if (!enabled()) {
        return;
    }

    mark_stage("shutdown");
    stop_watchdog();

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.frame_open) {
        append_frame_locked(s, s.current_frame);
        s.current_frame = FrameSnapshot{};
        s.frame_open = false;
    }
    if (s.output_stream.is_open()) {
        s.output_stream.flush();
        s.output_stream.close();
    }
    s.started = false;
}

void FrameStatsRecorder::begin_frame(std::uint64_t frame_id) {
    if (!enabled()) {
        return;
    }

    start_watchdog();
    const Uint64 frame_begin_counter = SDL_GetPerformanceCounter();

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) {
        s.output_path = default_output_path();
        s.started = true;
        s.frame_open = false;
        s.header_written = false;
        s.write_failure_reported = false;
        s.last_frame_begin_counter = 0;
        s.last_frame_end_counter = 0;
        s.metric_order = default_metric_order();
        s.known_metrics.clear();
        for (const std::string& metric : s.metric_order) {
            if (metric != "extra_metrics") {
                s.known_metrics.insert(metric);
            }
        }
        write_header_locked(s);
    }
    if (s.frame_open) {
        append_frame_locked(s, s.current_frame);
    }
    s.current_frame = FrameSnapshot{};
    s.current_frame.frame_id = frame_id;
    s.frame_open = true;
    set_locked(s,
               "main.frame_begin_interval_ms",
               number_to_string(counter_delta_ms(s.last_frame_begin_counter, frame_begin_counter)));
    set_locked(s,
               "main.frame_gap_ms",
               number_to_string(counter_delta_ms(s.last_frame_end_counter, frame_begin_counter)));
    set_locked(s, "main.last_completed_stage", "frame_begin");
    s.last_frame_begin_counter = frame_begin_counter;
    update_watchdog_heartbeat(frame_id, "frame_begin", false);
}

void FrameStatsRecorder::end_frame() {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started || !s.frame_open) {
        return;
    }
    const Uint64 frame_end_counter = SDL_GetPerformanceCounter();
    set_locked(s,
               "main.raw_frame_total_ms",
               number_to_string(counter_delta_ms(s.last_frame_begin_counter, frame_end_counter)));
    set_locked(s, "main.last_completed_stage", "frame_end");
    s.last_frame_end_counter = frame_end_counter;
    update_watchdog_heartbeat(s.current_frame.frame_id, "frame_end", false);
    append_frame_locked(s, s.current_frame);
    s.current_frame = FrameSnapshot{};
    s.frame_open = false;
}

void FrameStatsRecorder::flush() {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.started && !s.header_written) {
        write_header_locked(s);
    }
}

void FrameStatsRecorder::mark_stage(const char* stage, bool idle_wait) {
    if (!enabled()) {
        return;
    }

    std::uint64_t frame_id = 0;
    {
        RecorderState& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.started && s.frame_open) {
            frame_id = s.current_frame.frame_id;
            set_locked(s,
                       "main.last_completed_stage",
                       (stage && stage[0] != '\0') ? std::string(stage) : std::string("unknown"));
        }
    }
    update_watchdog_heartbeat(frame_id, stage, idle_wait);
}

void FrameStatsRecorder::set(const std::string& metric, const std::string& value) {
    if (!enabled()) {
        return;
    }
    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    set_locked(s, metric, value);
}

void FrameStatsRecorder::set(const std::string& metric, const char* value) {
    set(metric, value ? std::string(value) : std::string{});
}

void FrameStatsRecorder::set(const std::string& metric, bool value) {
    set(metric, value ? std::string{"1"} : std::string{"0"});
}

void FrameStatsRecorder::set(const std::string& metric, int value) {
    set(metric, std::to_string(value));
}

void FrameStatsRecorder::set(const std::string& metric, std::uint32_t value) {
    set(metric, std::to_string(value));
}

void FrameStatsRecorder::set(const std::string& metric, std::uint64_t value) {
    set(metric, std::to_string(value));
}

void FrameStatsRecorder::set(const std::string& metric, double value) {
    set(metric, number_to_string(value));
}

void FrameStatsRecorder::add(const std::string& metric, double value) {
    if (!enabled() || metric.empty() || !std::isfinite(value)) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started || !s.frame_open) {
        return;
    }
    double current = 0.0;
    const auto it = s.current_frame.values.find(metric);
    if (it != s.current_frame.values.end()) {
        try {
            current = std::stod(it->second);
        } catch (...) {
            current = 0.0;
        }
    }
    s.current_frame.values[metric] = number_to_string(current + value);
}

double FrameStatsRecorder::elapsed_ms(Uint64 begin_counter, Uint64 end_counter) {
    return counter_delta_ms(begin_counter, end_counter);
}

} // namespace runtime_stats
