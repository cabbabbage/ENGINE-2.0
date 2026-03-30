#pragma once

#include "image_cache_generator.hpp"

#include <string>

namespace imgcache {

// Windows D3D11 compute backend for image effects.
// Safe to call on non-Windows: it will report unavailable and no-op.
class D3D11EffectsBackend final {
public:
    static D3D11EffectsBackend& Instance();

    bool IsAvailable(std::string& reason);

    bool ApplyEffects(const ImageRGBA& expanded_src,
                      const EffectsParams& fx,
                      EffectLayerMode mode,
                      ImageRGBA& out,
                      std::string& err);

private:
    D3D11EffectsBackend();
    ~D3D11EffectsBackend();

    D3D11EffectsBackend(const D3D11EffectsBackend&) = delete;
    D3D11EffectsBackend& operator=(const D3D11EffectsBackend&) = delete;
};

} // namespace imgcache

