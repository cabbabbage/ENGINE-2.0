#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

namespace json_coercion {

inline bool read_bool_like(const nlohmann::json& value, bool fallback) {
    if (value.is_boolean()) {
        try {
            return value.get<bool>();
        } catch (...) {
        }
    }
    if (value.is_number_integer()) {
        try {
            return value.get<int>() != 0;
        } catch (...) {
        }
    }
    if (value.is_number_float()) {
        try {
            return value.get<double>() != 0.0;
        } catch (...) {
        }
    }
    if (value.is_string()) {
        try {
            std::string text = value.get<std::string>();
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (text == "true" || text == "1" || text == "yes" || text == "on") {
                return true;
            }
            if (text == "false" || text == "0" || text == "no" || text == "off") {
                return false;
            }
        } catch (...) {
        }
    }
    return fallback;
}

inline int read_int_like(const nlohmann::json& value, int fallback) {
    if (value.is_number_integer()) {
        try {
            return value.get<int>();
        } catch (...) {
        }
    }
    if (value.is_number()) {
        try {
            return static_cast<int>(value.get<double>());
        } catch (...) {
        }
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

inline float read_float_like(const nlohmann::json& value, float fallback) {
    if (value.is_number()) {
        try {
            return static_cast<float>(value.get<double>());
        } catch (...) {
        }
    }
    if (value.is_string()) {
        try {
            return std::stof(value.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

inline std::string read_string_like(const nlohmann::json& value, const std::string& fallback) {
    if (value.is_string()) {
        try {
            return value.get<std::string>();
        } catch (...) {
            return fallback;
        }
    }
    if (value.is_number_integer()) {
        try {
            return std::to_string(value.get<long long>());
        } catch (...) {
            return fallback;
        }
    }
    if (value.is_number_float()) {
        try {
            return std::to_string(value.get<double>());
        } catch (...) {
            return fallback;
        }
    }
    if (value.is_boolean()) {
        try {
            return value.get<bool>() ? "true" : "false";
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

inline const nlohmann::json* find_field(const nlohmann::json& payload, const char* key) {
    if (!payload.is_object() || key == nullptr) {
        return nullptr;
    }
    const auto it = payload.find(key);
    if (it == payload.end()) {
        return nullptr;
    }
    return &(*it);
}

inline bool read_bool_field_like(const nlohmann::json& payload, const char* key, bool fallback) {
    if (const nlohmann::json* value = find_field(payload, key)) {
        return read_bool_like(*value, fallback);
    }
    return fallback;
}

inline int read_int_field_like(const nlohmann::json& payload, const char* key, int fallback) {
    if (const nlohmann::json* value = find_field(payload, key)) {
        return read_int_like(*value, fallback);
    }
    return fallback;
}

inline float read_float_field_like(const nlohmann::json& payload, const char* key, float fallback) {
    if (const nlohmann::json* value = find_field(payload, key)) {
        return read_float_like(*value, fallback);
    }
    return fallback;
}

inline std::string read_string_field_like(const nlohmann::json& payload,
                                          const char* key,
                                          const std::string& fallback) {
    if (const nlohmann::json* value = find_field(payload, key)) {
        return read_string_like(*value, fallback);
    }
    return fallback;
}

} // namespace json_coercion
