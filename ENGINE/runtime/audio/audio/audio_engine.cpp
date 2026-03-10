#include "audio/audio_engine.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr float kMusicVolume = 0.6f;

fs::path resolve_with_base(const fs::path& candidate, const fs::path& base_root) {
    if (candidate.empty()) {
        return {};
    }
    if (candidate.is_absolute()) {
        return candidate;
    }
    if (!base_root.empty()) {
        return base_root / candidate;
    }
    return candidate;
}

std::vector<fs::path> collect_music_files(const nlohmann::json& audio_manifest,
                                          const std::string& content_root_hint) {
    std::vector<fs::path> result;
    if (!audio_manifest.is_object()) {
        return result;
    }

    const auto music_it = audio_manifest.find("music");
    if (music_it == audio_manifest.end() || !music_it->is_object()) {
        return result;
    }

    const nlohmann::json& music = *music_it;
    fs::path fallback_root = content_root_hint.empty() ? fs::path{} : fs::path(content_root_hint);
    fs::path base_root = fallback_root;
    if (auto root_it = music.find("content_root"); root_it != music.end() && root_it->is_string()) {
        fs::path declared = root_it->get<std::string>();
        if (!declared.is_absolute()) {
            declared = resolve_with_base(declared, fallback_root);
        }
        base_root = declared;
    }

    auto tracks_it = music.find("tracks");
    if (tracks_it == music.end() || !tracks_it->is_array()) {
        return result;
    }

    for (const auto& entry : *tracks_it) {
        fs::path local_base = base_root;
        fs::path track_path;
        if (entry.is_string()) {
            track_path = fs::path(entry.get<std::string>());
        } else if (entry.is_object()) {
            if (auto local_root_it = entry.find("content_root");
                local_root_it != entry.end() && local_root_it->is_string()) {
                fs::path declared = local_root_it->get<std::string>();
                if (!declared.is_absolute()) {
                    declared = resolve_with_base(declared, base_root.empty() ? fallback_root : base_root);
                }
                local_base = declared;
            }

            std::string path_value;
            if (auto path_it = entry.find("path"); path_it != entry.end() && path_it->is_string()) {
                path_value = path_it->get<std::string>();
            } else if (auto file_it = entry.find("file"); file_it != entry.end() && file_it->is_string()) {
                path_value = file_it->get<std::string>();
            }

            if (!path_value.empty()) {
                track_path = fs::path(path_value);
            }
        }

        if (track_path.empty()) {
            continue;
        }

        fs::path resolved = resolve_with_base(track_path, local_base);
        if (resolved.empty()) {
            continue;
        }

        try {
            resolved = fs::absolute(resolved);
        } catch (...) {
        }

        result.push_back(resolved);
    }

    return result;
}

bool load_wav_samples(const fs::path& path,
                      const SDL_AudioSpec& device_spec,
                      std::vector<float>& out_samples,
                      size_t& out_frames) {
    SDL_AudioSpec src_spec{};
    Uint8* src_data = nullptr;
    Uint32 src_len = 0;
    if (!SDL_LoadWAV(path.u8string().c_str(), &src_spec, &src_data, &src_len)) {
        std::cerr << "[AudioEngine] Failed to load audio '" << path.u8string() << "': " << SDL_GetError() << "\n";
        return false;
    }

    Uint8* dst_data = nullptr;
    int dst_len = 0;
    if (!SDL_ConvertAudioSamples(&src_spec, src_data, static_cast<int>(src_len), &device_spec, &dst_data, &dst_len)) {
        std::cerr << "[AudioEngine] Failed to convert audio '" << path.u8string() << "': " << SDL_GetError() << "\n";
        SDL_free(src_data);
        return false;
    }

    SDL_free(src_data);
    out_samples.resize(static_cast<size_t>(dst_len) / sizeof(float));
    std::memcpy(out_samples.data(), dst_data, static_cast<size_t>(dst_len));
    SDL_free(dst_data);
    const int channels = device_spec.channels;
    out_frames = channels > 0 ? out_samples.size() / static_cast<size_t>(channels) : 0;
    return !out_samples.empty();
}

}

AudioEngine& AudioEngine::instance() {
    static AudioEngine engine;
    return engine;
}

bool AudioEngine::ensure_device() {
    if (stream_) {
        return true;
    }
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &device_spec_,
                                        &AudioEngine::audio_stream_callback,
                                        this);
    if (!stream_) {
        std::cerr << "[AudioEngine] Failed to open audio stream: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream_));
    return true;
}

void SDLCALL AudioEngine::audio_stream_callback(void* userdata,
                                                SDL_AudioStream* stream,
                                                int additional_amount,
                                                int) {
    if (!userdata || additional_amount <= 0) {
        return;
    }
    auto* engine = static_cast<AudioEngine*>(userdata);
    const int sample_count = additional_amount / static_cast<int>(sizeof(float));
    if (sample_count <= 0) {
        return;
    }
    std::vector<float> mix_buffer(static_cast<size_t>(sample_count), 0.0f);
    const int frames = engine->device_spec_.channels > 0
                           ? sample_count / engine->device_spec_.channels
                           : 0;
    if (frames > 0) {
        engine->mix_audio(mix_buffer.data(), frames);
    }
    SDL_PutAudioStreamData(stream, mix_buffer.data(), additional_amount);
}

void AudioEngine::mix_audio(float* output, int frames) {
    const int channels = device_spec_.channels;
    if (channels <= 0 || !output) {
        return;
    }
    std::lock_guard<std::mutex> lock(voice_mutex_);
    auto it = voices_.begin();
    while (it != voices_.end()) {
        Voice& voice = *it;
        const size_t total_frames = channels > 0 ? voice.samples.size() / static_cast<size_t>(channels) : 0;
        size_t available = 0;
        if (voice.frame_offset < total_frames) {
            available = total_frames - voice.frame_offset;
        }
        const size_t frames_to_mix = std::min(static_cast<size_t>(frames), available);
        for (size_t i = 0; i < frames_to_mix; ++i) {
            const size_t frame_index = voice.frame_offset + i;
            const size_t sample_index = frame_index * static_cast<size_t>(channels);
            const size_t out_index = i * static_cast<size_t>(channels);
            output[out_index] += voice.samples[sample_index] * voice.left_gain;
            if (channels > 1) {
                output[out_index + 1] += voice.samples[sample_index + 1] * voice.right_gain;
            } else {
                output[out_index] += voice.samples[sample_index] * voice.right_gain;
            }
        }

        voice.frame_offset += frames_to_mix;
        const bool finished = voice.frame_offset >= total_frames;
        if (finished) {
            if (voice.loop && total_frames > 0) {
                voice.frame_offset = 0;
                ++it;
            } else {
                if (voice.is_music) {
                    music_finished_.store(true, std::memory_order_relaxed);
                }
                it = voices_.erase(it);
            }
        } else {
            ++it;
        }
    }

    const size_t total_samples = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    for (size_t i = 0; i < total_samples; ++i) {
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
    }
}

void AudioEngine::init(const std::string& map_id,
                       const nlohmann::json& audio_manifest,
                       const std::string& content_root_hint) {
    shutdown();

    if (!ensure_device()) {
        std::cerr << "[AudioEngine] Audio device unavailable; skipping audio init.\n";
        return;
    }

    std::vector<MusicTrack> loaded;
    std::vector<fs::path> wav_files = collect_music_files(audio_manifest, content_root_hint);

    for (auto it = wav_files.begin(); it != wav_files.end();) {
        try {
            if (!fs::exists(*it)) {
                std::cerr << "[AudioEngine] Music track not found: " << it->u8string() << "\n";
                it = wav_files.erase(it);
                continue;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[AudioEngine] Music path check failed for '" << it->u8string() << "': " << ex.what() << "\n";
            it = wav_files.erase(it);
            continue;
        }
        ++it;
    }

    if (!wav_files.empty()) {
        for (const auto& path : wav_files) {
            std::string abs_path = path.u8string();
            MusicTrack track;
            track.file_path = abs_path;
            if (!load_wav_samples(path, device_spec_, track.samples, track.frames)) {
                continue;
            }
            loaded.emplace_back(std::move(track));
        }
        if (loaded.size() > 1) {
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(loaded.begin(), loaded.end(), rng);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        playlist_ = std::move(loaded);
        current_map_ = map_id;
        next_track_index_ = 0;
        playlist_started_ = false;
    }

    pending_next_track_.store(!playlist_.empty(), std::memory_order_relaxed);

    if (!playlist_.empty()) {
        update();
    }
}

void AudioEngine::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playlist_.clear();
        current_map_.clear();
        next_track_index_ = 0;
        playlist_started_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        voices_.clear();
    }
    pending_next_track_.store(false, std::memory_order_relaxed);
    music_finished_.store(false, std::memory_order_relaxed);
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

void AudioEngine::play_next_track_locked() {
    if (playlist_.empty()) {
        playlist_started_ = false;
        return;
    }

    const size_t total = playlist_.size();
    for (size_t attempt = 0; attempt < total; ++attempt) {
        size_t index = next_track_index_;
        next_track_index_ = (next_track_index_ + 1) % total;
        MusicTrack& track = playlist_[index];
        if (!track.valid()) {
            continue;
        }
        Voice voice;
        voice.samples = track.samples;
        voice.loop = (playlist_.size() == 1);
        voice.is_music = true;
        voice.left_gain = kMusicVolume;
        voice.right_gain = kMusicVolume;
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            voices_.push_back(std::move(voice));
        }
        playlist_started_ = true;
        return;
    }
    playlist_started_ = false;
}

void AudioEngine::update() {
    if (!ensure_device()) {
        return;
    }
    if (music_finished_.exchange(false, std::memory_order_relaxed) ||
        pending_next_track_.exchange(false, std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(mutex_);
        play_next_track_locked();
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!playlist_started_ && !playlist_.empty()) {
        play_next_track_locked();
    }
}

void AudioEngine::set_effect_max_distance(float distance) {
    if (!std::isfinite(distance) || distance <= 0.0f) {
        distance = 1.0f;
    }
    effect_max_distance_.store(distance, std::memory_order_relaxed);
}

void AudioEngine::play_now(const Animation& animation, const Asset& asset) {
    const Animation::AudioClip* clip = animation.audio_data();
    if (!clip || !clip->buffer) {
        return;
    }

    if (!ensure_device()) {
        return;
    }

    const auto& buffer = *clip->buffer;
    if (buffer.samples.empty()) {
        return;
    }
    Uint8* dst_data = nullptr;
    int dst_len = 0;
    if (!SDL_ConvertAudioSamples(&buffer.spec,
                                 buffer.samples.data(),
                                 static_cast<int>(buffer.samples.size()),
                                 &device_spec_,
                                 &dst_data,
                                 &dst_len)) {
        std::cerr << "[AudioEngine] Failed to convert effect audio: " << SDL_GetError() << "\n";
        return;
    }

    float max_distance = effect_max_distance_.load(std::memory_order_relaxed);
    if (max_distance < 1.0f) {
        max_distance = 1.0f;
    }

    float distance = asset.distance_from_camera;
    if (!std::isfinite(distance) || distance < 0.0f) {
        distance = 0.0f;
    }

    float normalized = distance / max_distance;
    if (normalized > 1.0f) normalized = 1.0f;
    if (normalized < 0.0f) normalized = 0.0f;

    const float base_volume = static_cast<float>(clip->volume) / 100.0f;
    float distance_scale = 1.0f - normalized;
    distance_scale = distance_scale * distance_scale;
    float final_volume = base_volume * distance_scale;
    if (final_volume <= 0.0f) {
        SDL_free(dst_data);
        return;
    }

    float pan_basis = std::cos(asset.angle_from_camera);
    if (!std::isfinite(pan_basis)) {
        pan_basis = 0.0f;
    }
    pan_basis = std::clamp(pan_basis, -1.0f, 1.0f);

    float left_mix = 0.5f * (1.0f - pan_basis);
    float right_mix = 0.5f * (1.0f + pan_basis);

    const float crossfeed = 0.2f;
    left_mix = left_mix * (1.0f - crossfeed) + crossfeed;
    right_mix = right_mix * (1.0f - crossfeed) + crossfeed;

    left_mix = std::clamp(left_mix, 0.0f, 1.0f);
    right_mix = std::clamp(right_mix, 0.0f, 1.0f);

    std::vector<float> samples(static_cast<size_t>(dst_len) / sizeof(float));
    std::memcpy(samples.data(), dst_data, static_cast<size_t>(dst_len));
    SDL_free(dst_data);

    Voice voice;
    voice.samples = std::move(samples);
    voice.left_gain = final_volume * left_mix;
    voice.right_gain = final_volume * right_mix;
    voice.loop = false;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        voices_.push_back(std::move(voice));
    }
}
