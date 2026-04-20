#pragma once

#include "animation.hpp"
#include "utils/area.hpp"
#include <map>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace devmode::core {
class ManifestStore;
}

struct SDL_Renderer;

struct MappingOption {
	std::string animation;
	float percent;
};

struct MappingEntry {
	std::string condition;
	std::vector<MappingOption> options;
};

using Mapping = std::vector<MappingEntry>;

class AssetInfo {

        public:
    static constexpr std::uint8_t kTextureVariantNone = 0u;
    static constexpr std::uint8_t kTextureVariantNormal = 1u << 0;
    static constexpr std::uint8_t kTextureVariantAll = kTextureVariantNormal;

    struct AnimationTextureRebuildRequest {
        std::uint8_t all_frames_variants = kTextureVariantNone;
        std::unordered_map<int, std::uint8_t> frame_variants;

        bool empty() const;
        void clear();
        void mark_animation(std::uint8_t variants);
        void mark_frame(int frame_index, std::uint8_t variants);
        void merge(const AnimationTextureRebuildRequest& other);
    };

    struct TextureRebuildBucket {
        bool bundle_refresh_required = false;
        std::unordered_map<std::string, AnimationTextureRebuildRequest> animations;

        bool empty() const;
        void clear();
        void mark_bundle_refresh();
        void mark_animation(const std::string& animation_name, std::uint8_t variants);
        void mark_frame(const std::string& animation_name, int frame_index, std::uint8_t variants);
        void merge(const TextureRebuildBucket& other);
    };

    struct RuntimeTextureRebuildState {
        TextureRebuildBucket pending_on_load;
        TextureRebuildBucket pending_on_close;

        void clear();
    };

    SDL_Texture* preview_texture = nullptr;
    AssetInfo(const std::string &asset_folder_name);
    AssetInfo(const std::string &asset_folder_name, const nlohmann::json& metadata);
    static std::shared_ptr<AssetInfo> from_manifest_entry(const std::string& asset_folder_name, const nlohmann::json& metadata);
    using ManifestStoreProvider = std::function<devmode::core::ManifestStore*()>;
    static void set_manifest_store_provider(ManifestStoreProvider provider);
    ~AssetInfo();

    bool has_tag(const std::string &tag) const;
    std::string name;
    std::string type;
    std::string start_animation;
    bool passable;
    int min_same_type_distance;
    int min_distance_all;
    int starting_health = 100;
    float scale_factor;
    float size_variation_percent = 0.0f;
    float weight_kg = 0.0f;
    bool smooth_scaling = true;
    int original_canvas_width = 0;
    int original_canvas_height = 0;
    bool flipable;
    bool tillable = false;
    std::vector<std::string> tags;
    std::vector<std::string> anti_tags;

    struct AnchorChildPointCandidate {
        std::string anchor_point_name;
        nlohmann::json candidates = nlohmann::json::object();
    };
    std::vector<AnchorChildPointCandidate> anchor_point_child_candidates;

    struct OvalAnchorPoint {
        float angle_degrees = 0.0f;
        int texture_x = 0;
        int texture_y = 0;
        float depth_offset = 0.0f;
        float rotation_degrees = 0.0f;
        bool hidden = false;
        bool resolve_x = true;
        AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;
        std::vector<std::string> tags;
        std::vector<std::string> anti_tags;
    };

    struct OvalAnchorMapping {
        std::string name;
        std::string asset_name;
        float width_radius_x = 48.0f;
        float height_radius_z = 24.0f;
        float radius_offset_degrees = 0.0f;
        std::vector<std::string> legacy_names;
        std::vector<OvalAnchorPoint> points;

        bool valid() const {
            return !name.empty();
        }
    };
    std::vector<OvalAnchorMapping> oval_anchor_mappings;

    struct FloorBox {
        struct CandidatePayload {
            nlohmann::json candidates = nlohmann::json::array();
            int grid_resolution = 4;
        };

        std::string id;
        std::string name;
        float position_x = 0.0f;
        float position_z = 0.0f;
        float width = 0.0f;
        float depth = 0.0f;
        bool enabled = true;
        std::vector<std::string> tags;
        std::optional<CandidatePayload> candidate;
    };

    struct ImpassableShapePoint {
        int x = 0;
        int y = 0;
    };

    struct ImpassableShape {
        std::string id;
        std::string name;
        bool enabled = true;
        std::vector<ImpassableShapePoint> points;

        bool valid() const { return enabled && points.size() >= 3; }
    };

    bool movement_enabled = false;
    bool attack_box_enabled = false;
    bool hitbox_enabled = false;
    bool impassable_enabled = false;
    bool impassable_box_enabled = false; // Legacy in-memory flag. Not serialized.
    bool floor_boxes_enabled = false;
    std::vector<FloorBox> floor_boxes;
    std::vector<ImpassableShape> impassable_shapes;
    std::vector<animation_update::FrameHitBox> impassable_boxes; // Legacy editor cache. Not serialized.
    std::vector<float>  scale_variants;
    struct NamedArea {
        struct RenderFrame {
            int width = 0;
            int height = 0;
            int pivot_x = 0;
            int pivot_y = 0;
            float pixel_scale = 1.0f;

            bool is_valid() const {
                return width > 0 && height > 0 && std::isfinite(pixel_scale) && pixel_scale > 0.0f;
            }
};
        std::string name;
        std::string type;
        std::string kind;
        std::unique_ptr<Area> area;
        std::optional<RenderFrame> render_frame;

        std::string attachment_subtype;
        bool        attachment_is_on_top = false;
};
    std::vector<NamedArea> areas;
    std::map<std::string, Animation> animations;
    std::map<std::string, Mapping> mappings;

	public:
    void loadAnimations(SDL_Renderer* renderer,
                        bool include_all_animations = true,
                        bool assume_cache_ready = false);
    bool commit_manifest();
    void mark_dirty();
    bool is_dirty() const;
    RuntimeTextureRebuildState& runtime_texture_rebuild_state() { return runtime_texture_rebuild_state_; }
    const RuntimeTextureRebuildState& runtime_texture_rebuild_state() const { return runtime_texture_rebuild_state_; }
    void clear_runtime_texture_rebuild_state();
    void mark_texture_rebuild_on_close(const std::string& animation_name, std::uint8_t variants = kTextureVariantAll);
    void mark_texture_frame_rebuild_on_close(const std::string& animation_name,
                                             int frame_index,
                                             std::uint8_t variants = kTextureVariantAll);
    void mark_all_animation_textures_on_close(std::uint8_t variants = kTextureVariantAll);
    void mark_bundle_refresh_on_close();
    void mark_texture_rebuild_on_load(const std::string& animation_name, std::uint8_t variants = kTextureVariantAll);
    void mark_texture_frame_rebuild_on_load(const std::string& animation_name,
                                            int frame_index,
                                            std::uint8_t variants = kTextureVariantAll);
    TextureRebuildBucket consume_pending_texture_rebuild_on_close();
    TextureRebuildBucket consume_pending_texture_rebuild_on_load();
    void merge_pending_texture_rebuild_on_close(const TextureRebuildBucket& pending);
    void merge_pending_texture_rebuild_on_load(const TextureRebuildBucket& pending);
    void classify_animation_snapshot_rebuilds(const nlohmann::json& before_snapshot,
                                              const nlohmann::json& after_snapshot);
    bool save_self_to_manifest(devmode::core::ManifestStore* store = nullptr);
    bool save_self_to_cache_if_dirty(SDL_Renderer* renderer = nullptr);
    nlohmann::json manifest_payload() const;
    void set_asset_type(const std::string &t);
    void set_min_same_type_distance(int d);
    void set_min_distance_all(int d);
    void set_neighbor_search_radius(int radius);
    void set_flipable(bool v);
    void set_starting_health(int health);
    void set_scale_factor(float factor);
    void set_scale_percentage(float percent);
    void set_size_variation_percentage(float percent);
    void set_weight_kg(float weight);
    float get_weight_kg() const { return weight_kg; }
    void set_scale_filter(bool smooth);
    void set_tags(const std::vector<std::string> &t);
    void add_tag(const std::string &tag);
    void remove_tag(const std::string &tag);
    void set_anti_tags(const std::vector<std::string> &t);
    void add_anti_tag(const std::string &tag);
    void remove_anti_tag(const std::string &tag);
    void set_passable(bool v);
    void set_tillable(bool v);
    Area* find_area(const std::string& name);
    void upsert_area_from_editor(const class Area& area, std::optional<NamedArea::RenderFrame> frame = std::nullopt);
    std::string pick_next_animation(const std::string& mapping_id) const;
    int NeighborSearchRadius = 500;

    void set_spawn_groups_payload(const nlohmann::json& groups);
    nlohmann::json spawn_groups_payload() const;
    void set_anchor_point_child_candidates_payload(const nlohmann::json& candidates);
    nlohmann::json anchor_point_child_candidates_payload() const;
    nlohmann::json anchor_point_child_candidate_candidates(const std::string& anchor_point_name) const;
    bool upsert_anchor_point_child_candidate(const std::string& anchor_point_name, const nlohmann::json& candidates);
    bool rename_anchor_point_child_candidate(const std::string& old_name, const std::string& new_name);
    bool remove_anchor_point_child_candidate(const std::string& anchor_point_name);
    bool reconcile_anchor_point_child_candidates(const std::vector<std::string>& canonical_anchor_names);
    void set_oval_anchor_mappings_payload(const nlohmann::json& mappings);
    nlohmann::json oval_anchor_mappings_payload() const;
    bool upsert_oval_anchor_mapping(const OvalAnchorMapping& mapping);
    bool remove_oval_anchor_mapping(const std::string& mapping_name);
    bool rename_oval_anchor_mapping(const std::string& old_name,
                                    const std::string& new_name,
                                    bool append_legacy_alias = true);
    const OvalAnchorMapping* find_oval_anchor_mapping(const std::string& mapping_name,
                                                      bool include_legacy_aliases = true) const;
    static std::string oval_center_anchor_name_for_mapping(const std::string& mapping_name);
    bool ensure_oval_center_anchors_for_mapping(const std::string& mapping_name,
                                                bool include_legacy_aliases = true);
    bool ensure_oval_center_anchors_for_all_mappings();
    bool rename_oval_center_anchors_for_mapping(const std::string& old_mapping_name,
                                                const std::string& new_mapping_name,
                                                bool include_legacy_aliases = true);

    std::string info_json_path() const { return info_json_path_; }
    std::string asset_dir_path() const { return dir_path_; }

    const std::unordered_set<std::string>& tag_lookup() const { return tag_lookup_; }
    const std::unordered_set<std::string>& anti_tag_lookup() const { return anti_tag_lookup_; }

    bool remove_area(const std::string& name);
    bool rename_area(const std::string& old_name, const std::string& new_name);
    bool is_movement_enabled() const { return movement_enabled; }
    bool is_hitbox_enabled() const { return hitbox_enabled; }
    bool is_attack_box_enabled() const { return attack_box_enabled; }
    bool is_impassable_box_enabled() const { return impassable_enabled || impassable_box_enabled; }
    bool is_floor_boxes_enabled() const { return floor_boxes_enabled; }
    const std::vector<FloorBox>& floor_boxes_payload() const { return floor_boxes; }
    const std::vector<ImpassableShape>& impassable_shapes_payload() const {
        return impassable_shapes;
    }
    const std::vector<animation_update::FrameHitBox>& impassable_boxes_payload() const { return impassable_boxes; }

    std::vector<std::string> animation_names() const;

    nlohmann::json animation_payload(const std::string& name) const;

    bool upsert_animation(const std::string& name, const nlohmann::json& payload);

    bool remove_animation(const std::string& name);

    bool rename_animation(const std::string& old_name, const std::string& new_name);

    void set_start_animation_name(const std::string& name);

    bool reload_animations_from_disk();

    struct AnimationUpdateResult {
        bool changed = false;
        bool animation_changed = false;
        bool start_changed = false;
        std::uint8_t variant_mask = kTextureVariantNone;
        bool structural = false;
    };

    AnimationUpdateResult update_animation_properties_detailed(const std::string& animation_name,
                                                               const nlohmann::json& properties);
    bool update_animation_properties(const std::string& animation_name, const nlohmann::json& properties);

    struct AreaCodec {
        static SDL_Point scaled_anchor(const AssetInfo& info, std::optional<float> scale_override = std::nullopt);

        static nlohmann::json encode_entry( const AssetInfo& info, const Area& area, const std::string& final_type, const std::string& final_kind, std::optional<NamedArea::RenderFrame> frame = std::nullopt);

        static std::optional<NamedArea> decode_entry(const AssetInfo& info, const nlohmann::json& entry);
};

    void set_spawn_groups(const nlohmann::json& groups);

        private:
    void load_base_properties(const nlohmann::json &data);
    void load_animations(const nlohmann::json& data);
    void load_areas(const nlohmann::json &data);
    nlohmann::json anims_json_;
    std::string dir_path_;
    nlohmann::json info_json_;
    std::string info_json_path_;
    void initialize_from_json(const nlohmann::json& data);
    void sync_anchor_point_child_candidates_info_json();
    void sync_oval_anchor_mappings_info_json();
    void rebuild_tag_cache();
    void rebuild_anti_tag_cache();
    static std::uint8_t sanitize_texture_variant_mask(std::uint8_t variants);
    static std::uint8_t classify_texture_rebuild_variants(const nlohmann::json& before_payload,
                                                          const nlohmann::json& after_payload);
    std::unordered_set<std::string> tag_lookup_;
    std::unordered_set<std::string> anti_tag_lookup_;
    RuntimeTextureRebuildState runtime_texture_rebuild_state_;
    bool dirty_ = false;
    friend class AnimationLoader;
    friend class PrimaryAssetCache;
#if defined(ASSET_INFO_ENABLE_TEST_ACCESS)
    friend struct AssetInfoTestAccess;
#endif
};

#if defined(ASSET_INFO_ENABLE_TEST_ACCESS)
struct AssetInfoTestAccess {
    static void initialize_info_json(AssetInfo& info, nlohmann::json data);
    static void rebuild_tag_cache(AssetInfo& info);
    static void rebuild_anti_tag_cache(AssetInfo& info);
};
#endif
