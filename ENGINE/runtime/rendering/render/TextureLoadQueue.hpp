#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>

namespace texture_loading {

// Asynchronous texture loading system
// Thread-safe loading of textures to eliminate main thread stalls
//
// Usage:
//   1. Create TextureLoadQueue with SDL_Renderer
//   2. Call requestLoad() to queue texture loading (returns immediately)
//   3. Call processCompletedLoads() each frame on main thread
//   4. Callback is invoked when texture is ready
//
class TextureLoadQueue {
public:
    explicit TextureLoadQueue(SDL_Renderer* renderer);
    ~TextureLoadQueue();

    // Non-copyable
    TextureLoadQueue(const TextureLoadQueue&) = delete;
    TextureLoadQueue& operator=(const TextureLoadQueue&) = delete;

    // Request async texture load
    // Callback is invoked on main thread when texture is ready
    // Returns immediately
    void requestLoad(const std::string& path,
                     std::function<void(SDL_Texture*)> callback);

    // Convenience method: Load texture asynchronously and store in destination pointer
    // Initializes destination to placeholder texture immediately
    // Updates destination when real texture is loaded
    void requestLoadInto(const std::string& path,
                         SDL_Texture** destination,
                         int* width = nullptr,
                         int* height = nullptr);

    // Process completed loads on main thread (call each frame)
    // Converts SDL_Surface to SDL_Texture and invokes callbacks
    // Returns number of textures created this frame
    int processCompletedLoads(int max_per_frame = 5);

    // Get placeholder texture for loading state
    SDL_Texture* getPlaceholderTexture();

    // Get statistics
    size_t getPendingCount() const;
    size_t getQueuedCount() const;

    // Shutdown worker thread
    void shutdown();

private:
    struct LoadRequest {
        std::string path;
        SDL_Surface* surface = nullptr;
        std::function<void(SDL_Texture*)> callback;
    };

    SDL_Renderer* renderer_;
    SDL_Texture* placeholder_texture_ = nullptr;

    // Thread-safe queues
    std::queue<LoadRequest> pending_loads_;  // Waiting for worker thread
    std::queue<LoadRequest> ready_for_gpu_;  // Surface loaded, needs GPU upload
    mutable std::mutex pending_mutex_;
    mutable std::mutex ready_mutex_;

    // Worker thread
    std::unique_ptr<std::thread> worker_thread_;
    std::atomic<bool> shutdown_requested_{false};
    std::condition_variable work_available_;

    // Statistics
    std::atomic<size_t> pending_count_{0};
    std::atomic<size_t> queued_count_{0};

    void workerThreadMain();
    void createPlaceholderTexture();
};

} // namespace texture_loading
