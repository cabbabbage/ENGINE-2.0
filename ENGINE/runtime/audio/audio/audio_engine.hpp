#pragma once

#include <SDL3/SDL.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class Animation;
class Asset;

class AudioEngine {
public:
    static AudioEngine& instance();

    void init(const std::string& map_id, const nlohmann::json& audio_manifest, const std::string& content_root_hint);
    void shutdown();
    void update();

    void set_effect_max_distance(float distance);

    void play_now(const Animation& animation, const Asset& asset);

private:
    AudioEngine() = default;
    ~AudioEngine() = default;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    struct Voice {
        std::vector<float> samples;
        size_t frame_offset = 0;
        float left_gain = 1.0f;
        float right_gain = 1.0f;
        bool loop = false;
        bool is_music = false;
    };

    struct MusicTrack {
        std::vector<float> samples;
        size_t frames = 0;
        std::string file_path;
        bool valid() const { return !samples.empty(); }
    };

    static void SDLCALL audio_stream_callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
    bool ensure_device();
    void mix_audio(float* output, int frames);
    void play_next_track_locked();

    std::mutex mutex_;
    std::vector<MusicTrack> playlist_;
    std::mutex voice_mutex_;
    std::vector<Voice> voices_;
    std::string current_map_;
    std::atomic<bool> pending_next_track_{false};
    std::atomic<bool> music_finished_{false};
    std::atomic<float> effect_max_distance_{1200.0f};
    size_t next_track_index_ = 0;
    bool playlist_started_ = false;
    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioSpec device_spec_{SDL_AUDIO_F32, 2, 44100};
};
