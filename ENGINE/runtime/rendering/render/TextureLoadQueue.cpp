#include "TextureLoadQueue.hpp"
#include <SDL3_image/SDL_image.h>
#include <iostream>

namespace texture_loading {

TextureLoadQueue::TextureLoadQueue(SDL_Renderer* renderer)
    : renderer_(renderer) {
    if (!renderer_) {
        throw std::runtime_error("TextureLoadQueue: renderer cannot be null");
    }

    createPlaceholderTexture();

    // Start worker thread
    worker_thread_ = std::make_unique<std::thread>(&TextureLoadQueue::workerThreadMain, this);
}

TextureLoadQueue::~TextureLoadQueue() {
    shutdown();

    // Clean up placeholder
    if (placeholder_texture_) {
        SDL_DestroyTexture(placeholder_texture_);
        placeholder_texture_ = nullptr;
    }
}

void TextureLoadQueue::shutdown() {
    if (!worker_thread_) {
        return;
    }

    // Signal shutdown
    shutdown_requested_ = true;
    work_available_.notify_one();

    // Wait for worker thread
    if (worker_thread_->joinable()) {
        worker_thread_->join();
    }
    worker_thread_.reset();

    // Clean up any remaining surfaces
    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        while (!ready_for_gpu_.empty()) {
            auto& req = ready_for_gpu_.front();
            if (req.surface) {
                SDL_DestroySurface(req.surface);
            }
            ready_for_gpu_.pop();
        }
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        while (!pending_loads_.empty()) {
            pending_loads_.pop();
        }
    }
}

void TextureLoadQueue::requestLoad(const std::string& path,
                                    std::function<void(SDL_Texture*)> callback) {
    if (path.empty() || !callback) {
        return;
    }

    LoadRequest req;
    req.path = path;
    req.callback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_loads_.push(std::move(req));
        ++pending_count_;
    }

    work_available_.notify_one();
}

void TextureLoadQueue::requestLoadInto(const std::string& path,
                                        SDL_Texture** destination,
                                        int* width,
                                        int* height) {
    if (!destination || path.empty()) {
        return;
    }

    // Set placeholder immediately
    *destination = getPlaceholderTexture();
    if (width) *width = 64;
    if (height) *height = 64;

    // Queue async load
    requestLoad(path, [destination, width, height](SDL_Texture* texture) {
        if (texture && destination) {
            // Get texture dimensions
            if (width || height) {
                float w_f = 0.0f;
                float h_f = 0.0f;
                if (SDL_GetTextureSize(texture, &w_f, &h_f)) {
                    if (width) *width = static_cast<int>(std::lround(w_f));
                    if (height) *height = static_cast<int>(std::lround(h_f));
                }
            }
            // Update destination
            *destination = texture;
        }
    });
}

int TextureLoadQueue::processCompletedLoads(int max_per_frame) {
    if (!renderer_) {
        return 0;
    }

    int processed = 0;

    while (processed < max_per_frame) {
        LoadRequest req;

        // Get next ready request
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            if (ready_for_gpu_.empty()) {
                break;
            }
            req = std::move(ready_for_gpu_.front());
            ready_for_gpu_.pop();
            --queued_count_;
        }

        // Create texture from surface on main thread
        SDL_Texture* texture = nullptr;
        if (req.surface) {
            texture = SDL_CreateTextureFromSurface(renderer_, req.surface);
            SDL_DestroySurface(req.surface);
            req.surface = nullptr;

            if (!texture) {
                std::cerr << "[TextureLoadQueue] Failed to create texture from surface for '"
                          << req.path << "': " << SDL_GetError() << "\n";
            }
        }

        // Invoke callback
        if (req.callback) {
            req.callback(texture);
        }

        ++processed;
    }

    return processed;
}

SDL_Texture* TextureLoadQueue::getPlaceholderTexture() {
    return placeholder_texture_;
}

size_t TextureLoadQueue::getPendingCount() const {
    return pending_count_.load();
}

size_t TextureLoadQueue::getQueuedCount() const {
    return queued_count_.load();
}

void TextureLoadQueue::workerThreadMain() {
    while (!shutdown_requested_) {
        LoadRequest req;

        // Wait for work
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            work_available_.wait(lock, [this] {
                return !pending_loads_.empty() || shutdown_requested_;
            });

            if (shutdown_requested_) {
                break;
            }

            if (pending_loads_.empty()) {
                continue;
            }

            req = std::move(pending_loads_.front());
            pending_loads_.pop();
            --pending_count_;
        }

        // Load surface on worker thread (thread-safe)
        SDL_Surface* surface = IMG_Load(req.path.c_str());
        if (!surface) {
            std::cerr << "[TextureLoadQueue] Failed to load image '" << req.path
                      << "': " << SDL_GetError() << "\n";

            // Still invoke callback with null texture
            if (req.callback) {
                std::lock_guard<std::mutex> lock(ready_mutex_);
                ready_for_gpu_.push(std::move(req));
                ++queued_count_;
            }
            continue;
        }

        req.surface = surface;

        // Move to ready queue for GPU upload
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            ready_for_gpu_.push(std::move(req));
            ++queued_count_;
        }
    }
}

void TextureLoadQueue::createPlaceholderTexture() {
    // Create a simple 64x64 checkerboard placeholder texture
    const int size = 64;
    const int cell_size = 8;

    SDL_Surface* surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA8888);
    if (!surface) {
        std::cerr << "[TextureLoadQueue] Failed to create placeholder surface: "
                  << SDL_GetError() << "\n";
        return;
    }

    // Fill with checkerboard pattern (gray and light gray)
    SDL_LockSurface(surface);
    Uint32* pixels = static_cast<Uint32*>(surface->pixels);
    const Uint32 color1 = SDL_MapSurfaceRGBA(surface, 128, 128, 128, 255); // Gray
    const Uint32 color2 = SDL_MapSurfaceRGBA(surface, 192, 192, 192, 255); // Light gray

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool checker = ((x / cell_size) + (y / cell_size)) % 2 == 0;
            pixels[y * size + x] = checker ? color1 : color2;
        }
    }
    SDL_UnlockSurface(surface);

    placeholder_texture_ = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);

    if (!placeholder_texture_) {
        std::cerr << "[TextureLoadQueue] Failed to create placeholder texture: "
                  << SDL_GetError() << "\n";
    }
}

} // namespace texture_loading
