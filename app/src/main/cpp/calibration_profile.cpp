#include "calibration_profile.h"

#include <mutex>

namespace calibration_profile {
namespace {
std::mutex gMutex;
Profile gProfile;
}

void configure(const Profile& profile) {
    std::lock_guard<std::mutex> lock(gMutex);
    gProfile = profile;
}

void clear() {
    std::lock_guard<std::mutex> lock(gMutex);
    gProfile = {};
}

Profile snapshot() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gProfile;
}

}  // namespace calibration_profile
