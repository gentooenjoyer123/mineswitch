#pragma once
#include "http.hpp"
#include "storage.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace mineswitch {
struct LaunchPlan {
    bool preparation_only = false;
    std::string main_class; // Dotted Java name; runtime integration may convert to JNI slashes.
    std::vector<std::string> jvm_options, game_arguments;
    std::string working_directory;
    std::string preparation_summary; // One preparation-statistics line; no game/FPS metrics.
};

// User JVM options: -D, -XX: and -Xss only, whitespace separated. Memory size,
// agents and module boundaries stay under launcher control. Throws on rejection.
std::vector<std::string> parseJvmArguments(const std::string& text);

// Preparation runs off the render thread. It verifies/reuses shared artifacts,
// obtains missing official metadata/files, and publishes launch-plan.json before
// marking the instance Ready. It never starts a JVM or runs installer scripts.
class GamePreparer {
public:
    GamePreparer(Storage&, HttpClient&);
    LaunchPlan prepare(Instance&, const Settings&, const Profile&,
                       ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr,
                       bool force_validation = false, bool reuse_ready_plan = false);
private:
    Storage& storage_;
    HttpClient& http_;
};
}
