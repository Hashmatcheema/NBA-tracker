#pragma once

namespace j2k::plugins {

class IReleaseModelPlugin {
public:
    virtual ~IReleaseModelPlugin() = default;
    virtual float predict_release_frames() const = 0;
};

}  // namespace j2k::plugins
