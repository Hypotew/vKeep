#include "camera.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr std::string_view NODE_PREFIX = "video";
constexpr const char* LOOPBACK_DRIVER = "v4l2 loopback"; // caméra virtuelle, sans image de la pièce

// Une webcam expose aussi des nœuds de métadonnées, qui ne donnent aucune image.
bool is_real_camera(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    v4l2_capability capability{};
    const bool answered = ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0;
    close(fd);
    if (!answered)
        return false;

    const __u32 caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? capability.device_caps
                                                                            : capability.capabilities;
    const auto* driver = reinterpret_cast<const char*>(capability.driver);
    return (caps & V4L2_CAP_VIDEO_CAPTURE) != 0 && std::strcmp(driver, LOOPBACK_DRIVER) != 0;
}

} // namespace

std::string find_camera() {
    std::vector<int> numbers;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/video4linux", error)) {
        const std::string name = entry.path().filename().string();
        int number = 0;
        const char* digits = name.data() + NODE_PREFIX.size();
        if (name.starts_with(NODE_PREFIX) &&
            std::from_chars(digits, name.data() + name.size(), number).ec == std::errc{})
            numbers.push_back(number);
    }
    std::sort(numbers.begin(), numbers.end()); // numérique : video2 passe avant video10

    for (const int number : numbers) {
        const std::string path = "/dev/video" + std::to_string(number);
        if (is_real_camera(path))
            return path;
    }
    return "";
}
