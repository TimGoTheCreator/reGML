#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace regml {
struct GmrtObjectEvent {
    int type = 0;
    int number = 0;
    std::string global_name;
};

struct GmrtObjectResource {
    std::string name;
    int id = -1;
    int parent = -1;
    int sprite = -1;
    int mask = -1;
    int depth = 0;
    bool visible = true;
    bool persistent = false;
    bool solid = false;
    std::vector<GmrtObjectEvent> events;
};

struct GmrtRoomPlacement {
    int object = -1;
    int id = 0;
    int layer = -1;
    double x = 0;
    double y = 0;
    double scale_x = 1;
    double scale_y = 1;
    double image_speed = 1;
    double image_index = 0;
    double rotation = 0;
    unsigned color = 0xffffffff;
};

struct GmrtRoomSprite {
    int sprite = -1;
    int id = 0;
    int depth = 0;
    double x = 0;
    double y = 0;
    double scale_x = 1;
    double scale_y = 1;
    double frame = 0;
    double speed = 1;
    double rotation = 0;
    unsigned color = 0xffffff;
    double alpha = 1;
};

struct GmrtRoomLayer {
    std::string name;
    int id = -1;
    int depth = 0;
    int type = 0;
    double x = 0;
    double y = 0;
    bool visible = true;
    std::vector<int> instances;
    std::vector<GmrtRoomSprite> sprites;
};

struct GmrtRoomResource {
    std::string name;
    int id = -1;
    int width = 640;
    int height = 480;
    int speed = 60;
    bool persistent = false;
    unsigned color = 0;
    bool views_enabled = false;
    std::vector<GmrtRoomPlacement> instances;
    std::vector<GmrtRoomLayer> layers;
};

struct GmrtSpriteResource {
    std::string name;
    int id = -1;
    int width = 0;
    int height = 0;
    int origin_x = 0;
    int origin_y = 0;
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    bool smooth = false;
    std::vector<std::filesystem::path> frames;
};

class GmrtProject {
  public:
    static GmrtProject load_for_wad(const std::filesystem::path &wad_path);

    [[nodiscard]] const std::filesystem::path &index_path() const noexcept { return index_path_; }
    [[nodiscard]] const std::vector<GmrtObjectResource> &objects() const noexcept { return objects_; }
    [[nodiscard]] const std::vector<GmrtRoomResource> &rooms() const noexcept { return rooms_; }
    [[nodiscard]] const std::vector<GmrtSpriteResource> &sprites() const noexcept { return sprites_; }
    [[nodiscard]] const std::vector<int> &room_order() const noexcept { return room_order_; }
    [[nodiscard]] const std::vector<std::string> &event_global_names() const noexcept { return event_indices_; }
    [[nodiscard]] int start_room() const noexcept { return start_room_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] int asset_index(std::string_view name) const noexcept;
    [[nodiscard]] int asset_type(std::string_view name) const noexcept;
    [[nodiscard]] bool asset_exists(int index, int type) const noexcept;

  private:
    std::filesystem::path index_path_;
    int start_room_ = 0;
    int width_ = 640;
    int height_ = 480;
    std::vector<GmrtObjectResource> objects_;
    std::vector<GmrtRoomResource> rooms_;
    std::vector<GmrtSpriteResource> sprites_;
    std::vector<int> room_order_;
    std::vector<std::string> event_indices_;
    std::unordered_map<std::string, int> asset_indices_;
    std::unordered_map<std::string, int> asset_types_;
};
} // namespace regml
