#pragma once

namespace app::startup_runtime {

bool env_flag_enabled(const char* name, bool default_value);
int env_int_clamped(const char* name, int default_value, int min_value, int max_value);

} // namespace app::startup_runtime
