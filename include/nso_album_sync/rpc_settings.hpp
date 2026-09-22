#pragma once

namespace nso {

struct RpcSettings {
    bool show_username = true;
    bool show_profile_picture = true;
    bool show_play_time = true;
    bool zelda = true;
    bool animal_crossing = true;
    bool splatoon3 = true;
    bool splatoon2 = true;

    bool operator==(const RpcSettings&) const = default;
};

}  // namespace nso
