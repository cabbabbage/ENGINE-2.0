// asset_tool_cli.cpp
//
// CLI wrapper for ImageCacheGenerator.
// Generates cache structure: cache/<asset>/animations/<anim>/scale_<pct>/{normal,foreground,background}/<idx>.png

#include "image_cache_generator.hpp"

#include <iostream>
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>

// Simple console logger implementation
class ConsoleLogger : public imgcache::ILogger {
public:
    void info(const std::string& msg) override {
        std::cout << "[INFO] " << msg << "\n";
        std::cout.flush();
    }

    void warn(const std::string& msg) override {
        std::cerr << "[WARN] " << msg << "\n";
        std::cerr.flush();
    }

    void error(const std::string& msg) override {
        std::cerr << "[ERROR] " << msg << "\n";
        std::cerr.flush();
    }
};

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]\n\n";
    std::cout << "OPTIONS:\n";
    std::cout << "  --manifest <path>       Path to manifest.json (default: auto-discover)\n";
    std::cout << "  --cache-root <path>     Override cache directory (default: <repo>/cache)\n";
    std::cout << "  --force-rebuild         Force rebuild all frames/variants in selected scope\n";
    std::cout << "  --missing-only          Rebuild only missing output files in selected scope\n";
    std::cout << "  --dry-run               Plan without executing (show what would be done)\n";
    std::cout << "  --asset <name>          Only process specified asset\n";
    std::cout << "  --animation <name>      Only process specified animation (requires --asset)\n";
    std::cout << "  --frame <idx>           Only process specified frame index (requires --asset and --animation)\n";
    std::cout << "  --workers <N>           Number of worker threads (default: CPU count - 1)\n";
    std::cout << "  --effects-backend <B>   Effects backend: auto|cpu|d3d11 (default: auto)\n";
    std::cout << "  --verbose-tasks         Enable per-task progress logs (default: quiet aggregate logs)\n";
    std::cout << "  --help                  Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << prog_name << " --missing-only                   # Repair missing files only\n";
    std::cout << "  " << prog_name << " --force-rebuild                 # Rebuild everything\n";
    std::cout << "  " << prog_name << " --asset player                  # Process only player asset\n";
    std::cout << "  " << prog_name << " --asset player --animation idle # Process only player/idle\n";
}

int main(int argc, char** argv) {
    imgcache::GeneratorOptions opts;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        else if (arg == "--manifest") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --manifest requires a path argument\n";
                return 2;
            }
            opts.manifest_path = argv[++i];
        }
        else if (arg == "--cache-root") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --cache-root requires a path argument\n";
                return 2;
            }
            opts.cache_root_override = argv[++i];
        }
        else if (arg == "--force-rebuild") {
            opts.force_rebuild = true;
        }
        else if (arg == "--missing-only") {
            opts.missing_only = true;
        }
        else if (arg == "--dry-run") {
            opts.dry_run = true;
        }
        else if (arg == "--asset") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --asset requires a name argument\n";
                return 2;
            }
            opts.filters.assets.insert(argv[++i]);
        }
        else if (arg == "--animation") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --animation requires a name argument\n";
                return 2;
            }
            opts.filters.animations.insert(argv[++i]);
        }
        else if (arg == "--frame") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --frame requires an index argument\n";
                return 2;
            }
            try {
                int frame_idx = std::stoi(argv[++i]);
                if (frame_idx < 0) {
                    std::cerr << "Error: frame index must be non-negative\n";
                    return 2;
                }
                opts.filters.source_frames.insert(frame_idx);
            }
            catch (const std::exception& e) {
                std::cerr << "Error: invalid frame index: " << e.what() << "\n";
                return 2;
            }
        }
        else if (arg == "--workers") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --workers requires a number argument\n";
                return 2;
            }
            try {
                int workers = std::stoi(argv[++i]);
                if (workers < 1) {
                    std::cerr << "Error: worker count must be at least 1\n";
                    return 2;
                }
                opts.worker_count_override = static_cast<std::uint32_t>(workers);
            }
            catch (const std::exception& e) {
                std::cerr << "Error: invalid worker count: " << e.what() << "\n";
                return 2;
            }
        }
        else if (arg == "--effects-backend") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --effects-backend requires an argument: auto|cpu|d3d11\n";
                return 2;
            }
            std::string value = argv[++i];
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (value == "auto") {
                opts.effects_backend = imgcache::EffectsBackend::Auto;
            } else if (value == "cpu") {
                opts.effects_backend = imgcache::EffectsBackend::Cpu;
            } else if (value == "d3d11") {
                opts.effects_backend = imgcache::EffectsBackend::D3D11;
            } else {
                std::cerr << "Error: invalid --effects-backend value '" << value
                          << "' (expected auto|cpu|d3d11)\n";
                return 2;
            }
        }
        else if (arg == "--verbose-tasks") {
            opts.quiet_task_logs = false;
        }
        else {
            std::cerr << "Error: unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    // Validate filter combinations
    if (!opts.filters.animations.empty() && opts.filters.assets.empty()) {
        std::cerr << "Error: --animation requires --asset to be specified\n";
        return 2;
    }
    if (!opts.filters.source_frames.empty() &&
        (opts.filters.assets.empty() || opts.filters.animations.empty())) {
        std::cerr << "Error: --frame requires both --asset and --animation to be specified\n";
        return 2;
    }

    // Run generator
    ConsoleLogger logger;
    logger.info("Starting image cache generator (C++ version)");

    auto result = imgcache::ImageCacheGenerator::Run(opts, logger);

    // Report results
    if (result.ok) {
        logger.info("=== Generation Complete ===");
        std::cout << "  Tasks succeeded: " << result.stats.tasks_succeeded
                  << " / " << result.stats.tasks_total << "\n";
        std::cout << "  PNGs written: " << result.stats.pngs_written << "\n";
        std::cout << "  Assets touched: " << result.stats.assets_touched << "\n";
        std::cout << "  Animations touched: " << result.stats.animations_touched << "\n";

        if (result.stats.tasks_total == 0) {
            logger.info("No work required - all cache files up to date");
        }

        return 0;
    }
    else {
        logger.error("=== Generation Failed ===");
        std::cerr << "  Error: " << result.error << "\n";
        std::cerr << "  Tasks succeeded: " << result.stats.tasks_succeeded
                  << " / " << result.stats.tasks_total << "\n";
        std::cerr << "  PNGs written: " << result.stats.pngs_written << "\n";

        return 1;
    }
}
