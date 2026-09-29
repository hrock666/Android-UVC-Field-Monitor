#pragma once

#include <array>

namespace calibration_profile {

struct Profile {
    bool enabled = false;
    std::array<float, 9> matrix{{1,0,0, 0,1,0, 0,0,1}};
    std::array<float, 3> offsetCode{{0,0,0}};
    std::array<int, 4> pu{{0,0,0,0}};
    bool pqInput = false;
    int colorimetry = 1;  // 0=BT.601, 1=BT.709, 2=BT.2020
};

void configure(const Profile& profile);
void clear();
Profile snapshot();

}  // namespace calibration_profile
