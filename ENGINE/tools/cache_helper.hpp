#pragma once

// cache_helper.hpp (header-only)
// C++ parity target for Python cache_helper.py JSON normalization + stable hashing + compare/update.
//
// What this file provides (fully implemented):
// - Recursive JSON normalization (sorted keys for objects, arrays preserved)
// - Stable hash: Normalize -> compact JSON (no whitespace) -> BLAKE2b digest_size=16 -> hex
// - Load JSON from disk
// - Write JSON to disk (indent=2, sorted keys via Normalize), with atomic replace
// - CompareAndUpdateJson: missing/unreadable/different handling with optional write
// - Inspect: load, normalize, hash
//
// Dependency: nlohmann::json (header-only)

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace imgcache {

struct JsonIOResult {
    bool ok = false;
    std::string error;
    nlohmann::json value;
};

struct CompareUpdateResult {
    bool matches = false;              // true only if existing equals snippet after normalization
    bool wrote = false;                // true only if a write occurred (write_if_different==true)
    bool existing_unreadable = false;  // true if file missing or parse/read failed
    std::string error;                 // may contain read and/or write error messages
};

struct InspectResult {
    bool ok = false;
    std::string error;
    nlohmann::json normalized;
    std::string stable_hash_hex;
};

class CacheHelper final {
public:
    static inline nlohmann::json Normalize(const nlohmann::json& j) {
        if (j.is_object()) {
            std::map<std::string, nlohmann::json> sorted;
            for (auto it = j.begin(); it != j.end(); ++it) {
                sorted.emplace(it.key(), Normalize(it.value()));
            }
            nlohmann::json out = nlohmann::json::object();
            for (const auto& kv : sorted) {
                out[kv.first] = kv.second;
            }
            return out;
        }

        if (j.is_array()) {
            nlohmann::json out = nlohmann::json::array();
            out.reserve(j.size());
            for (const auto& v : j) {
                out.push_back(Normalize(v));
            }
            return out;
        }

        return j;
    }

    static inline std::string StableHashHex_BLAKE2b16(const nlohmann::json& snippet) {
        const nlohmann::json normalized = Normalize(snippet);

        // Python parity: json.dumps(..., sort_keys=True, separators=(",", ":"))
        // Normalize() enforces sorted key order; dump(-1) produces compact JSON.
        const std::string payload = normalized.dump(-1);

        std::array<std::uint8_t, 16> digest{};
        if (!detail::blake2b(reinterpret_cast<const std::uint8_t*>(payload.data()),
                             payload.size(),
                             digest.data(),
                             digest.size())) {
            return {};
        }
        return detail::hex_lower(digest.data(), digest.size());
    }

    static inline JsonIOResult LoadJsonFile(const std::string& path) {
        JsonIOResult r;
        r.ok = false;

        std::string err;
        const std::string text = detail::read_all_text(path, err);
        if (!err.empty()) {
            r.error = err;
            return r;
        }

        try {
            r.value = nlohmann::json::parse(text);
            r.ok = true;
            return r;
        } catch (const std::exception& e) {
            r.error = std::string("JSON parse failed: ") + e.what();
            return r;
        }
    }

    static inline JsonIOResult WriteJsonFile(const std::string& path, const nlohmann::json& data) {
        JsonIOResult r;
        r.ok = false;

        try {
            const nlohmann::json normalized = Normalize(data);

            // Python parity: indent=2, sort_keys=True (we already normalized with sorted keys)
            const std::string text = normalized.dump(2) + "\n";

            std::string err;
            if (!detail::write_all_text_atomic(path, text, err)) {
                r.error = err;
                return r;
            }

            r.ok = true;
            r.value = normalized;
            return r;
        } catch (const std::exception& e) {
            r.error = std::string("Exception during WriteJsonFile: ") + e.what();
            return r;
        }
    }

    static inline CompareUpdateResult CompareAndUpdateJson(
        const nlohmann::json& snippet,
        const std::string& json_path,
        bool write_if_different = true
    ) {
        CompareUpdateResult out;

        const nlohmann::json normalized_snippet = Normalize(snippet);

        JsonIOResult existing = LoadJsonFile(json_path);
        if (!existing.ok) {
            out.matches = false;
            out.existing_unreadable = true;
            out.error = existing.error;

            if (write_if_different) {
                JsonIOResult w = WriteJsonFile(json_path, normalized_snippet);
                out.wrote = w.ok;
                if (!w.ok) {
                    if (!out.error.empty()) out.error += " | ";
                    out.error += w.error;
                }
            }
            return out;
        }

        const nlohmann::json normalized_existing = Normalize(existing.value);

        if (normalized_existing == normalized_snippet) {
            out.matches = true;
            out.wrote = false;
            out.existing_unreadable = false;
            out.error.clear();
            return out;
        }

        out.matches = false;
        out.existing_unreadable = false;

        if (write_if_different) {
            JsonIOResult w = WriteJsonFile(json_path, normalized_snippet);
            out.wrote = w.ok;
            if (!w.ok) out.error = w.error;
        }

        return out;
    }

    static inline InspectResult Inspect(const std::string& json_path) {
        InspectResult r;

        JsonIOResult in = LoadJsonFile(json_path);
        if (!in.ok) {
            r.ok = false;
            r.error = in.error;
            return r;
        }

        r.normalized = Normalize(in.value);
        r.stable_hash_hex = StableHashHex_BLAKE2b16(r.normalized);
        if (r.stable_hash_hex.empty()) {
            r.ok = false;
            r.error = "Failed to compute BLAKE2b hash.";
            return r;
        }

        r.ok = true;
        return r;
    }

private:
    CacheHelper() = delete;

    struct detail {
        static inline std::uint64_t rotr64(std::uint64_t w, unsigned c) {
            return (w >> c) | (w << (64U - c));
        }

        static inline std::uint64_t load64_le(const void* src) {
            const std::uint8_t* p = static_cast<const std::uint8_t*>(src);
            return (std::uint64_t)p[0]
                 | ((std::uint64_t)p[1] << 8)
                 | ((std::uint64_t)p[2] << 16)
                 | ((std::uint64_t)p[3] << 24)
                 | ((std::uint64_t)p[4] << 32)
                 | ((std::uint64_t)p[5] << 40)
                 | ((std::uint64_t)p[6] << 48)
                 | ((std::uint64_t)p[7] << 56);
        }

        static inline void store64_le(void* dst, std::uint64_t w) {
            std::uint8_t* p = static_cast<std::uint8_t*>(dst);
            p[0] = (std::uint8_t)(w);
            p[1] = (std::uint8_t)(w >> 8);
            p[2] = (std::uint8_t)(w >> 16);
            p[3] = (std::uint8_t)(w >> 24);
            p[4] = (std::uint8_t)(w >> 32);
            p[5] = (std::uint8_t)(w >> 40);
            p[6] = (std::uint8_t)(w >> 48);
            p[7] = (std::uint8_t)(w >> 56);
        }

        static inline void G(std::uint64_t& a, std::uint64_t& b, std::uint64_t& c, std::uint64_t& d,
                             std::uint64_t x, std::uint64_t y) {
            a = a + b + x;
            d = rotr64(d ^ a, 32);
            c = c + d;
            b = rotr64(b ^ c, 24);
            a = a + b + y;
            d = rotr64(d ^ a, 16);
            c = c + d;
            b = rotr64(b ^ c, 63);
        }

        static inline void compress(std::uint64_t h[8], const std::uint8_t block[128],
                                    std::uint64_t t0, std::uint64_t t1, bool last) {
            static constexpr std::uint64_t IV[8] = {
                0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL,
                0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL,
                0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
                0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL
            };

            static constexpr std::uint8_t SIGMA[12][16] = {
                { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
                {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3 },
                {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4 },
                { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8 },
                { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13 },
                { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9 },
                {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11 },
                {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10 },
                { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5 },
                {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0 },
                { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
                {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3 }
            };

            std::uint64_t m[16];
            for (int i = 0; i < 16; ++i) {
                m[i] = load64_le(block + i * 8);
            }

            std::uint64_t v[16];
            for (int i = 0; i < 8; ++i) v[i] = h[i];
            for (int i = 0; i < 8; ++i) v[i + 8] = IV[i];

            v[12] ^= t0;
            v[13] ^= t1;
            if (last) v[14] = ~v[14];

            for (int r = 0; r < 12; ++r) {
                const std::uint8_t* s = SIGMA[r];

                G(v[0], v[4], v[ 8], v[12], m[s[0]],  m[s[1]]);
                G(v[1], v[5], v[ 9], v[13], m[s[2]],  m[s[3]]);
                G(v[2], v[6], v[10], v[14], m[s[4]],  m[s[5]]);
                G(v[3], v[7], v[11], v[15], m[s[6]],  m[s[7]]);

                G(v[0], v[5], v[10], v[15], m[s[8]],  m[s[9]]);
                G(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
                G(v[2], v[7], v[ 8], v[13], m[s[12]], m[s[13]]);
                G(v[3], v[4], v[ 9], v[14], m[s[14]], m[s[15]]);
            }

            for (int i = 0; i < 8; ++i) {
                h[i] ^= v[i] ^ v[i + 8];
            }
        }

        static inline bool blake2b(const std::uint8_t* in, size_t inlen,
                                   std::uint8_t* out, size_t outlen) {
            if (!out || outlen == 0 || outlen > 64) return false;

            static constexpr std::uint64_t IV[8] = {
                0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL,
                0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL,
                0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
                0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL
            };

            std::uint64_t h[8];
            for (int i = 0; i < 8; ++i) h[i] = IV[i];

            // Parameter block: outlen, keylen=0, fanout=1, depth=1
            h[0] ^= 0x01010000ULL ^ static_cast<std::uint64_t>(outlen);

            std::uint64_t t0 = 0;
            std::uint64_t t1 = 0;

            std::uint8_t block[128] = {0};

            while (inlen > 128) {
                std::memcpy(block, in, 128);
                t0 += 128;
                if (t0 < 128) t1 += 1;
                compress(h, block, t0, t1, false);
                in += 128;
                inlen -= 128;
            }

            std::memset(block, 0, 128);
            std::memcpy(block, in, inlen);
            t0 += static_cast<std::uint64_t>(inlen);
            if (t0 < inlen) t1 += 1;
            compress(h, block, t0, t1, true);

            std::uint8_t full[64];
            for (int i = 0; i < 8; ++i) store64_le(full + i * 8, h[i]);
            std::memcpy(out, full, outlen);
            return true;
        }

        static inline std::string hex_lower(const std::uint8_t* data, size_t len) {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string s;
            s.resize(len * 2);
            for (size_t i = 0; i < len; ++i) {
                const std::uint8_t b = data[i];
                s[i * 2 + 0] = kHex[(b >> 4) & 0xF];
                s[i * 2 + 1] = kHex[(b >> 0) & 0xF];
            }
            return s;
        }

        static inline std::string read_all_text(const std::string& path, std::string& err) {
            err.clear();
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                err = "Failed to open for read: " + path;
                return {};
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            if (!in.good() && !in.eof()) {
                err = "Failed while reading: " + path;
                return {};
            }
            return ss.str();
        }

        static inline bool write_all_text_atomic(const std::string& path,
                                                const std::string& text,
                                                std::string& err) {
            err.clear();
            try {
                namespace fs = std::filesystem;

                fs::path p(path);
                fs::path parent = p.parent_path();
                if (!parent.empty()) {
                    std::error_code ec;
                    fs::create_directories(parent, ec);
                    if (ec) {
                        err = "Failed to create directories: " + parent.string() + " (" + ec.message() + ")";
                        return false;
                    }
                }

                fs::path tmp = p;
                tmp += ".tmp";

                {
                    std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
                    if (!out) {
                        err = "Failed to open for write: " + tmp.string();
                        return false;
                    }
                    out.write(text.data(), static_cast<std::streamsize>(text.size()));
                    if (!out) {
                        err = "Failed while writing: " + tmp.string();
                        return false;
                    }
                }

                std::error_code ec;
                fs::rename(tmp, p, ec);
                if (!ec) return true;

                // Windows: rename over existing can fail, try remove then rename
                std::error_code ec2;
                fs::remove(p, ec2);
                fs::rename(tmp, p, ec);
                if (ec) {
                    std::error_code ec3;
                    fs::remove(tmp, ec3);
                    err = "Failed to replace file: " + p.string() + " (" + ec.message() + ")";
                    return false;
                }
                return true;
            } catch (const std::exception& e) {
                err = std::string("Exception during write: ") + e.what();
                return false;
            }
        }
    };
};

} // namespace imgcache
