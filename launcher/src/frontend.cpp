#include "mineswitch/frontend.h"
#include "mineswitch/catalog.hpp"
#include "mineswitch/content.hpp"
#include "mineswitch/drawing.hpp"
#include "mineswitch/heap.hpp"
#include "mineswitch/i18n.hpp"
#include "mineswitch/images.hpp"
#include "mineswitch/import.hpp"
#include "mineswitch/mods.hpp"
#include "mineswitch/packwiz.hpp"
#include "mineswitch/prepare.hpp"
#include "mineswitch/repair.hpp"
#include "mineswitch/versions.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
extern "C" bool mineswitch_prepare_runtime(void);
#endif

namespace mineswitch {
namespace {
std::string sizeText(std::uint64_t bytes) {
    char value[40];
    if (bytes >= (1ULL << 30))
        std::snprintf(value, sizeof(value), "%.1f", bytes / double(1ULL << 30));
    else if (bytes >= (1ULL << 20))
        std::snprintf(value, sizeof(value), "%.0f", bytes / double(1ULL << 20));
    else
        std::snprintf(value, sizeof(value), "%.0f", bytes / 1024.0);
    return trf(bytes >= (1ULL << 30) ? "{0} GB" : bytes >= (1ULL << 20) ? "{0} MB" : "{0} KB", {value});
}
std::string mibText(int mib) { return trf("{0} MB", {std::to_string(mib)}); }
// Download counts: 1.2M, 3.4K.
std::string amountText(std::uint64_t n) {
    char value[32];
    if (n >= 1000000)
        std::snprintf(value, sizeof(value), "%.1f", n / 1e6);
    else if (n >= 1000)
        std::snprintf(value, sizeof(value), "%.1f", n / 1e3);
    else
        std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(n));
    return n >= 1000000 ? trf("{0}M", {value}) : n >= 1000 ? trf("{0}K", {value}) : std::string(value);
}
std::string dateText(std::int64_t seconds) {
    std::time_t when = static_cast<std::time_t>(seconds);
    char value[32] = "";
    if (auto *local = std::localtime(&when))
        std::strftime(value, sizeof(value), "%d.%m.%Y", local);
    return value;
}
std::string agoText(std::int64_t seconds) {
    if (seconds <= 0)
        return tr("never");
    auto now = static_cast<std::int64_t>(std::time(nullptr));
    auto delta = now - seconds;
    if (delta < 0)
        return dateText(seconds);
    if (delta < 90)
        return tr("just now");
    if (delta < 3600)
        return trf("{0} ago", {countText(delta / 60, "minute", "minutes", "минуту", "минуты", "минут")});
    if (delta < 86400)
        return trf("{0} ago", {countText(delta / 3600, "hour", "hours", "час", "часа", "часов")});
    if (delta < 2 * 86400)
        return tr("yesterday");
    if (delta < 30 * 86400)
        return trf("{0} ago", {countText(delta / 86400, "day", "days", "день", "дня", "дней")});
    return dateText(seconds);
}
std::string loaderText(LoaderKind kind) {
    switch (kind) {
    case LoaderKind::Vanilla: return "Vanilla";
    case LoaderKind::Fabric: return "Fabric";
    case LoaderKind::Quilt: return "Quilt";
    case LoaderKind::Forge: return "Forge";
    case LoaderKind::NeoForge: return "NeoForge";
    default: return tr("Unknown loader");
    }
}
std::string gameText(const GameSpec &game) {
    auto text = "Minecraft " + game.minecraft_version + " · " + loaderText(game.loader.kind);
    if (game.loader.kind != LoaderKind::Vanilla && !game.loader.version.empty())
        text += " " + game.loader.version;
    return text;
}
std::string stateText(InstanceState state) {
    switch (state) {
    case InstanceState::Ready: return tr("Ready");
    case InstanceState::NeedsFiles: return tr("Needs files");
    case InstanceState::NeedsPreparation: return tr("Needs preparation");
    case InstanceState::Unsupported: return tr("Incompatible");
    default: return tr("Broken");
    }
}
Color stateColor(InstanceState state) {
    return state == InstanceState::Ready              ? accent
           : state == InstanceState::NeedsPreparation ? info
           : state == InstanceState::NeedsFiles       ? warning
                                                      : danger;
}
std::string modeText(int mode, bool hardcore) {
    if (hardcore)
        return tr("Hardcore");
    switch (mode) {
    case 1: return tr("Creative");
    case 2: return tr("Adventure");
    case 3: return tr("Spectator");
    default: return tr("Survival");
    }
}
std::string kindTitle(ContentKind kind) {
    switch (kind) {
    case ContentKind::Mod: return tr("Mods");
    case ContentKind::ResourcePack: return tr("Resource packs");
    case ContentKind::ShaderPack: return tr("Shaders");
    default: return tr("Modpacks");
    }
}
std::string errorText(const std::string &code, const std::string &detail) {
    if (code == "catalog.cf_key_missing" || code == "catalog.cf_key_unavailable")
        return tr("CurseForge is unavailable right now. You can set your own API key in Settings.");
    if (code == "http_tls")
        return tr("Could not establish a secure connection. Check the Switch date, time and network. Restart the launcher after changing the clock.");
    if (code == "cancelled" || code == "catalog.cancelled")
        return tr("Operation cancelled. Installed instances are kept.");
    if (code == "disk_space")
        return tr("Not enough free space on the SD card.");
    if (code == "catalog.rate_limited")
        return tr("The catalog is limiting requests right now. Try again later.");
    if (code.compare(0, 4, "http") == 0)
        return trf("No connection to the server. Check your internet connection. [{0}]", {code});
    // Backend messages are English; tr() finds the exact ones.
    return tr(detail) + (code.empty() ? "" : " [" + code + "]");
}
bool usernameValid(const std::string &name) {
    return !name.empty() && name.size() <= 16 &&
           name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}
// Quick play exists since 1.20 (23w14a); older clients take --server/--port.
bool quickPlaySupported(const std::string &version) {
    if (version.compare(0, 2, "1.") == 0)
        return std::atoi(version.c_str() + 2) >= 20;
    if (version.size() > 2 && version[2] == 'w')
        return std::atoi(version.c_str()) >= 23;
    return std::atoi(version.c_str()) >= 23;
}
const Color icon_colors[] = {accent, info, {199, 168, 228}, {237, 191, 121}, {240, 128, 128}, {120, 214, 214}, {226, 226, 140}, {180, 180, 200}};
const char *icon_names[] = {"Mint", "Blue", "Lilac", "Amber", "Coral", "Turquoise", "Lemon", "Gray"};
// Log names from listLogs() carry an English prefix for crash and preparation logs.
std::string logName(const std::string &name) {
    for (std::string prefix : {"Crash · ", "Preparation · "})
        if (name.compare(0, prefix.size(), prefix) == 0)
            return tr(prefix) + name.substr(prefix.size());
    return name;
}

// Background work returns a closure that applies its result on the UI thread.
using Apply = std::function<void()>;
using Work = std::function<Apply(ProgressCallback, const std::atomic_bool *)>;
class Job {
#ifdef __SWITCH__
    Thread worker_{};
    bool joinable_ = false;
    void join() {
        if (joinable_) {
            threadWaitForExit(&worker_);
            threadClose(&worker_);
            joinable_ = false;
        }
    }
#else
    std::thread worker_;
    void join() {
        if (worker_.joinable())
            worker_.join();
    }
#endif
    mutable std::mutex mutex_;
    Progress progress_;
    Apply result_;
    std::string error_, error_code_;
    std::atomic_bool done_{false};

  public:
    std::atomic_bool cancelled{false};
    bool active = false;
    std::string title;
    ~Job() {
        cancelled = true;
        join();
    }
    void start(const std::string &label, Work work) {
        if (active)
            throw Error("job_active", "Wait for the current task to finish");
        join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_ = {};
            result_ = {};
            error_.clear();
            error_code_.clear();
        }
        title = label;
        cancelled = false;
        done_ = false;
        active = true;
        try {
            auto execute = [this, work = std::move(work)] {
                try {
                    auto result = work(
                        [this](const Progress &p) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            progress_ = p;
                        },
                        &cancelled);
                    std::lock_guard<std::mutex> lock(mutex_);
                    result_ = std::move(result);
                } catch (const Error &e) {
                    // The toast stays short; the log keeps the full cause.
                    std::fprintf(stderr, "MINESWITCH_JOB_ERROR %s [%s] %s\n", title.c_str(), e.code.c_str(), e.what());
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = errorText(e.code, e.what());
                    error_code_ = e.code;
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = tr(e.what());
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = tr("Could not finish the task");
                }
                done_.store(true, std::memory_order_release);
            };
#ifdef __SWITCH__
            auto task = std::make_unique<std::function<void()>>(std::move(execute));
            auto entry = [](void *value) {
                std::unique_ptr<std::function<void()>> run(static_cast<std::function<void()> *>(value));
                (*run)();
            };
            Result result = threadCreate(&worker_, entry, task.get(), nullptr, 1024 * 1024, 0x3B, -2);
            if (R_FAILED(result))
                throw Error("worker", "Could not create a background task");
            result = threadStart(&worker_);
            if (R_FAILED(result)) {
                threadClose(&worker_);
                throw Error("worker", "Could not start a background task");
            }
            task.release();
            joinable_ = true;
#else
            worker_ = std::thread(std::move(execute));
#endif
        } catch (...) {
            active = false;
            throw;
        }
    }
    Progress progress() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return progress_;
    }
    bool take(Apply &result, std::string &error, std::string &code) {
        if (!active || !done_.load(std::memory_order_acquire))
            return false;
        join();
        active = false;
        std::lock_guard<std::mutex> lock(mutex_);
        result = std::move(result_);
        error = error_;
        code = error_code_;
        return true;
    }
};

enum class Page { Library, Catalog, Files, Tasks, Settings };
enum class Tab { Overview, Mods, ResourcePacks, ShaderPacks, Worlds, Servers, Screenshots, Logs, Settings };
constexpr int tab_count = 9;
const char *tab_names[tab_count] = {"Overview", "Mods", "Resource packs", "Shaders", "Worlds", "Servers", "Screenshots", "Logs", "Settings"};
ContentKind tabKind(Tab tab) {
    return tab == Tab::ResourcePacks ? ContentKind::ResourcePack : tab == Tab::ShaderPacks ? ContentKind::ShaderPack : ContentKind::Mod;
}

struct Action {
    SDL_Rect rect;
    std::function<void()> call;
    bool enabled;
    std::string label;
    SDL_Rect clip{0, 0, 1280, 720};
    int group = -1;
};
struct MenuItem {
    std::string label;
    std::function<void()> call;
    bool danger = false, enabled = true;
};
struct Modal {
    std::string title, text;
    std::vector<MenuItem> items;
};
struct Toast {
    std::string text;
    Color color;
    Uint32 until;
};
struct HistoryEntry {
    std::string title, result;
    bool ok;
    std::int64_t when;
};
struct Scroll {
    float current = 0, target = 0;
};
struct ListArea {
    std::string id;
    SDL_Rect area;
    int max_scroll;
};
struct FileEntry {
    std::string name, path;
    bool directory;
};
// Catalog browsing state. Modpacks have their own page; mods, resource packs
// and shaders are browsed from inside an instance.
struct Browse {
    ContentKind kind = ContentKind::Modpack;
    Provider provider = Provider::Modrinth;
    SearchSort sort = SearchSort::Relevance;
    std::string query, minecraft = "1.21.1";
    std::vector<CatalogProject> results;
    std::uint32_t next_offset = 0;
    bool has_more = false, searched = false, loading = false;
    std::optional<CatalogProject> project;
    std::vector<CatalogVersion> versions;
    bool versions_loading = false;
    std::uint64_t epoch = 0;
};
// New instance / change version wizard.
struct Wizard {
    bool editing = false; // true: change an existing instance's game version.
    Instance target;
    std::string name;
    int filter = 0; // 0 releases, 1 snapshots, 2 all
    std::vector<GameVersion> versions;
    bool versions_loading = false;
    std::string versions_error;
    std::string minecraft;
    LoaderKind loader = LoaderKind::Vanilla;
    std::vector<LoaderVersion> loader_versions;
    bool loader_loading = false;
    std::string loader_error, loader_version;
    std::uint64_t epoch = 0;
};
struct LogView {
    std::string title;
    std::vector<std::string> lines;
    int top = 0, shift = 0;
};

class Application {
    Storage storage_;
    Settings settings_;
    HeapBudget heap_budget_ = currentHeapBudget();
    std::vector<Profile> profiles_;
    std::vector<Instance> instances_;
    CurlHttpClient http_;
    Drawing draw_;
    ImageCache images_;
    Job task_, query_;
    std::optional<std::pair<std::string, Work>> queued_query_;
    Page page_ = Page::Library;
    std::vector<Action> actions_;
    int focus_ = -1, content_start_ = 0, group_counter_ = 0;
    std::map<std::string, Scroll> scroll_;
    std::vector<ListArea> lists_;
    std::vector<std::pair<std::string, std::string>> hints_;
    std::deque<Toast> toasts_;
    std::deque<HistoryEntry> history_;
    std::optional<Modal> modal_;
    int modal_saved_focus_ = 0;
    // Library / instance
    std::optional<Instance> detail_;
    Tab tab_ = Tab::Overview;
    bool tab_loading_ = false;
    std::uint64_t tab_epoch_ = 0;
    std::vector<InstalledMod> mods_;
    std::vector<PackEntry> packs_;
    std::vector<WorldInfo> worlds_;
    std::vector<ServerEntry> servers_;
    std::vector<MediaFile> shots_, logs_;
    std::map<std::string, CatalogVersion> updates_;
    bool updates_checked_ = false;
    std::optional<int> viewer_;
    std::optional<LogView> log_;
    std::optional<Wizard> wizard_;
    // Catalogs
    Browse packs_browse_, content_browse_;
    bool browsing_content_ = false;
    std::optional<PackPlan> inspected_;
    std::string archive_, inspected_icon_;
    // SD import
    std::string directory_, browser_root_;
    std::vector<FileEntry> files_;
    bool include_optional_ = false, quit_ = false, preview_ = false, exit_requested_ = false;
    std::optional<LaunchPlan> pending_launch_;
    std::optional<LaunchPlan> processor_launch_;
    std::string dialog_, dialog_title_;
    bool editing_ = false, secret_ = false;
    std::size_t edit_limit_ = 256;
    std::function<void(std::string)> edit_done_;
    std::vector<SDL_GameController *> controllers_;
    Uint32 next_axis_ = 0;
    std::uint64_t free_bytes_ = 0;
    SDL_Point touch_start_{0, 0};
    bool touch_dragged_ = false;
    std::string touch_list_;

    // ---------------------------------------------------------------- state
    Profile profile() const {
        auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const Profile &p) { return p.id == settings_.profile_id; });
        return it == profiles_.end() ? profiles_.front() : *it;
    }
    void sortInstances() {
        auto key = [](std::string s) {
            for (auto &c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        };
        std::stable_sort(instances_.begin(), instances_.end(), [&](const Instance &a, const Instance &b) {
            if (settings_.library_sort == 1 && a.last_played != b.last_played)
                return a.last_played > b.last_played;
            if (settings_.library_sort == 2 && a.game.minecraft_version != b.game.minecraft_version)
                return a.game.minecraft_version > b.game.minecraft_version;
            return key(a.name) < key(b.name);
        });
    }
    void refresh() {
        if (preview_)
            return;
        instances_ = storage_.listInstances();
        sortInstances();
        free_bytes_ = storage_.availableBytes();
        if (detail_) {
            auto it = std::find_if(instances_.begin(), instances_.end(), [&](const Instance &i) { return i.id == detail_->id; });
            if (it != instances_.end())
                detail_ = *it;
            else
                closeDetail();
        }
    }
    void toast(const std::string &text, Color color = accent) {
        toasts_.push_back({text, color, SDL_GetTicks() + 4500});
        while (toasts_.size() > 2)
            toasts_.pop_front();
    }
    void remember(const std::string &title, const std::string &result, bool ok) {
        history_.push_front({title, result, ok, static_cast<std::int64_t>(std::time(nullptr))});
        while (history_.size() > 30)
            history_.pop_back();
    }
    void edit(const std::string &title, const std::string &value, std::size_t maximum, std::function<void(std::string)> done,
              bool secret = false) {
#ifdef __SWITCH__
        SwkbdConfig keyboard{};
        if (R_FAILED(swkbdCreate(&keyboard, 0)))
            throw Error("keyboard", "Could not open the keyboard");
        if (secret)
            swkbdConfigMakePresetPassword(&keyboard);
        else
            swkbdConfigMakePresetDefault(&keyboard);
        swkbdConfigSetHeaderText(&keyboard, title.c_str());
        swkbdConfigSetInitialText(&keyboard, value.c_str());
        swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(maximum));
        std::vector<char> output(maximum * 4 + 1);
        Result result = swkbdShow(&keyboard, output.data(), output.size());
        swkbdClose(&keyboard);
        if (R_SUCCEEDED(result))
            done(output.data());
#else
        dialog_title_ = title;
        dialog_ = value;
        edit_limit_ = maximum;
        secret_ = secret;
        edit_done_ = std::move(done);
        editing_ = true;
        SDL_StartTextInput();
#endif
    }
    void acceptEdit() {
        editing_ = false;
        SDL_StopTextInput();
        // Reset before the callback: it may open the next keyboard prompt.
        auto callback = std::move(edit_done_);
        edit_done_ = {};
        auto value = std::move(dialog_);
        dialog_.clear();
        if (callback)
            callback(value);
    }
    void openMenu(const std::string &title, const std::string &text, std::vector<MenuItem> items) {
        if (!modal_)
            modal_saved_focus_ = focus_;
        modal_ = Modal{title, text, std::move(items)};
        focus_ = 0;
    }
    void closeMenu() {
        modal_.reset();
        focus_ = modal_saved_focus_;
    }
    void confirm(const std::string &title, const std::string &text, const std::string &yes, std::function<void()> call) {
        openMenu(title, text, {{yes, std::move(call), true}, {tr("Cancel"), [] {}}});
    }
    // Heavy work: installs, preparation, copies. Visible on the Tasks page.
    bool task(const std::string &label, Work work) {
        if (preview_) {
            toast(tr("This is a UI preview. Run without --preview to work with files."), warning);
            return false;
        }
        if (task_.active) {
            toast(trf("Wait for \"{0}\" to finish", {task_.title}), warning);
            return false;
        }
        // A completed preparation and a queued user action can share a frame.
        // Accepting new work supersedes the launch that has not handed off yet.
        pending_launch_.reset();
        processor_launch_.reset();
        task_.start(label, std::move(work));
        return true;
    }
    // Light work: searches and lists. A newer query replaces a pending one.
    void query(const std::string &label, Work work) {
        if (preview_)
            return;
        if (query_.active) {
            query_.cancelled = true;
            queued_query_ = std::make_pair(label, std::move(work));
            return;
        }
        query_.start(label, std::move(work));
    }

    // -------------------------------------------------------------- catalog
    Browse &browse() { return detail_ && browsing_content_ ? content_browse_ : packs_browse_; }
    void search(bool more = false) {
        auto &b = browse();
        auto settings = settings_;
        SearchQuery q{b.query, b.minecraft, more ? b.next_offset : 0, 20, b.kind, LoaderKind::Unknown, b.sort};
        if (b.kind != ContentKind::Modpack && detail_) {
            q.minecraft_version = detail_->game.minecraft_version;
            q.loader = detail_->game.loader.kind;
        }
        auto provider = b.provider;
        auto epoch = ++b.epoch;
        auto *target = &b;
        if (!more) {
            b.results.clear();
            b.has_more = false;
        }
        b.loading = true;
        b.searched = true;
        query(q.kind == ContentKind::Modpack ? "Modpack search" : "Search: " + kindTitle(q.kind),
              [this, settings, provider, q, epoch, target, more](ProgressCallback, const std::atomic_bool *stop) -> Apply {
                  CatalogClient catalog(http_, settings);
                  auto page = catalog.search(provider, q, stop);
                  return [this, target, page, epoch, more] {
                      if (target->epoch != epoch)
                          return;
                      if (!more) {
                          target->results.clear();
                          focus_ = -1;
                      }
                      target->results.insert(target->results.end(), page.projects.begin(), page.projects.end());
                      target->next_offset = page.offset + static_cast<std::uint32_t>(page.projects.size());
                      target->has_more = page.has_more;
                      target->loading = false;
                  };
              });
    }
    void openProject(const CatalogProject &project) {
        auto &b = browse();
        auto settings = settings_;
        b.project = project;
        b.versions.clear();
        b.versions_loading = true;
        auto epoch = ++b.epoch;
        auto *target = &b;
        std::string filter = b.kind == ContentKind::Modpack ? b.minecraft : detail_ ? detail_->game.minecraft_version : "";
        auto loader = b.kind == ContentKind::Mod && detail_ ? detail_->game.loader.kind : LoaderKind::Unknown;
        auto kind = b.kind;
        focus_ = -1;
        query("Versions: " + project.name,
              [this, settings, project, filter, kind, loader, epoch, target](ProgressCallback, const std::atomic_bool *stop) -> Apply {
                  CatalogClient catalog(http_, settings);
                  auto list = catalog.versions(project.provider, project.id, filter, stop, kind, loader);
                  return [this, target, list, epoch] {
                      if (target->epoch != epoch)
                          return;
                      target->versions = list;
                      target->versions_loading = false;
                      focus_ = -1;
                  };
              });
    }
    void installContent(const CatalogVersion &version) {
        if (!detail_)
            return;
        auto item = *detail_;
        auto settings = settings_;
        auto kind = content_browse_.kind;
        auto name = content_browse_.project ? content_browse_.project->name : version.name;
        task(trf("Installing: {0}", {name}), [this, item, settings, version, kind, name](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
            CatalogClient catalog(http_, settings);
            auto exact = catalog.version(version.provider, version.project_id, version.id, stop, kind);
            std::vector<std::string> installed;
            if (kind == ContentKind::Mod) {
                ModManager manager(storage_, http_);
                installed = manager.installWithDependencies(item, catalog, exact, progress, stop);
            } else {
                installPack(storage_, http_, item, kind, exact, progress, stop);
                installed.push_back(exact.name);
            }
            auto copy = item;
            copy.installed_bytes += exact.archive.bytes;
            storage_.saveInstance(copy);
            return [this, name, installed, id = item.id] {
                auto text = installed.size() > 1
                                ? trf("Installed: {0} and {1}", {name, countText(static_cast<long>(installed.size() - 1), "dependency", "dependencies",
                                                                                 "зависимость", "зависимости", "зависимостей")})
                                : trf("Installed: {0}", {name});
                toast(text);
                remember(trf("Installing: {0}", {name}), text, true);
                refresh();
                if (detail_ && detail_->id == id)
                    loadTab();
            };
        });
    }
    void inspect(const std::string &path) {
        task(tr("Checking archive"), [this, path](ProgressCallback, const std::atomic_bool *stop) -> Apply {
            ArchiveImporter importer(storage_, http_, parse_pack_manifest);
            auto plan = importer.inspectArchive(path, stop);
            return [this, plan, path] {
                inspected_ = plan;
                archive_ = path;
                inspected_icon_.clear();
                focus_ = -1;
            };
        });
    }
    void downloadPack(const CatalogVersion &version) {
        auto settings = settings_;
        auto icon = packs_browse_.project ? packs_browse_.project->icon_url : std::string();
        task(tr("Downloading modpack"), [this, settings, version, icon](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
            CatalogClient catalog(http_, settings);
            auto exact = catalog.version(version.provider, version.project_id, version.id, stop);
            auto folder = joinPath(storage_.root(), "cache/pack-archives");
            createDirectories(folder);
            std::string path = joinPath(folder, newId() + (version.provider == Provider::Modrinth ? ".mrpack" : ".zip"));
            Download bounded = exact.archive;
            auto available = storage_.availableBytes();
            if (available <= 32 * 1024 * 1024)
                throw Error("disk_space", "Insufficient free space");
            bounded.max_bytes = std::min(bounded.max_bytes, available - 32 * 1024 * 1024);
            http_.download(bounded, path, progress, stop);
            ArchiveImporter importer(storage_, http_, parse_pack_manifest);
            auto plan = importer.inspectArchive(path, stop);
            return [this, plan, path, icon] {
                inspected_ = plan;
                archive_ = path;
                inspected_icon_ = icon;
                focus_ = -1;
            };
        });
    }
    void import() {
        auto settings = settings_;
        auto path = archive_;
        auto icon = inspected_icon_;
        bool optional = include_optional_;
        if (task(tr("Installing modpack"),
                 [this, settings, path, optional, icon](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                     CatalogClient catalog(http_, settings);
                     ArchiveImporter importer(storage_, http_, parse_pack_manifest,
                                              [&catalog](PackPlan &p, const std::atomic_bool *c) { catalog.resolve_pack(p, c); });
                     auto instance = importer.importArchive(path, {"", optional, true}, progress, stop);
                     if (!icon.empty()) {
                         // The pack icon is decorative; failures leave the generated icon.
                         try {
                             HttpRequest request;
                             request.url = icon;
                             request.max_bytes = 2 * 1024 * 1024;
                             auto response = http_.request(request, stop);
                             if (response.status == 200 && !response.body.empty()) {
                                 atomicWriteText(joinPath(instance.root, "icon.png"), response.body);
                                 instance.icon = "file";
                                 storage_.saveInstance(instance);
                             }
                         } catch (const Error &) {
                         }
                     }
                     std::string packwiz_error;
                     if (!instance.packwiz_url.empty()) {
                         // A failed first sync is retried by "Play", which needs it to succeed.
                         try {
                             syncPackwiz(storage_, http_, settings, instance, true, progress, stop);
                         } catch (const Error &e) {
                             if (e.code == "cancelled")
                                 throw;
                             packwiz_error = e.what();
                             std::fprintf(stderr, "MINESWITCH_PACKWIZ_ERROR [%s] %s\n", e.code.c_str(), e.what());
                         }
                     }
                     return [this, instance, packwiz_error] {
                         refresh();
                         openInstance(instance);
                         if (packwiz_error.empty())
                             toast(trf("Installed \"{0}\"", {instance.name}));
                         else
                             toast(trf("Instance created, but packwiz mods were not downloaded: {0}", {tr(packwiz_error)}), danger);
                         remember(tr("Installing modpack"), instance.name, packwiz_error.empty());
                     };
                 })) {
            inspected_.reset();
            page_ = Page::Tasks;
            focus_ = -1;
        }
    }

    // ------------------------------------------------------------ instances
    void openInstance(const Instance &instance) {
        detail_ = instance;
        tab_ = Tab::Overview;
        browsing_content_ = false;
        content_browse_ = Browse{};
        page_ = Page::Library;
        focus_ = -1;
        for (auto it = scroll_.begin(); it != scroll_.end();)
            it = it->first.compare(0, 4, "tab:") == 0 ? scroll_.erase(it) : std::next(it);
        loadTab();
    }
    void closeDetail() {
        detail_.reset();
        browsing_content_ = false;
        viewer_.reset();
        log_.reset();
        focus_ = -1;
    }
    void selectTab(Tab tab) {
        if (!detail_)
            return;
        tab_ = tab;
        browsing_content_ = false;
        focus_ = -1;
        loadTab();
    }
    // Reloads the data shown on the current instance tab.
    void loadTab() {
        if (!detail_ || preview_)
            return;
        auto item = *detail_;
        auto tab = tab_;
        auto epoch = ++tab_epoch_;
        updates_.clear();
        updates_checked_ = false;
        if (tab == Tab::Overview || tab == Tab::Settings) {
            tab_loading_ = false;
            return;
        }
        tab_loading_ = true;
        query("Reading instance", [this, item, tab, epoch](ProgressCallback, const std::atomic_bool *) -> Apply {
            std::vector<InstalledMod> mods;
            std::vector<PackEntry> packs;
            std::vector<WorldInfo> worlds;
            std::vector<ServerEntry> servers;
            std::vector<MediaFile> media;
            switch (tab) {
            case Tab::Mods: mods = ModManager(storage_, http_).list(item); break;
            case Tab::ResourcePacks:
            case Tab::ShaderPacks: packs = listPacks(storage_, item, tabKind(tab)); break;
            case Tab::Worlds: worlds = listWorlds(storage_, item); break;
            case Tab::Servers: servers = listServers(storage_, item); break;
            case Tab::Screenshots: media = listScreenshots(storage_, item); break;
            case Tab::Logs: media = listLogs(storage_, item); break;
            default: break;
            }
            return [this, epoch, tab, mods, packs, worlds, servers, media] {
                if (epoch != tab_epoch_)
                    return;
                tab_loading_ = false;
                mods_ = mods;
                packs_ = packs;
                worlds_ = worlds;
                servers_ = servers;
                if (tab == Tab::Screenshots)
                    shots_ = media;
                if (tab == Tab::Logs)
                    logs_ = media;
            };
        });
    }
    // Runs a quick local mutation, then reloads the tab.
    void local(const std::string &label, std::function<void()> work, const std::string &done = {}) {
        if (preview_)
            return;
        auto item = detail_;
        query(label, [this, work, done, label](ProgressCallback, const std::atomic_bool *) -> Apply {
            work();
            return [this, done] {
                if (!done.empty())
                    toast(done);
                refresh();
                loadTab();
            };
        });
    }
    void saveDetail(const Instance &instance) {
        storage_.saveInstance(instance);
        detail_ = instance;
        refresh();
    }
    int effectiveHeap(const Instance &instance) const { return instance.heap_mib ? instance.heap_mib : settings_.heap_mib; }
    void prepare(Instance instance, bool force_validation = false, std::vector<std::string> extra = {}) {
#ifdef __SWITCH__
        if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
            throw Error("applet_memory", "To run Minecraft, open the launcher through a game while holding R.");
        if (R_SUCCEEDED(timeInitialize())) {
            u64 system_seconds = 0;
            Result clock_result = timeGetCurrentTime(TimeType_UserSystemClock, &system_seconds);
            timeExit();
            auto process_seconds = std::time(nullptr);
            if (R_SUCCEEDED(clock_result) && process_seconds >= 0 &&
                std::llabs(static_cast<long long>(system_seconds) - static_cast<long long>(process_seconds)) > 120)
                throw Error("clock_restart", "The console time changed after launch. Close MineSwitch and open it again before starting the game.");
        }
#endif
        auto settings = settings_;
        settings.heap_mib = effectiveHeap(instance);
        if (settings.heap_mib > heap_budget_.max_mib)
            settings.heap_mib = heap_budget_.max_mib;
        settings.jvm_args = settings_.jvm_args + " " + instance.jvm_args;
        parseJvmArguments(settings.jvm_args);
        auto user = profile();
        bool started = task(force_validation ? trf("Full check: {0}", {instance.name}) : trf("Preparing: {0}", {instance.name}),
                            [this, settings, user, instance, force_validation, extra](ProgressCallback progress,
                                                                                      const std::atomic_bool *stop) mutable -> Apply {
                                const auto started = SDL_GetTicks();
#ifdef __SWITCH__
                                progress({"runtime", "Checking Java 21 on the SD card", 0, 0, 0, 0});
                                if (!mineswitch_prepare_runtime())
                                    throw Error("runtime_install", "Could not install Java 21 on the SD card");
                                if (stop && stop->load())
                                    throw Error("cancelled", "Operation cancelled");
#endif
                                const auto runtime_ms = SDL_GetTicks() - started;
                                if (!instance.packwiz_url.empty()) {
                                    // Like Prism's packwiz-installer pre-launch step; offline play
                                    // keeps the last synced files.
                                    try {
                                        auto synced = syncPackwiz(storage_, http_, settings, instance, false, progress, stop);
                                        if (synced.changed)
                                            std::fprintf(stderr, "MINESWITCH_PACKWIZ version=%s downloaded=%u removed=%u kept=%u skipped=%u\n",
                                                         synced.version.c_str(), synced.downloaded, synced.removed, synced.kept, synced.skipped);
                                    } catch (const Error &e) {
                                        if (e.code == "cancelled" || !packwizSynced(instance))
                                            throw;
                                        std::fprintf(stderr, "MINESWITCH_PACKWIZ_ERROR [%s] %s\n", e.code.c_str(), e.what());
                                    }
                                }
                                GamePreparer preparer(storage_, http_);
                                auto launch = preparer.prepare(instance, settings, user, progress, stop, force_validation, true);
                                launch.preparation_summary += " runtime_ms=" + std::to_string(runtime_ms) +
                                                              " total_ms=" + std::to_string(SDL_GetTicks() - started);
#ifdef __SWITCH__
                                if (force_validation)
                                    mineswitch_frontend_diagnostic(launch.preparation_summary.c_str());
#endif
                                if (!launch.preparation_only && !force_validation)
                                    launch.game_arguments.insert(launch.game_arguments.end(), extra.begin(), extra.end());
                                return [this, launch, instance, force_validation]() mutable { prepared(instance, std::move(launch), force_validation); };
                            });
        if (started) {
            page_ = Page::Tasks;
            focus_ = -1;
        }
    }
    void prepared(Instance instance, LaunchPlan launch, bool force_validation) {
        refresh();
        if (force_validation) {
            toast(tr(launch.preparation_only ? "Resources verified. Choose \"Play\" to set up the loader." : "Minecraft resources verified."));
            remember(tr("Full check"), instance.name, true);
            if (page_ == Page::Tasks)
                page_ = Page::Library;
            return;
        }
        if (launch.preparation_only) {
            processor_launch_ = std::move(launch);
            closeDetail();
            wizard_.reset();
            inspected_.reset();
            page_ = Page::Tasks;
            focus_ = -1;
            return;
        }
#ifdef __SWITCH__
        for (auto &item : instances_)
            if (item.id == instance.id) {
                item.last_played = static_cast<std::int64_t>(std::time(nullptr));
                try {
                    storage_.saveInstance(item);
                } catch (const Error &) {
                }
            }
        pending_launch_ = std::move(launch);
#else
        (void)instance;
        toast(tr("Preparation finished. The game can only be launched in the Switch build."), info);
        remember(tr("Preparation"), instance.name, true);
        page_ = Page::Library;
#endif
    }
    void play(const Instance &instance, std::vector<std::string> extra = {}) {
        if (instance.state == InstanceState::NeedsFiles)
            return verifyFiles(instance);
        if (instance.state == InstanceState::Unsupported || instance.state == InstanceState::Broken)
            return toast(tr(instance.reason.empty() ? "This instance is incompatible" : instance.reason), danger);
        prepare(instance, false, std::move(extra));
    }
    void syncPack(Instance instance) {
        auto settings = settings_;
        if (task("packwiz: " + instance.name, [this, instance, settings](ProgressCallback progress, const std::atomic_bool *stop) mutable -> Apply {
                auto result = syncPackwiz(storage_, http_, settings, instance, true, progress, stop);
                return [this, instance, result] {
                    refresh();
                    if (detail_ && detail_->id == instance.id)
                        detail_ = instance;
                    auto text = trf("Version {0}: {1} downloaded, {2} removed, {3} unchanged",
                                    {result.version.empty() ? std::string("—") : result.version, std::to_string(result.downloaded),
                                     std::to_string(result.removed), std::to_string(result.kept)});
                    toast(text);
                    remember("packwiz: " + instance.name, text, true);
                };
            })) {
            page_ = Page::Tasks;
            focus_ = -1;
        }
    }
    void verifyFiles(Instance instance) {
        if (task(trf("Checking files: {0}", {instance.name}), [this, instance](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                auto result = verifyManualFiles(storage_, instance, progress, stop);
                return [this, result] {
                    refresh();
                    toast(tr(result.state == InstanceState::NeedsFiles ? "Some files are still missing" : "Instance files verified"),
                          result.state == InstanceState::NeedsFiles ? warning : accent);
                };
            }))
            page_ = Page::Tasks;
    }
    void instanceMenu(const Instance &item) {
        openMenu(item.name, gameText(item.game),
                 {{tr(item.state == InstanceState::Ready ? "Play" : "Prepare and play"), [this, item] { play(item); }},
                  {tr("Open"), [this, item] { openInstance(item); }},
                  {tr("Rename"), [this, item] { renameInstance(item); }},
                  {tr("Copy"), [this, item] { duplicate(item); }},
                  {tr("Export (ZIP for PolyMC)"), [this, item] { exportZip(item); }},
                  {tr("Delete"), [this, item] { removeInstance(item); }, true}});
    }
    void renameInstance(Instance item) {
        edit(tr("Instance name"), item.name, 120, [this, item](std::string name) mutable {
            if (name.empty())
                return;
            item.name = std::move(name);
            storage_.saveInstance(item);
            if (detail_ && detail_->id == item.id)
                detail_ = item;
            refresh();
        });
    }
    void duplicate(const Instance &item) {
        edit(tr("Copy name"), item.name + tr(" (copy)"), 120, [this, item](std::string name) {
            if (name.empty())
                return;
            task(trf("Copying: {0}", {item.name}), [this, item, name](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                auto copy = duplicateInstance(storage_, item, name, progress, stop);
                return [this, copy] {
                    refresh();
                    toast(trf("Created copy \"{0}\"", {copy.name}));
                    remember(tr("Copying instance"), copy.name, true);
                };
            });
        });
    }
    void removeInstance(const Instance &item) {
        confirm(tr("Delete instance?"), trf("\"{0}\" will be deleted from the SD card with its worlds, mods and settings. This cannot be undone.", {item.name}),
                tr("Delete forever"), [this, item] {
                    task(trf("Deleting: {0}", {item.name}), [this, item](ProgressCallback, const std::atomic_bool *) -> Apply {
                        deleteInstance(storage_, item);
                        return [this, item] {
                            if (detail_ && detail_->id == item.id)
                                closeDetail();
                            refresh();
                            toast(trf("Deleted \"{0}\"", {item.name}));
                            remember(tr("Deleting instance"), item.name, true);
                        };
                    });
                });
    }
    void exportZip(const Instance &item) {
        if (task(trf("Export: {0}", {item.name}), [this, item](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                auto path = exportInstance(storage_, item, progress, stop);
                return [this, path, item] {
                    toast(trf("Export ready: {0}", {path}));
                    remember(trf("Export: {0}", {item.name}), path, true);
                    refresh();
                };
            }))
            page_ = Page::Tasks;
    }
    void chooseIcon(const Instance &item) {
        std::vector<MenuItem> items;
        if (pathExists(joinPath(item.root, "icon.png")))
            items.push_back({tr("Instance picture"), [this, item = Instance(item)]() mutable {
                                 item.icon = "file";
                                 saveDetail(item);
                             }});
        for (int i = 0; i < 8; ++i)
            items.push_back({tr(icon_names[i]), [this, item = Instance(item), i]() mutable {
                                 item.icon = "preset:" + std::to_string(i);
                                 saveDetail(item);
                             }});
        openMenu(tr("Instance icon"), "", std::move(items));
    }
    std::vector<MenuItem> heapChoices(int current, bool allow_default, std::function<void(int)> apply) {
        std::vector<MenuItem> items;
        if (allow_default)
            items.push_back({trf("Default ({0} MB)", {std::to_string(settings_.heap_mib)}) + (current == 0 ? tr(" · selected") : ""),
                             [apply] { apply(0); }});
        for (int size = 512; size <= heap_budget_.max_mib; size = nextHeapOption(size, heap_budget_.max_mib)) {
            items.push_back({mibText(size) + (current == size ? tr(" · selected") : ""), [apply, size] { apply(size); }});
            if (size == heap_budget_.max_mib)
                break;
        }
        return items;
    }

    // ------------------------------------------------------- content actions
    void checkUpdates() {
        if (!detail_)
            return;
        auto item = *detail_;
        auto settings = settings_;
        auto kind = tabKind(tab_);
        auto epoch = tab_epoch_;
        std::vector<std::string> names;
        if (kind == ContentKind::Mod)
            for (const auto &mod : mods_)
                names.push_back(mod.filename);
        else
            for (const auto &pack : packs_)
                if (!pack.directory)
                    names.push_back(pack.filename);
        task(trf("Checking updates: {0}", {kindTitle(kind)}),
             [this, item, settings, kind, epoch, names](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                 auto folder = joinPath(instanceRoot(storage_, item), std::string("game/") + contentFolder(kind));
                 std::map<std::string, LocalFile> files;
                 std::uint32_t done = 0;
                 for (const auto &name : names) {
                     progress({"scan", name, 0, 0, done++, static_cast<std::uint32_t>(names.size())});
                     try {
                         files[name] = digestFile(joinPath(folder, name), stop);
                     } catch (const Error &error) {
                         if (error.code == "cancelled")
                             throw;
                     }
                 }
                 progress({"catalog", "Searching Modrinth and CurseForge", 0, 0, done, done});
                 CatalogClient catalog(http_, settings);
                 auto known = catalog.identify(files, kind, stop);
                 auto newer = catalog.updates(known, files, kind, item.game.minecraft_version, item.game.loader.kind, stop);
                 return [this, epoch, newer, known] {
                     if (epoch != tab_epoch_)
                         return;
                     updates_ = newer;
                     updates_checked_ = true;
                     toast(newer.empty() ? trf("Everything is up to date · {0} recognized", {std::to_string(known.size())})
                                         : trf("Updates available: {0}", {std::to_string(newer.size())}),
                           newer.empty() ? accent : info);
                 };
             });
    }
    void updateContent(std::vector<std::pair<std::string, CatalogVersion>> list) {
        if (!detail_ || list.empty())
            return;
        auto item = *detail_;
        auto settings = settings_;
        auto kind = tabKind(tab_);
        task(trf("Updating: {0}", {list.size() == 1 ? list.front().first
                                                    : countText(static_cast<long>(list.size()), "file", "files", "файл", "файла", "файлов")}),
             [this, item, settings, kind, list](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                 CatalogClient catalog(http_, settings);
                 ModManager manager(storage_, http_);
                 std::vector<std::string> failed;
                 std::uint32_t done = 0;
                 for (const auto &entry : list) {
                     try {
                         auto exact = catalog.version(entry.second.provider, entry.second.project_id, entry.second.id, stop, kind);
                         progress({"update", entry.first, 0, 0, done, static_cast<std::uint32_t>(list.size())});
                         if (kind == ContentKind::Mod)
                             manager.update(item, entry.first, exact, {}, stop);
                         else {
                             removeContent(storage_, item, kind, entry.first);
                             installPack(storage_, http_, item, kind, exact, {}, stop);
                         }
                     } catch (const Error &error) {
                         if (error.code == "cancelled")
                             throw;
                         failed.push_back(entry.first + ": " + tr(error.what()));
                     }
                     ++done;
                 }
                 return [this, failed, done] {
                     if (failed.empty())
                         toast(trf("Updated: {0}", {std::to_string(done)}));
                     else
                         toast(trf("Could not update: {0}", {failed.front()}), danger);
                     remember(tr("Update"), failed.empty() ? tr("Success") : failed.front(), failed.empty());
                     loadTab();
                 };
             });
    }
    void modMenu(const InstalledMod &mod) {
        auto item = *detail_;
        std::vector<MenuItem> items{{tr(mod.enabled ? "Disable" : "Enable"), [this, item, mod] { toggleMod(mod); }}};
        auto update = updates_.find(mod.filename);
        if (update != updates_.end())
            items.push_back({trf("Update to {0}", {update->second.name}), [this, mod, update] { updateContent({{mod.filename, update->second}}); }});
        items.push_back({tr("Delete"), [this, item, mod] {
                             confirm(tr("Delete mod?"), mod.name + " (" + mod.filename + ")", tr("Delete"), [this, item, mod] {
                                 local("Deleting mod", [this, item, mod] { ModManager(storage_, http_).remove(item, mod.filename); },
                                       tr("Mod deleted"));
                             });
                         },
                         true});
        openMenu(mod.name, mod.problem.empty() ? mod.version + " · " + mod.filename : tr(mod.problem), std::move(items));
    }
    void toggleMod(const InstalledMod &mod) {
        auto item = *detail_;
        local(mod.enabled ? "Disabling mod" : "Enabling mod",
              [this, item, mod] { ModManager(storage_, http_).setEnabled(item, mod.filename, !mod.enabled); });
    }
    void packMenu(const PackEntry &pack, ContentKind kind) {
        auto item = *detail_;
        std::vector<MenuItem> items{{tr(pack.enabled ? "Disable" : "Enable"), [this, item, pack, kind] {
                                         local("Toggling pack", [this, item, pack, kind] { setPackEnabled(storage_, item, kind, pack.filename, !pack.enabled); });
                                     }}};
        auto update = updates_.find(pack.filename);
        if (update != updates_.end())
            items.push_back({trf("Update to {0}", {update->second.name}), [this, pack, update] { updateContent({{pack.filename, update->second}}); }});
        items.push_back({tr("Delete"), [this, item, pack, kind] {
                             confirm(tr("Delete?"), pack.name, tr("Delete"), [this, item, pack, kind] {
                                 local("Deleting pack", [this, item, pack, kind] { removeContent(storage_, item, kind, pack.filename); }, tr("Deleted"));
                             });
                         },
                         true});
        openMenu(pack.name, pack.description.empty() ? pack.filename : pack.description, std::move(items));
    }
    void worldMenu(const WorldInfo &world) {
        auto item = *detail_;
        std::vector<MenuItem> items;
        if (quickPlaySupported(item.game.minecraft_version))
            items.push_back({tr("Play this world"), [this, item, world] { play(item, {"--quickPlaySingleplayer", world.folder}); }});
        items.push_back({tr("Rename"), [this, item, world] {
                             edit(tr("World name"), world.name, 128, [this, item, world](std::string name) {
                                 if (!name.empty())
                                     local("Renaming world", [this, item, world, name] { renameWorld(storage_, item, world.folder, name); },
                                           tr("World renamed"));
                             });
                         }});
        items.push_back({tr("Backup (ZIP)"), [this, item, world] {
                             task(trf("Backup: {0}", {world.name}), [this, item, world](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                                 auto path = backupWorld(storage_, item, world.folder, progress, stop);
                                 return [this, path] {
                                     toast(trf("Backup saved: {0}", {path.substr(path.find_last_of('/') + 1)}));
                                     remember(tr("World backup"), path, true);
                                     refresh();
                                 };
                             });
                         }});
        items.push_back({tr("Copy world"), [this, item, world] {
                             task(trf("Copying world: {0}", {world.name}), [this, item, world](ProgressCallback progress, const std::atomic_bool *stop) -> Apply {
                                 copyWorld(storage_, item, world.folder, progress, stop);
                                 return [this] {
                                     toast(tr("World copy created"));
                                     refresh();
                                     loadTab();
                                 };
                             });
                         }});
        items.push_back({tr("Delete"), [this, item, world] {
                             confirm(tr("Delete world?"), trf("\"{0}\" will be deleted permanently. You can make a backup first.", {world.name}),
                                     tr("Delete forever"), [this, item, world] {
                                         local("Deleting world", [this, item, world] { deleteWorld(storage_, item, world.folder); }, tr("World deleted"));
                                     });
                         },
                         true});
        openMenu(world.name, modeText(world.game_type, world.hardcore) + (world.version.empty() ? "" : " · " + world.version) + " · " + trf("folder {0}", {world.folder}),
                 std::move(items));
    }
    void saveServerList(std::vector<ServerEntry> list, const std::string &done) {
        auto item = *detail_;
        local("Saving servers", [this, item, list] { saveServers(storage_, item, list); }, done);
    }
    void addServer() {
        edit(tr("Server address (host or host:port)"), "", 255, [this](std::string address) {
            if (address.empty())
                return;
            edit(tr("Server name"), "Minecraft Server", 64, [this, address](std::string name) {
                auto list = servers_;
                list.push_back({name, address, -1});
                saveServerList(list, tr("Server added"));
            });
        });
    }
    void serverMenu(int index) {
        auto item = *detail_;
        auto server = servers_[index];
        std::vector<MenuItem> items;
        items.push_back({tr("Connect"), [this, item, server] {
                             if (quickPlaySupported(item.game.minecraft_version))
                                 play(item, {"--quickPlayMultiplayer", server.address});
                             else {
                                 auto host = server.address, port = std::string("25565");
                                 auto colon = host.find_last_of(':');
                                 if (colon != std::string::npos && host.find(':') == colon) {
                                     port = host.substr(colon + 1);
                                     host.resize(colon);
                                 }
                                 play(item, {"--server", host, "--port", port});
                             }
                         }});
        items.push_back({tr("Change address"), [this, index, server] {
                             edit(tr("Server address"), server.address, 255, [this, index](std::string value) {
                                 if (value.empty())
                                     return;
                                 auto list = servers_;
                                 list[index].address = value;
                                 saveServerList(list, tr("Address changed"));
                             });
                         }});
        items.push_back({tr("Change name"), [this, index, server] {
                             edit(tr("Server name"), server.name, 64, [this, index](std::string value) {
                                 auto list = servers_;
                                 list[index].name = value;
                                 saveServerList(list, tr("Name changed"));
                             });
                         }});
        if (index > 0)
            items.push_back({tr("Move up"), [this, index] {
                                 auto list = servers_;
                                 std::swap(list[index], list[index - 1]);
                                 saveServerList(list, {});
                             }});
        items.push_back({tr("Delete"), [this, index, server] {
                             confirm(tr("Delete server?"), server.name + " · " + server.address, tr("Delete"), [this, index] {
                                 auto list = servers_;
                                 list.erase(list.begin() + index);
                                 saveServerList(list, tr("Server deleted"));
                             });
                         },
                         true});
        openMenu(server.name, server.address, std::move(items));
    }
    void deleteShot(int index) {
        auto item = *detail_;
        auto shot = shots_[index];
        confirm(tr("Delete screenshot?"), shot.name, tr("Delete"), [this, item, shot] {
            images_.forget(shot.path);
            local("Deleting screenshot", [this, item, shot] { deleteScreenshot(storage_, item, shot.name); }, tr("Screenshot deleted"));
            if (viewer_ && *viewer_ >= static_cast<int>(shots_.size()) - 1)
                viewer_ = std::max(0, static_cast<int>(shots_.size()) - 2);
            if (shots_.size() <= 1)
                viewer_.reset();
        });
    }
    void openLog(const MediaFile &file) {
        auto item = *detail_;
        query("Reading log", [this, item, file](ProgressCallback, const std::atomic_bool *) -> Apply {
            auto lines = readLog(storage_, item, file.path);
            return [this, file, lines] {
                log_ = LogView{file.name, lines, std::max(0, static_cast<int>(lines.size()) - 28), 0};
            };
        });
    }

    // --------------------------------------------------------------- wizard
    void openWizard(std::optional<Instance> target = {}) {
        Wizard w;
        if (target) {
            w.editing = true;
            w.target = *target;
            w.name = target->name;
            w.minecraft = target->game.minecraft_version;
            w.loader = target->game.loader.kind == LoaderKind::Unknown ? LoaderKind::Vanilla : target->game.loader.kind;
            w.loader_version = target->game.loader.version;
        } else
            w.name = "";
        wizard_ = w;
        focus_ = -1;
        loadGameVersions();
        if (w.loader != LoaderKind::Vanilla)
            loadLoaderVersions();
    }
    void loadGameVersions() {
        wizard_->versions_loading = true;
        wizard_->versions_error.clear();
        auto epoch = ++wizard_->epoch;
        query("Minecraft version list", [this, epoch](ProgressCallback, const std::atomic_bool *stop) -> Apply {
            std::vector<GameVersion> list;
            std::string error;
            try {
                list = gameVersions(storage_, http_, stop);
            } catch (const Error &e) {
                error = errorText(e.code, e.what());
            }
            return [this, list, error, epoch] {
                if (!wizard_ || wizard_->epoch != epoch)
                    return;
                wizard_->versions = list;
                wizard_->versions_error = error;
                wizard_->versions_loading = false;
                focus_ = -1;
                if (wizard_->minecraft.empty())
                    for (const auto &v : list)
                        if (v.type == "release" && v.java_major == 21) {
                            wizard_->minecraft = v.id;
                            break;
                        }
            };
        });
    }
    void loadLoaderVersions() {
        if (!wizard_)
            return;
        auto loader = wizard_->loader;
        auto minecraft = wizard_->minecraft;
        wizard_->loader_versions.clear();
        wizard_->loader_error.clear();
        if (loader == LoaderKind::Vanilla || minecraft.empty()) {
            wizard_->loader_loading = false;
            return;
        }
        wizard_->loader_loading = true;
        auto epoch = ++wizard_->epoch;
        query("Versions: " + loaderText(loader), [this, loader, minecraft, epoch](ProgressCallback, const std::atomic_bool *stop) -> Apply {
            std::vector<LoaderVersion> list;
            std::string error;
            try {
                list = loaderVersions(http_, loader, minecraft, stop);
            } catch (const Error &e) {
                error = errorText(e.code, e.what());
            }
            return [this, list, error, epoch] {
                if (!wizard_ || wizard_->epoch != epoch)
                    return;
                wizard_->loader_versions = list;
                wizard_->loader_error = error;
                wizard_->loader_loading = false;
                auto keep = std::find_if(list.begin(), list.end(), [&](const auto &v) { return v.version == wizard_->loader_version; });
                if (keep == list.end()) {
                    auto stable = std::find_if(list.begin(), list.end(), [](const auto &v) { return v.stable; });
                    wizard_->loader_version = stable != list.end() ? stable->version : list.empty() ? "" : list.front().version;
                }
            };
        });
    }
    void finishWizard() {
        auto &w = *wizard_;
        if (w.minecraft.empty())
            return toast(tr("Choose a Minecraft version"), warning);
        if (w.loader != LoaderKind::Vanilla && w.loader_version.empty())
            return toast(tr("Choose a loader version"), warning);
        if (w.editing) {
            auto item = w.target;
            bool changed = item.game.minecraft_version != w.minecraft;
            item.game.minecraft_version = w.minecraft;
            item.game.loader = {w.loader, w.loader == LoaderKind::Vanilla ? "" : w.loader_version};
            if (item.state != InstanceState::NeedsFiles) {
                item.state = InstanceState::NeedsPreparation;
                item.reason = "Version changed: files will be prepared before launch";
            }
            saveDetail(item);
            wizard_.reset();
            focus_ = -1;
            toast(tr(changed ? "Version changed. Check mod compatibility." : "Loader changed"));
            return;
        }
        Instance instance;
        instance.id = newId();
        instance.name = w.name.empty() ? "Minecraft " + w.minecraft + (w.loader == LoaderKind::Vanilla ? "" : " " + loaderText(w.loader)) : w.name;
        instance.game.minecraft_version = w.minecraft;
        instance.game.loader = {w.loader, w.loader == LoaderKind::Vanilla ? "" : w.loader_version};
        instance.root = joinPath(storage_.root(), "instances/" + instance.id);
        instance.reason = "Minecraft files need to be downloaded";
        instance.icon = "preset:" + std::to_string(std::rand() % 8);
        createDirectories(joinPath(instance.root, "game"));
        storage_.saveInstance(instance);
        wizard_.reset();
        refresh();
        openInstance(instance);
        toast(tr("Instance created"));
    }

    // -------------------------------------------------------------- widgets
    int addAction(SDL_Rect rect, std::function<void()> call, bool enabled, const std::string &label,
                  SDL_Rect clip = {0, 0, 1280, 720}, int group = -1) {
        actions_.push_back({rect, std::move(call), enabled, label, clip, group});
        return static_cast<int>(actions_.size()) - 1;
    }
    bool focused(int index) const { return focus_ == index; }
    // Places text so its capital letters are centred on cy.
    void label(const std::string &text, int x, int cy, int size, Color color = foreground, int maximum = 0) {
        draw_.text(text, x, cy - size * 58 / 100, size, color, maximum);
    }
    bool button(SDL_Rect rect, const std::string &label, std::function<void()> call, bool enabled = true, bool selected = false,
                bool primary = false, int size = 20) {
        int index = addAction(rect, std::move(call), enabled, label);
        Color fill = primary && enabled ? accent : selected ? raised : panel;
        draw_.rounded(rect, 10, fill);
        if (focused(index))
            draw_.roundedOutline({rect.x - 3, rect.y - 3, rect.w + 6, rect.h + 6}, 12, enabled ? foreground : muted, 3);
        else if (selected)
            draw_.roundedOutline(rect, 10, border, 1);
        auto color = primary && enabled ? background : enabled ? foreground : muted;
        int width = draw_.width(label, size);
        int x = rect.w - 28 >= width ? rect.x + (rect.w - width) / 2 : rect.x + 14;
        this->label(label, x, rect.y + rect.h / 2, size, color, rect.w - 28);
        return focused(index);
    }
    int badge(int x, int y, const std::string &text, Color color, int size = 15) {
        int w = draw_.width(text, size) + 16;
        draw_.rounded({x, y, w, size + 12}, (size + 12) / 2, {color.r, color.g, color.b, 45});
        label(text, x + 8, y + (size + 12) / 2, size, color);
        return w;
    }
    void toggleSwitch(int x, int y, bool on) {
        draw_.rounded({x, y, 52, 28}, 14, on ? accent : border);
        draw_.circle(on ? x + 38 : x + 14, y + 14, 10, on ? background : muted);
    }
    void pattern(SDL_Rect r, Color color, int variant) {
        draw_.rounded(r, std::max(4, r.w / 8), color);
        Color shade{static_cast<unsigned char>(color.r / 2), static_cast<unsigned char>(color.g / 2), static_cast<unsigned char>(color.b / 2)};
        int cell = r.w / 5;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                if ((row * 3 + col * 7 + variant) % 5 < 2)
                    draw_.box({r.x + cell / 2 + col * cell, r.y + cell / 2 + row * cell, cell - 1, cell - 1}, shade);
    }
    void instanceIcon(const Instance &item, SDL_Rect r) {
        if (item.icon == "file") {
            if (auto *texture = images_.get(joinPath(item.root, "icon.png"), 128, 128)) {
                draw_.rounded(r, std::max(4, r.w / 8), raised);
                draw_.image(texture, r);
                return;
            }
        }
        unsigned hash = 0;
        for (char c : item.id)
            hash = hash * 31 + static_cast<unsigned char>(c);
        int color = static_cast<int>(hash % 8);
        if (item.icon.compare(0, 7, "preset:") == 0)
            color = std::clamp(std::atoi(item.icon.c_str() + 7), 0, 7);
        pattern(r, icon_colors[color], static_cast<int>(hash % 5));
    }
    void remoteIcon(const std::string &url, SDL_Rect r, int variant) {
        draw_.rounded(r, 10, raised);
        if (auto *texture = images_.get(url, 96, 96))
            draw_.image(texture, r);
        else
            pattern({r.x + 8, r.y + 8, r.w - 16, r.h - 16}, border, variant);
    }
    void header(const std::string &title, const std::string &subtitle) {
        draw_.text(title, 272, 26, 32, foreground, 960);
        if (!subtitle.empty())
            draw_.text(subtitle, 274, 76, 18, muted, 950);
    }
    void hint(const std::string &key, const std::string &label) { hints_.push_back({key, label}); }
    void empty(SDL_Rect area, const std::string &title, const std::string &text) {
        draw_.rounded(area, 14, panel);
        int w = draw_.width(title, 24);
        draw_.text(title, area.x + (area.w - w) / 2, area.y + area.h / 2 - 50, 24, foreground);
        draw_.wrap(text, area.x + 60, area.y + area.h / 2 - 6, 18, muted, area.w - 120, 3);
    }
    // Lays out count items in a scrolling grid and registers each as an action.
    void grid(const std::string &id, SDL_Rect area, int count, int columns, int item_h, int gap,
              const std::function<void(int, SDL_Rect, bool)> &paint, const std::function<void(int)> &select,
              const std::function<std::string(int)> &label = {}) {
        int group = ++group_counter_;
        auto &state = scroll_[id];
        int rows = (count + columns - 1) / columns;
        int content = rows ? rows * item_h + (rows - 1) * gap : 0;
        int max_scroll = std::max(0, content - area.h);
        int first = static_cast<int>(actions_.size());
        if (focus_ >= first && focus_ < first + count) {
            int row = (focus_ - first) / columns;
            float top = static_cast<float>(row * (item_h + gap)), bottom = top + item_h;
            if (top < state.target)
                state.target = std::max(0.0f, top - gap);
            if (bottom > state.target + area.h)
                state.target = bottom - area.h + (row + 1 < rows ? gap + item_h / 3 : 0);
        }
        state.target = std::clamp(state.target, 0.0f, static_cast<float>(max_scroll));
        if (settings_.reduced_motion || std::fabs(state.target - state.current) < 1)
            state.current = state.target;
        else
            state.current += (state.target - state.current) * 0.35f;
        int item_w = (area.w - (columns - 1) * gap - (max_scroll ? 10 : 0)) / columns;
        draw_.pushClip(area);
        for (int i = 0; i < count; ++i) {
            SDL_Rect r{area.x + (i % columns) * (item_w + gap), area.y + (i / columns) * (item_h + gap) - static_cast<int>(state.current),
                       item_w, item_h};
            int index = addAction(r, [select, i] { select(i); }, true, label ? label(i) : std::string(), area, group);
            if (r.y + r.h >= area.y && r.y <= area.y + area.h)
                paint(i, r, focused(index));
        }
        draw_.popClip();
        if (max_scroll) {
            int bar = std::max(30, area.h * area.h / std::max(1, content));
            int y = area.y + static_cast<int>((area.h - bar) * (state.current / max_scroll));
            draw_.rounded({area.x + area.w - 5, area.y, 5, area.h}, 2, {49, 68, 87, 120});
            draw_.rounded({area.x + area.w - 5, y, 5, bar}, 2, muted);
        }
        lists_.push_back({id, area, max_scroll});
    }
    void row(SDL_Rect r, bool focus, Color fill = panel) {
        draw_.rounded(r, 10, focus ? raised : fill);
        if (focus)
            draw_.roundedOutline({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 12, accent, 3);
    }

    // -------------------------------------------------------------- screens
    void sidebarPanel() {
        draw_.box({0, 0, 240, 720}, sidebar);
        draw_.rounded({26, 30, 38, 38}, 8, accent);
        draw_.box({34, 38, 9, 9}, background);
        draw_.box({48, 38, 9, 9}, background);
        draw_.box({40, 52, 11, 9}, background);
        draw_.text("MineSwitch", 76, 28, 26);
        draw_.text(preview_ ? tr("PREVIEW") : "JAVA EDITION", 78, 64, 13, muted);
        const char *pages[] = {"Library", "Catalog", "Import from SD", "Tasks", "Settings"};
        for (int i = 0; i < 5; ++i) {
            bool selected = static_cast<int>(page_) == i;
            SDL_Rect r{16, 112 + i * 58, 208, 48};
            std::string text = tr(pages[i]);
            int index = addAction(r, [this, i] { changePage(static_cast<Page>(i)); }, true, text);
            draw_.rounded(r, 10, selected ? raised : sidebar);
            if (selected)
                draw_.rounded({16, r.y + 12, 4, 24}, 2, accent);
            if (focused(index))
                draw_.roundedOutline({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 12, accent, 3);
            draw_.text(text, 36, r.y + 9, 20, selected ? foreground : muted);
            if (i == 3 && (task_.active || !history_.empty()))
                draw_.circle(200, r.y + 24, 5, task_.active ? accent : border);
        }
        if (task_.active) {
            auto p = task_.progress();
            draw_.rounded({16, 432, 208, 74}, 10, panel);
            draw_.text(task_.title, 28, 438, 15, foreground, 186);
            draw_.text(tr(p.detail.empty() ? "Please wait…" : p.detail), 28, 460, 13, muted, 186);
            draw_.rounded({28, 488, 184, 6}, 3, border);
            double fraction = p.total ? std::min(1.0, p.completed / double(p.total))
                              : p.files_total ? std::min(1.0, p.files_done / double(p.files_total)) : -1;
            if (fraction >= 0)
                draw_.rounded({28, 488, std::max(6, static_cast<int>(184 * fraction)), 6}, 3, accent);
            else
                draw_.rounded({28 + static_cast<int>((SDL_GetTicks() / 10) % 140), 488, 44, 6}, 3, accent);
        }
        draw_.box({26, 538, 188, 1}, border);
        draw_.text(profile().username, 28, 552, 22, accent, 186);
        draw_.text(tr("Local profile"), 28, 586, 14, muted);
        draw_.text(trf("SD · {0} free", {sizeText(free_bytes_)}), 28, 616, 14, muted);
        content_start_ = static_cast<int>(actions_.size());
    }
    void library() {
        header(tr("Library"), countText(static_cast<long>(instances_.size()), "instance", "instances", "сборка", "сборки", "сборок") +
                                  " · Minecraft Java Edition");
        const char *sorts[] = {"By name", "By last played", "By version"};
        button({760, 30, 216, 44}, tr(sorts[settings_.library_sort]), [this] {
            settings_.library_sort = (settings_.library_sort + 1) % 3;
            if (!preview_)
                storage_.saveSettings(settings_);
            sortInstances();
        });
        button({990, 30, 120, 44}, tr("Import"), [this] { changePage(Page::Files); });
        button({1124, 30, 132, 44}, tr("+ Create"), [this] { openWizard(); }, true, false, true);
        hint("A", tr("Open"));
        hint("X", tr("Actions"));
        hint("Y", tr("Play"));
        if (instances_.empty()) {
            empty({272, 112, 984, 540}, tr("Your library starts here"),
                  tr("Create an instance with the Minecraft version and loader you need, import an archive from the SD card or find a ready-made modpack in the Modrinth and CurseForge catalog."));
            button({560, 480, 200, 52}, tr("+ Create"), [this] { openWizard(); }, true, false, true);
            button({776, 480, 200, 52}, tr("Catalog"), [this] { changePage(Page::Catalog); });
            return;
        }
        grid("library", {272, 104, 984, 560}, static_cast<int>(instances_.size()), 3, 168, 16,
             [this](int i, SDL_Rect r, bool focus) {
                 const auto &item = instances_[i];
                 row(r, focus);
                 instanceIcon(item, {r.x + 18, r.y + 18, 76, 76});
                 draw_.text(item.name, r.x + 110, r.y + 18, 21, foreground, r.w - 128);
                 draw_.text(item.game.minecraft_version + " · " + loaderText(item.game.loader.kind), r.x + 110, r.y + 52, 17, muted, r.w - 128);
                 badge(r.x + 110, r.y + 82, stateText(item.state), stateColor(item.state), 14);
                 draw_.text(trf("Played: {0}", {agoText(item.last_played)}), r.x + 18, r.y + 124, 15, muted, r.w - 36);
             },
             [this](int i) { openInstance(instances_[i]); }, [this](int i) { return "instance:" + instances_[i].name; });
    }
    void instancePage() {
        auto item = *detail_;
        draw_.rounded({272, 20, 984, 128}, 14, panel);
        instanceIcon(item, {292, 38, 92, 92});
        draw_.text(item.name, 404, 36, 28, foreground, 560);
        draw_.text(gameText(item.game), 406, 78, 17, muted, 560);
        int x = 406;
        x += badge(x, 106, stateText(item.state), stateColor(item.state), 14) + 8;
        x += badge(x, 106, mibText(std::min(effectiveHeap(item), heap_budget_.max_mib)), info, 14) + 8;
        badge(x, 106, profile().username, muted, 14);
        bool compatible = item.state != InstanceState::Unsupported && item.state != InstanceState::Broken;
        button({988, 44, 248, 64},
               tr(item.state == InstanceState::NeedsFiles ? "Check files" : item.state == InstanceState::Ready ? "Play" : "Prepare"),
               [this, item] { play(item); }, !task_.active && compatible, false, true, 22);
        button({272, 160, 52, 40}, "<", [this] { closeDetail(); });
        int tx = 336;
        for (int i = 0; i < tab_count; ++i) {
            std::string name = tr(tab_names[i]);
            int w = draw_.width(name, 17) + 22;
            bool selected = static_cast<int>(tab_) == i;
            SDL_Rect r{tx, 160, w, 40};
            int index = addAction(r, [this, i] { selectTab(static_cast<Tab>(i)); }, true, "tab:" + name);
            draw_.rounded(r, 10, selected ? raised : background);
            if (selected)
                draw_.rounded({r.x + 10, r.y + 36, r.w - 20, 3}, 1, accent);
            if (focused(index))
                draw_.roundedOutline({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 12, accent, 2);
            draw_.text(name, tx + 11, 166, 17, selected ? foreground : muted);
            tx += w + 4;
        }
        hint("L/R", tr("Tabs"));
        SDL_Rect area{272, 214, 984, 452};
        switch (tab_) {
        case Tab::Overview: overviewTab(item, area); break;
        case Tab::Mods: modsTab(item, area); break;
        case Tab::ResourcePacks:
        case Tab::ShaderPacks: packsTab(item, area); break;
        case Tab::Worlds: worldsTab(item, area); break;
        case Tab::Servers: serversTab(item, area); break;
        case Tab::Screenshots: shotsTab(area); break;
        case Tab::Logs: logsTab(area); break;
        case Tab::Settings: instanceSettingsTab(item, area); break;
        }
    }
    void overviewTab(const Instance &item, SDL_Rect area) {
        draw_.rounded({area.x, area.y, 620, area.h}, 14, panel);
        int y = area.y + 20;
        auto line = [&](const std::string &label, const std::string &value, Color color = foreground) {
            draw_.text(label, area.x + 22, y, 15, muted);
            draw_.wrap(value.empty() ? "—" : value, area.x + 200, y - 2, 17, color, 400, 2);
            y += value.size() > 48 ? 56 : 36;
        };
        line(tr("Status"), item.reason.empty() ? stateText(item.state) : stateText(item.state) + ": " + tr(item.reason), stateColor(item.state));
        line(tr("Version"), gameText(item.game));
        line(tr("Last played"), agoText(item.last_played));
        line(tr("Java memory"), (item.heap_mib ? mibText(item.heap_mib) : trf("Default · {0} MB", {std::to_string(settings_.heap_mib)})) +
                                    (settings_.full_initial_heap ? " · Xms = Xmx" : " · Xms " + mibText(256)));
        line(tr("Download size"), item.installed_bytes ? sizeText(item.installed_bytes) : "");
        line(tr("Folder"), item.root);
        if (!item.packwiz_url.empty())
            line("packwiz", item.packwiz_url);
        draw_.text(tr("Notes"), area.x + 22, y + 6, 15, muted);
        draw_.wrap(item.notes.empty() ? tr("No notes") : item.notes, area.x + 22, y + 30, 17, item.notes.empty() ? muted : foreground, 576, 3);
        if (item.state == InstanceState::NeedsFiles)
            draw_.wrap(tr("Missing files are listed in missing-files.json in the instance folder. Copy the files and choose \"Check files\"."),
                       area.x + 22, area.y + area.h - 56, 15, warning, 576, 2);
        int bx = area.x + 640, bw = 344, by = area.y;
        auto action = [&](const std::string &label, std::function<void()> call, bool enabled = true) {
            button({bx, by, bw, 46}, label, std::move(call), enabled);
            by += 54;
        };
        bool idle = !task_.active;
        action(tr("Version and loader…"), [this, item] { openWizard(item); }, idle);
        button({bx, by, bw / 2 - 4, 46}, tr("Name"), [this, item] { renameInstance(item); });
        button({bx + bw / 2 + 4, by, bw / 2 - 4, 46}, tr("Icon"), [this, item] { chooseIcon(item); });
        by += 54;
        if (!item.packwiz_url.empty())
            action(tr("Update mods (packwiz)"), [this, item] { syncPack(item); }, idle);
        action(tr("Notes"), [this, item]() {
            edit(tr("Notes"), item.notes, 2000, [this, item = Instance(item)](std::string text) mutable {
                item.notes = std::move(text);
                saveDetail(item);
            });
        });
        action(tr("Copy instance"), [this, item] { duplicate(item); }, idle);
        action(tr("Export ZIP (PolyMC)"), [this, item] { exportZip(item); }, idle);
        action(tr("Full file check"), [this, item] { prepare(item, true); },
               idle && item.state != InstanceState::Unsupported && item.state != InstanceState::NeedsFiles);
        action(tr("Delete instance"), [this, item] { removeInstance(item); }, idle);
    }
    void modsTab(const Instance &item, SDL_Rect area) {
        bool loader = item.game.loader.kind != LoaderKind::Vanilla && item.game.loader.kind != LoaderKind::Unknown;
        if (!loader) {
            empty(area, tr("Mods need a loader"),
                  tr("This instance has no mod loader. Choose Fabric, Quilt, Forge or NeoForge with \"Version and loader\" on the Overview tab."));
            button({area.x + 362, area.y + area.h - 110, 260, 50}, tr("Version and loader…"), [this, item] { openWizard(item); }, true, false, true);
            return;
        }
        contentToolbar(area, ContentKind::Mod,
                       trf("{0} of {1} enabled", {std::to_string(std::count_if(mods_.begin(), mods_.end(), [](const auto &m) { return m.enabled; })),
                                                  std::to_string(mods_.size())}));
        SDL_Rect list{area.x, area.y + 60, area.w, area.h - 60};
        if (mods_.empty()) {
            empty(list, tr(tab_loading_ ? "Reading mods…" : "No mods yet"),
                  tr("Press \"Add\" to find mods on Modrinth or CurseForge. Required dependencies are installed automatically."));
            return;
        }
        hint("A", tr("On/off"));
        hint("X", tr("Actions"));
        grid("tab:mods", list, static_cast<int>(mods_.size()), 1, 60, 8,
             [this](int i, SDL_Rect r, bool focus) {
                 const auto &mod = mods_[i];
                 row(r, focus);
                 draw_.text(mod.name, r.x + 18, r.y + 6, 19, mod.enabled ? foreground : muted, r.w - 340);
                 draw_.text(mod.problem.empty() ? (mod.version.empty() ? mod.filename : mod.version + " · " + mod.filename) : tr(mod.problem),
                            r.x + 18, r.y + 34, 14, mod.problem.empty() ? muted : warning, r.w - 340);
                 auto update = updates_.find(mod.filename);
                 if (update != updates_.end())
                     badge(r.x + r.w - 300, r.y + 16, tr("Update"), info, 14);
                 toggleSwitch(r.x + r.w - 76, r.y + 16, mod.enabled);
             },
             [this](int i) { toggleMod(mods_[i]); }, [this](int i) { return "mod:" + mods_[i].filename; });
    }
    // Button sized to its label; returns the x after it.
    int fit(int x, int y, int h, const std::string &text, std::function<void()> call, bool enabled = true, bool selected = false,
            bool primary = false) {
        int w = draw_.width(text, 18) + 40;
        button({x, y, w, h}, text, std::move(call), enabled, selected, primary, 18);
        return x + w + 10;
    }
    void contentToolbar(SDL_Rect area, ContentKind kind, const std::string &summary) {
        int x = fit(area.x, area.y, 46, tr("+ Add"), [this, kind] {
            browsing_content_ = true;
            if (content_browse_.kind != kind || !detail_) {
                content_browse_ = Browse{};
                content_browse_.kind = kind;
            }
            focus_ = -1;
        }, true, false, true);
        x = fit(x, area.y, 46, tr(updates_checked_ ? "Check again" : "Check for updates"), [this] { checkUpdates(); }, !task_.active);
        std::vector<std::pair<std::string, CatalogVersion>> all(updates_.begin(), updates_.end());
        if (!all.empty())
            x = fit(x, area.y, 46, trf("Update all ({0})", {std::to_string(all.size())}), [this, all] { updateContent(all); }, !task_.active, false, true);
        x = fit(x, area.y, 46, tr("Reload"), [this] { loadTab(); });
        label(summary, x + 8, area.y + 23, 16, muted, area.x + area.w - x - 8);
    }
    void packsTab(const Instance &item, SDL_Rect area) {
        auto kind = tabKind(tab_);
        contentToolbar(area, kind, countText(static_cast<long>(packs_.size()), "pack", "packs", "пак", "пака", "паков"));
        SDL_Rect list{area.x, area.y + 60, area.w, area.h - 60};
        if (packs_.empty()) {
            bool shaders = kind == ContentKind::ShaderPack;
            empty(list, tr(tab_loading_ ? "Reading files…" : shaders ? "No shaders yet" : "No resource packs yet"),
                  tr(shaders ? "Shaders need the Iris mod (Fabric/Quilt) or Oculus (Forge). Performance on Switch is limited."
                             : "Resource packs change textures and sounds. Enabled packs must also be selected in the game settings."));
            (void)item;
            return;
        }
        hint("A", tr("On/off"));
        hint("X", tr("Actions"));
        grid("tab:packs", list, static_cast<int>(packs_.size()), 1, 64, 8,
             [this, item, kind](int i, SDL_Rect r, bool focus) {
                 const auto &pack = packs_[i];
                 row(r, focus);
                 auto folder = joinPath(item.root, std::string("game/") + contentFolder(kind));
                 draw_.rounded({r.x + 12, r.y + 8, 48, 48}, 8, raised);
                 if (pack.directory) {
                     if (auto *icon = images_.get(joinPath(joinPath(folder, pack.filename), "pack.png"), 64, 64))
                         draw_.image(icon, {r.x + 12, r.y + 8, 48, 48});
                 }
                 draw_.text(pack.name, r.x + 76, r.y + 8, 19, pack.enabled ? foreground : muted, r.w - 380);
                 draw_.text(!pack.problem.empty() ? tr(pack.problem) : pack.description.empty() ? sizeText(pack.bytes) : pack.description,
                            r.x + 76, r.y + 36, 14, pack.problem.empty() ? muted : warning, r.w - 380);
                 if (updates_.count(pack.filename))
                     badge(r.x + r.w - 300, r.y + 18, tr("Update"), info, 14);
                 toggleSwitch(r.x + r.w - 76, r.y + 18, pack.enabled);
             },
             [this, kind](int i) {
                 auto item = *detail_;
                 auto pack = packs_[i];
                 local("Toggling pack", [this, item, pack, kind] { setPackEnabled(storage_, item, kind, pack.filename, !pack.enabled); });
             },
             [this](int i) { return "pack:" + packs_[i].filename; });
    }
    void worldsTab(const Instance &item, SDL_Rect area) {
        if (worlds_.empty()) {
            empty(area, tr(tab_loading_ ? "Reading worlds…" : "No worlds yet"),
                  tr("Create a world in the game and it will appear here: you can launch it directly, back it up and copy it."));
            return;
        }
        hint("A", tr("Actions"));
        draw_.text(countText(static_cast<long>(worlds_.size()), "world", "worlds", "мир", "мира", "миров"), area.x, area.y + 4, 16, muted);
        grid("tab:worlds", {area.x, area.y + 34, area.w, area.h - 34}, static_cast<int>(worlds_.size()), 1, 76, 8,
             [this, item](int i, SDL_Rect r, bool focus) {
                 const auto &world = worlds_[i];
                 row(r, focus);
                 SDL_Rect icon{r.x + 12, r.y + 10, 56, 56};
                 draw_.rounded(icon, 8, raised);
                 if (world.icon)
                     if (auto *texture = images_.get(joinPath(joinPath(item.root, "game/saves/" + world.folder), "icon.png"), 64, 64))
                         draw_.image(texture, icon);
                 draw_.text(world.name, r.x + 84, r.y + 10, 20, foreground, r.w - 120);
                 std::string details = world.problem.empty()
                                           ? modeText(world.game_type, world.hardcore) + (world.cheats ? " · " + tr("cheats") : "") +
                                                 (world.version.empty() ? "" : " · " + world.version) +
                                                 (world.last_played ? " · " + agoText(world.last_played / 1000) : "")
                                           : tr(world.problem);
                 draw_.text(details, r.x + 84, r.y + 42, 15, world.problem.empty() ? muted : warning, r.w - 120);
             },
             [this](int i) { worldMenu(worlds_[i]); }, [this](int i) { return "world:" + worlds_[i].folder; });
    }
    void serversTab(const Instance &item, SDL_Rect area) {
        button({area.x, area.y, 220, 46}, tr("+ Add server"), [this] { addServer(); }, true, false, true);
        draw_.text(tr(quickPlaySupported(item.game.minecraft_version) ? "\"Connect\" starts the game right on the server"
                                                                      : "The connection is passed to the game with --server"),
                   area.x + 240, area.y + 12, 16, muted, 700);
        SDL_Rect list{area.x, area.y + 60, area.w, area.h - 60};
        if (servers_.empty()) {
            empty(list, tr(tab_loading_ ? "Reading servers.dat…" : "No servers yet"),
                  tr("Add a server here or in the game. Servers without license checks (offline mode) are supported."));
            return;
        }
        hint("A", tr("Actions"));
        grid("tab:servers", list, static_cast<int>(servers_.size()), 1, 64, 8,
             [this](int i, SDL_Rect r, bool focus) {
                 row(r, focus);
                 draw_.text(servers_[i].name, r.x + 18, r.y + 8, 20, foreground, r.w - 40);
                 draw_.text(servers_[i].address, r.x + 18, r.y + 36, 15, muted, r.w - 40);
             },
             [this](int i) { serverMenu(i); }, [this](int i) { return "server:" + servers_[i].address; });
    }
    void shotsTab(SDL_Rect area) {
        if (shots_.empty()) {
            empty(area, tr(tab_loading_ ? "Reading screenshots…" : "No screenshots yet"), tr("Screenshots from the game (F2 on a keyboard) appear here."));
            return;
        }
        hint("A", tr("Open"));
        hint("X", tr("Delete"));
        grid("tab:shots", area, static_cast<int>(shots_.size()), 4, 150, 12,
             [this](int i, SDL_Rect r, bool focus) {
                 row(r, focus, raised);
                 SDL_Rect image{r.x + 6, r.y + 6, r.w - 12, r.h - 38};
                 if (auto *texture = images_.get(shots_[i].path, 320, 180))
                     draw_.image(texture, image, true);
                 draw_.text(dateText(shots_[i].modified), r.x + 10, r.y + r.h - 30, 14, muted, r.w - 20);
             },
             [this](int i) { viewer_ = i; }, [this](int i) { return "shot:" + shots_[i].name; });
    }
    void logsTab(SDL_Rect area) {
        if (logs_.empty()) {
            empty(area, tr(tab_loading_ ? "Looking for logs…" : "No logs yet"), tr("Game logs and crash reports appear after the first launch."));
            return;
        }
        hint("A", tr("Open"));
        grid("tab:logs", area, static_cast<int>(logs_.size()), 1, 58, 8,
             [this](int i, SDL_Rect r, bool focus) {
                 const auto &log = logs_[i];
                 row(r, focus);
                 bool crash = log.name.compare(0, 5, "Crash") == 0;
                 draw_.text(logName(log.name), r.x + 18, r.y + 6, 18, crash ? warning : foreground, r.w - 40);
                 draw_.text(dateText(log.modified) + " · " + sizeText(log.bytes), r.x + 18, r.y + 32, 14, muted);
             },
             [this](int i) { openLog(logs_[i]); }, [this](int i) { return "log:" + logs_[i].name; });
    }
    void instanceSettingsTab(const Instance &item, SDL_Rect area) {
        int y = area.y;
        auto setting = [&](const std::string &title, const std::string &value, std::function<void()> call) {
            int index = addAction({area.x, y, area.w, 64}, std::move(call), true, title);
            row({area.x, y, area.w, 64}, focused(index));
            draw_.text(title, area.x + 20, y + 8, 19);
            draw_.text(value, area.x + 20, y + 36, 15, muted, area.w - 40);
            y += 74;
        };
        setting(tr("Java memory"), item.heap_mib ? trf("{0} MB (this instance only)", {std::to_string(item.heap_mib)})
                                                 : trf("Default · {0} MB", {std::to_string(settings_.heap_mib)}),
                [this, item] {
                    openMenu(trf("Memory for \"{0}\"", {item.name}), trf("Maximum on this console: {0} MB", {std::to_string(heap_budget_.max_mib)}),
                             heapChoices(item.heap_mib, true, [this, item = Instance(item)](int size) mutable {
                                 item.heap_mib = size;
                                 saveDetail(item);
                             }));
                });
        setting(tr("Extra JVM arguments"), item.jvm_args.empty() ? tr("None · -D…, -XX:… and -Xss… are allowed") : item.jvm_args, [this, item] {
            edit(tr("Instance JVM arguments"), item.jvm_args, 1000, [this, item = Instance(item)](std::string text) mutable {
                parseJvmArguments(text);
                item.jvm_args = std::move(text);
                saveDetail(item);
                toast(tr("JVM arguments saved"));
            });
        });
        setting(tr("Minecraft version and loader"), gameText(item.game), [this, item] { openWizard(item); });
        setting(tr("Player profile"), trf("{0} · change it in the launcher settings", {profile().username}), [this] { changePage(Page::Settings); });
        draw_.wrap(tr("Memory and JVM arguments apply on the next launch. Defaults are set on the Settings page."), area.x + 4, y + 8, 16,
                   muted, area.w - 8, 2);
    }
    void browseScreen() {
        auto &b = browse();
        bool content = b.kind != ContentKind::Modpack;
        if (b.project)
            return projectScreen(b);
        if (!b.searched && !preview_)
            search();
        if (content) {
            header(trf("Add: {0}", {kindTitle(b.kind)}), detail_->name + " · " + gameText(detail_->game));
            button({1104, 30, 152, 44}, tr("Back"), [this] { browsing_content_ = false; focus_ = -1; });
        } else
            header(tr("Modpack catalog"), tr("Modrinth and CurseForge · pick an exact version before installing"));
        int y = 100;
        button({272, y, 150, 46}, "Modrinth", [this] { setProvider(Provider::Modrinth); }, true, b.provider == Provider::Modrinth);
        button({430, y, 160, 46}, "CurseForge", [this] { setProvider(Provider::CurseForge); }, true, b.provider == Provider::CurseForge);
        const char *sorts[] = {"Relevance", "Popular", "Recently updated"};
        button({602, y, 196, 46}, tr(sorts[static_cast<int>(b.sort)]), [this] {
            auto &target = browse();
            target.sort = static_cast<SearchSort>((static_cast<int>(target.sort) + 1) % 3);
            search();
        });
        if (!content)
            button({810, y, 220, 46}, "Minecraft: " + (b.minecraft.empty() ? tr("any") : b.minecraft), [this] {
                edit(tr("Minecraft version (empty for any)"), packs_browse_.minecraft, 40, [this](std::string s) {
                    packs_browse_.minecraft = std::move(s);
                    search();
                });
            });
        y += 58;
        button({272, y, 820, 48}, b.query.empty() ? tr("Search by name…") : trf("Search: {0}", {b.query}), [this] {
            edit(tr("Search"), browse().query, 128, [this](std::string s) {
                browse().query = std::move(s);
                search();
            });
        });
        button({1104, y, 152, 48}, tr("Find"), [this] { search(); }, true, false, true);
        hint("A", tr("Open"));
        hint("Y", tr("Search"));
        SDL_Rect area{272, y + 62, 984, 666 - y - 62};
        if (b.results.empty()) {
            empty(area, tr(b.loading ? "Searching…" : b.searched ? "Nothing found" : "Catalog"),
                  content && b.kind == ContentKind::Mod
                      ? trf("Only mods for {0} are shown. Dependencies are installed automatically.", {gameText(detail_->game)})
                      : tr("Compatibility is checked before launch. Mods with their own native libraries may not work on Switch."));
            return;
        }
        int count = static_cast<int>(b.results.size()) + (b.has_more ? 1 : 0);
        grid(content ? "browse:content" : "browse:packs", area, count, 1, 84, 8,
             [this, &b](int i, SDL_Rect r, bool focus) {
                 if (i == static_cast<int>(b.results.size())) {
                     row(r, focus, background);
                     std::string label = tr(b.loading ? "Loading…" : "Show more");
                     draw_.text(label, r.x + (r.w - draw_.width(label, 19)) / 2, r.y + 24, 19, accent);
                     return;
                 }
                 const auto &project = b.results[i];
                 row(r, focus);
                 remoteIcon(project.icon_url, {r.x + 12, r.y + 12, 60, 60}, i);
                 draw_.text(project.name, r.x + 88, r.y + 8, 20, foreground, r.w - 300);
                 draw_.text(project.summary, r.x + 88, r.y + 44, 15, muted, r.w - 110);
                 std::string meta = (project.author.empty() ? "" : project.author + " · ") + trf("{0} downloads", {amountText(project.downloads)});
                 int w = draw_.width(meta, 15);
                 draw_.text(meta, r.x + r.w - w - 18, r.y + 12, 15, muted, 260);
             },
             [this, &b](int i) {
                 if (i == static_cast<int>(b.results.size())) {
                     if (!b.loading)
                         search(true);
                 } else
                     openProject(b.results[i]);
             },
             [&b](int i) { return i < static_cast<int>(b.results.size()) ? "project:" + b.results[i].name : std::string("more"); });
    }
    void setProvider(Provider provider) {
        auto &b = browse();
        if (b.provider == provider)
            return;
        b.provider = provider;
        search();
    }
    void projectScreen(Browse &b) {
        const auto &project = *b.project;
        draw_.rounded({272, 20, 984, 136}, 14, panel);
        remoteIcon(project.icon_url, {292, 38, 100, 100}, 0);
        draw_.text(project.name, 412, 34, 28, foreground, 640);
        draw_.text((project.author.empty() ? "" : project.author + " · ") + trf("{0} downloads", {amountText(project.downloads)}) + " · " +
                       (project.provider == Provider::Modrinth ? "Modrinth" : "CurseForge"),
                   414, 74, 16, muted, 640);
        draw_.wrap(project.summary, 414, 100, 16, foreground, 800, 2);
        button({1104, 32, 136, 44}, tr("Back"), [this] {
            browse().project.reset();
            focus_ = -1;
        });
        bool content = b.kind != ContentKind::Modpack;
        draw_.text(content ? trf("Versions for {0}", {gameText(detail_->game)})
                   : b.minecraft.empty() ? tr("Versions") : trf("Versions for Minecraft {0}", {b.minecraft}),
                   274, 170, 18, muted);
        if (b.kind == ContentKind::Mod)
            draw_.text(tr("Required dependencies are installed automatically"), 760, 172, 15, muted, 490);
        SDL_Rect area{272, 202, 984, 464};
        if (b.versions.empty()) {
            empty(area, tr(b.versions_loading ? "Loading versions…" : "No matching versions"),
                  b.versions_loading ? "" : tr("The project has no files for the selected Minecraft version or loader."));
            return;
        }
        hint("A", tr(content ? "Install" : "Download"));
        grid("browse:versions", area, static_cast<int>(b.versions.size()), 1, 62, 8,
             [this, &b](int i, SDL_Rect r, bool focus) {
                 const auto &version = b.versions[i];
                 row(r, focus);
                 draw_.text(version.name, r.x + 18, r.y + 6, 19, foreground, r.w - 220);
                 std::string details;
                 for (auto loader : version.loaders)
                     details += (details.empty() ? "" : ", ") + loaderText(loader);
                 std::string games;
                 for (std::size_t g = 0; g < version.minecraft_versions.size() && g < 4; ++g)
                     games += (g ? ", " : "") + version.minecraft_versions[g];
                 if (version.minecraft_versions.size() > 4)
                     games += "…";
                 draw_.text(games + (details.empty() ? "" : " · " + details) + " · " + sizeText(version.archive.bytes), r.x + 18, r.y + 34, 14,
                            muted, r.w - 220);
                 auto date = version.published.substr(0, 10);
                 draw_.text(date, r.x + r.w - draw_.width(date, 15) - 18, r.y + 20, 15, muted);
             },
             [this, &b](int i) {
                 if (b.kind == ContentKind::Modpack)
                     downloadPack(b.versions[i]);
                 else
                     installContent(b.versions[i]);
             },
             [&b](int i) { return "version:" + b.versions[i].name; });
    }
    void filesScreen() {
        header(tr("Import from SD card"), tr("Modrinth .mrpack · CurseForge ZIP · MultiMC / PolyMC / Prism export"));
        button({272, 100, 170, 46}, tr("Up"), [this] {
            if (directory_ == browser_root_)
                return;
            auto parent = directory_.substr(0, directory_.find_last_of('/'));
            if (parent.size() < browser_root_.size())
                parent = browser_root_;
            browse(parent);
        }, directory_ != browser_root_);
        draw_.text(directory_, 460, 112, 17, muted, 790);
        SDL_Rect area{272, 160, 984, 506};
        if (files_.empty()) {
            empty(area, tr("No archives here"), tr("Copy a modpack (.mrpack or .zip) to the SD card and open its folder here."));
            return;
        }
        hint("A", tr("Open"));
        grid("files:" + directory_, area, static_cast<int>(files_.size()), 1, 54, 6,
             [this](int i, SDL_Rect r, bool focus) {
                 const auto &file = files_[i];
                 row(r, focus);
                 badge(r.x + 14, r.y + 13, tr(file.directory ? "Folder" : "Archive"), file.directory ? muted : accent, 13);
                 draw_.text(file.name, r.x + 100, r.y + 12, 19, foreground, r.w - 120);
             },
             [this](int i) {
                 auto file = files_[i];
                 if (file.directory)
                     browse(file.path);
                 else
                     inspect(file.path);
             },
             [this](int i) { return "file:" + files_[i].name; });
    }
    void inspectionScreen() {
        const auto &plan = *inspected_;
        header(tr("Install modpack"), archive_);
        draw_.rounded({272, 104, 984, 400}, 14, panel);
        if (!inspected_icon_.empty())
            remoteIcon(inspected_icon_, {300, 132, 96, 96}, 2);
        else
            pattern({300, 132, 96, 96}, info, 2);
        draw_.text(plan.name, 420, 130, 28, foreground, 800);
        draw_.text(gameText(plan.game) + (plan.version.empty() ? "" : " · " + trf("version {0}", {plan.version})), 422, 172, 18, muted, 800);
        int x = 422;
        x += badge(x, 204, countText(static_cast<long>(plan.downloads.size()), "file", "files", "файл", "файла", "файлов"), info) + 8;
        x += badge(x, 204, trf("{0} to download", {sizeText(plan.required_free_bytes)}), info) + 8;
        badge(x, 204, trf("{0} free", {sizeText(free_bytes_)}), free_bytes_ > plan.required_free_bytes ? accent : danger);
        int y = 260;
        if (plan.notices.empty())
            draw_.wrap(tr("The modpack is installed into its own folder on the SD card. Minecraft and loader files are prepared before the first launch."),
                       300, y, 18, muted, 920, 3);
        for (std::size_t i = 0; i < plan.notices.size() && i < 6; ++i, y += 34)
            draw_.text("- " + tr(plan.notices[i]), 300, y, 16, warning, 920);
        button({272, 524, 330, 56}, tr("Install"), [this] { import(); }, !task_.active && plan.state != InstanceState::Unsupported, false, true);
        button({618, 524, 300, 56}, tr(include_optional_ ? "Optional files: yes" : "Optional files: no"),
               [this] { include_optional_ = !include_optional_; });
        button({934, 524, 322, 56}, tr("Cancel"), [this] {
            inspected_.reset();
            focus_ = -1;
        });
    }
    void tasksScreen() {
        header(tr("Tasks"), tr("Downloads and preparation run in the background, so you can keep using the launcher"));
        draw_.rounded({272, 104, 984, 210}, 14, panel);
        if (task_.active) {
            auto p = task_.progress();
            draw_.text(task_.title, 300, 124, 26, foreground, 930);
            draw_.wrap(tr(p.detail.empty() ? "Please wait…" : p.detail), 300, 168, 18, muted, 930, 2);
            draw_.rounded({300, 236, 928, 10}, 5, border);
            if (p.total)
                draw_.rounded({300, 236, std::max(10, static_cast<int>(928 * std::min(1.0, p.completed / double(p.total)))), 10}, 5, accent);
            else if (p.files_total)
                draw_.rounded({300, 236, std::max(10, static_cast<int>(928 * std::min(1.0, p.files_done / double(p.files_total)))), 10}, 5,
                              accent);
            else
                draw_.rounded({300 + static_cast<int>((SDL_GetTicks() / 8) % 740), 236, 188, 10}, 5, accent);
            draw_.text(p.files_total ? trf("{0} / {1} files", {std::to_string(p.files_done), std::to_string(p.files_total)})
                       : p.stage == "assets" ? trf("{0} / {1} assets", {std::to_string(p.completed), std::to_string(p.total)})
                       : p.total ? sizeText(p.completed) + " / " + sizeText(p.total)
                                 : "",
                       300, 262, 17, muted);
            button({1036, 254, 196, 46}, tr(task_.cancelled ? "Cancelling…" : "Cancel task"), [this] { task_.cancelled = true; }, !task_.cancelled);
        } else if (processor_launch_) {
            draw_.text(tr("Loader preparation needed"), 300, 124, 26);
            draw_.wrap(tr("Files are downloaded. The next step runs the Forge/NeoForge installer and closes the app. Then open MineSwitch again and choose this instance."),
                       300, 168, 18, muted, 930, 3);
            button({300, 248, 300, 50}, tr("Run preparation"), [this] {
#ifdef __SWITCH__
                pending_launch_ = std::move(processor_launch_);
                processor_launch_.reset();
#else
                toast(tr("To prepare it, open this instance in MineSwitch on the console."), info);
#endif
            }, true, false, true);
            button({616, 248, 160, 50}, tr("Later"), [this] {
                processor_launch_.reset();
                changePage(Page::Library);
            });
        } else {
            draw_.text(tr("No active tasks"), 300, 124, 26);
            draw_.text(tr("Downloads, installs, preparation and copies are shown here"), 300, 168, 18, muted);
            button({300, 236, 220, 50}, tr("Go to library"), [this] { changePage(Page::Library); }, true, false, true);
        }
        draw_.text(tr("Recent"), 274, 332, 18, muted);
        SDL_Rect area{272, 362, 984, 304};
        if (history_.empty()) {
            draw_.text(tr("Nothing yet"), 274, 372, 17, muted);
            return;
        }
        grid("history", area, static_cast<int>(history_.size()), 1, 56, 6,
             [this](int i, SDL_Rect r, bool focus) {
                 const auto &entry = history_[i];
                 row(r, focus);
                 draw_.circle(r.x + 22, r.y + 28, 6, entry.ok ? accent : danger);
                 draw_.text(entry.title, r.x + 40, r.y + 6, 18, foreground, r.w - 180);
                 draw_.text(entry.result, r.x + 40, r.y + 32, 14, entry.ok ? muted : danger, r.w - 60);
                 auto when = agoText(entry.when);
                 draw_.text(when, r.x + r.w - draw_.width(when, 14) - 16, r.y + 8, 14, muted);
             },
             [this](int i) { openMenu(history_[i].title, history_[i].result, {{tr("Close"), [] {}}}); });
    }
    void settingsScreen() {
        std::string cpu = trf("CPU cores: {0}", {"?"});
#ifdef __SWITCH__
        u64 core_mask = 0;
        if (R_SUCCEEDED(svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
            char mask[32];
            std::snprintf(mask, sizeof(mask), "0x%llx", static_cast<unsigned long long>(core_mask));
            cpu = trf("CPU cores: {0} (mask {1})", {std::to_string(__builtin_popcountll(core_mask)), mask});
        }
#else
        cpu = trf("CPU cores: {0}", {std::to_string(SDL_GetCPUCount())});
#endif
        header(tr("Settings"), trf("{0} · memory {1} · profiles, Java, catalogs", {cpu, sizeText(heap_budget_.process_bytes)}));
        struct Item {
            std::string title, value;
            std::function<void()> call;
            bool section = false;
        };
        std::vector<Item> items;
        items.push_back({tr("Profiles"), "", {}, true});
        for (const auto &p : profiles_)
            items.push_back({p.username, tr(p.id == settings_.profile_id ? "Used for playing" : "Select"),
                             [this, p] { profileMenu(p); }});
        items.push_back({tr("+ Add profile"), tr("Offline profile: singleplayer and servers without account checks"), [this] {
                             edit(tr("New player name"), "", 16, [this](std::string name) {
                                 if (!usernameValid(name))
                                     throw Error("profile", "Name: 1–16 Latin letters, digits or _");
                                 Profile p{newId(), name};
                                 profiles_.push_back(p);
                                 storage_.saveProfiles(profiles_);
                                 settings_.profile_id = p.id;
                                 storage_.saveSettings(settings_);
                                 toast(trf("Profile {0} added and selected", {name}));
                             });
                         }});
        items.push_back({"Java", "", {}, true});
        items.push_back({trf("Default memory: {0} MB", {std::to_string(settings_.heap_mib)}),
                         trf("Maximum on this console: {0} MB", {std::to_string(heap_budget_.max_mib)}) +
                             (heap_budget_.process_bytes ? " · " + trf("{0} available to the app", {sizeText(heap_budget_.process_bytes)}) : ""),
                         [this] {
                             openMenu(tr("Default Java memory"), tr("Instances with their own value keep it"),
                                      heapChoices(settings_.heap_mib, false, [this](int size) {
                                          settings_.heap_mib = size;
                                          storage_.saveSettings(settings_);
                                      }));
                         }});
        items.push_back({tr(settings_.full_initial_heap ? "Initial memory (Xms): same as maximum" : "Initial memory (Xms): 256 MB"),
                         tr(settings_.full_initial_heap ? "All Java memory is reserved when the game starts"
                                                        : "The heap grows as needed up to the maximum"),
                         [this] {
                             settings_.full_initial_heap = !settings_.full_initial_heap;
                             storage_.saveSettings(settings_);
                         }});
        items.push_back({tr("JVM arguments for all instances"), settings_.jvm_args.empty() ? tr("None · -D…, -XX:… and -Xss… are allowed") : settings_.jvm_args, [this] {
                             edit(tr("JVM arguments"), settings_.jvm_args, 1000, [this](std::string text) {
                                 parseJvmArguments(text);
                                 settings_.jvm_args = std::move(text);
                                 storage_.saveSettings(settings_);
                                 toast(tr("JVM arguments saved"));
                             });
                         }});
        items.push_back({tr("Catalogs"), "", {}, true});
        items.push_back({tr(settings_.curseforge_key.empty() ? "CurseForge: key is fetched automatically" : "CurseForge: using your own key"),
                         tr("You can set your own API key instead of the shared one"), [this] {
                             edit(tr("CurseForge API key (empty for automatic)"), settings_.curseforge_key, 4096,
                                  [this](std::string key) {
                                      settings_.curseforge_key = std::move(key);
                                      storage_.saveSettings(settings_);
                                      toast(tr("CurseForge settings saved"));
                                  },
                                  true);
                         }});
        items.push_back({tr("Interface"), "", {}, true});
        // Bilingual title so the row is recognizable in either language.
        items.push_back({std::string("Language / Язык: ") + (language() == Language::Russian ? "Русский" : "English"), tr("Launcher interface language"),
                         [this] {
                             auto choose = [this](Language value) {
                                 settings_.language = languageCode(value);
                                 storage_.saveSettings(settings_);
                                 setLanguage(value);
                             };
                             openMenu("Language / Язык", "", {{"English", [choose] { choose(Language::English); }},
                                                             {"Русский", [choose] { choose(Language::Russian); }}});
                         }});
        items.push_back({tr(settings_.reduced_motion ? "Animations: off" : "Animations: on"), tr("Smooth list scrolling"), [this] {
                             settings_.reduced_motion = !settings_.reduced_motion;
                             storage_.saveSettings(settings_);
                         }});
        items.push_back({tr("Maintenance"), "", {}, true});
        items.push_back({tr("Clear cache"), tr("Catalog icons and downloaded modpack archives (installed instances are not affected)"), [this] { clearCache(); }});
        items.push_back({tr("About"), trf("MineSwitch · Java 21 HotSpot · storage {0}", {storage_.root()}),
                         [this] {
                             openMenu("MineSwitch", tr("Minecraft Java Edition launcher for Nintendo Switch. Java 21, LWJGL 3.3.3, NXVK + Zink. Microsoft sign-in is not supported yet."),
                                      {{tr("Close"), [] {}}});
                         }});
        hint("A", tr("Select"));
        grid("settings", {272, 100, 984, 566}, static_cast<int>(items.size()), 1, 62, 6,
             [this, items](int i, SDL_Rect r, bool focus) {
                 const auto &item = items[i];
                 if (item.section) {
                     draw_.text(item.title, r.x + 4, r.y + 26, 17, accent);
                     return;
                 }
                 row(r, focus);
                 draw_.text(item.title, r.x + 18, r.y + 6, 19, foreground, r.w - 40);
                 draw_.text(item.value, r.x + 18, r.y + 34, 14, muted, r.w - 40);
             },
             [items](int i) {
                 if (items[i].call)
                     items[i].call();
             },
             [items](int i) { return "setting:" + items[i].title; });
    }
    void profileMenu(const Profile &p) {
        std::vector<MenuItem> items{{tr("Use"), [this, p] {
                                         settings_.profile_id = p.id;
                                         storage_.saveSettings(settings_);
                                     }},
                                    {tr("Rename"), [this, p] {
                                         edit(tr("Player name (A–Z, 0–9, _)"), p.username, 16, [this, p](std::string value) {
                                             if (!usernameValid(value))
                                                 throw Error("profile", "Name: 1–16 Latin letters, digits or _");
                                             for (auto &item : profiles_)
                                                 if (item.id == p.id)
                                                     item.username = value;
                                             storage_.saveProfiles(profiles_);
                                         });
                                     }}};
        if (profiles_.size() > 1)
            items.push_back({tr("Delete"), [this, p] {
                                 profiles_.erase(std::remove_if(profiles_.begin(), profiles_.end(), [&](const Profile &x) { return x.id == p.id; }),
                                                 profiles_.end());
                                 storage_.saveProfiles(profiles_);
                                 if (settings_.profile_id == p.id) {
                                     settings_.profile_id = profiles_.front().id;
                                     storage_.saveSettings(settings_);
                                 }
                             },
                             true});
        openMenu(p.username, tr("Local profile"), std::move(items));
    }
    void clearCache() {
        task(tr("Clearing cache"), [this](ProgressCallback, const std::atomic_bool *) -> Apply {
            std::uint64_t removed = 0;
            for (const char *folder : {"cache/icons", "cache/pack-archives"}) {
                auto path = joinPath(storage_.root(), folder);
                if (!pathExists(path))
                    continue;
                std::function<void(const std::string &)> count = [&](const std::string &p) {
                    struct stat st {};
                    if (::lstat(p.c_str(), &st) != 0)
                        return;
                    if (!S_ISDIR(st.st_mode)) {
                        removed += static_cast<std::uint64_t>(st.st_size);
                        return;
                    }
                    if (DIR *d = ::opendir(p.c_str())) {
                        while (auto *e = ::readdir(d)) {
                            std::string n = e->d_name;
                            if (n != "." && n != "..")
                                count(p + "/" + n);
                        }
                        ::closedir(d);
                    }
                };
                count(path);
                removeOwnedTree(path);
            }
            return [this, removed] {
                images_.clear();
                refresh();
                toast(trf("Freed {0}", {sizeText(removed)}));
            };
        });
    }
    void wizardScreen() {
        auto &w = *wizard_;
        draw_.text(w.editing ? trf("Version and loader · {0}", {w.target.name}) : tr("New instance"), 272, 26, 32, foreground, 820);
        const char *filters[] = {"Releases", "Snapshots", "All"};
        for (int i = 0; i < 3; ++i)
            button({272 + i * 158, 94, 150, 44}, tr(filters[i]), [this, i] { wizard_->filter = i; }, true, w.filter == i, false, 18);
        std::vector<int> shown;
        for (int i = 0; i < static_cast<int>(w.versions.size()); ++i) {
            const auto &v = w.versions[i];
            if ((w.filter == 0 && v.type == "release") || (w.filter == 1 && v.type == "snapshot") || w.filter == 2)
                shown.push_back(i);
        }
        SDL_Rect left{272, 150, 470, 446};
        // Registered after the filters so the first focus lands on them.
        if (w.versions.empty()) {
            empty(left, tr(w.versions_loading ? "Loading versions…" : "List unavailable"),
                  w.versions_error.empty() ? "" : w.versions_error + " " + tr("You can enter the version manually."));
        } else
            grid("wizard:versions", left, static_cast<int>(shown.size()), 1, 52, 6,
                 [this, shown](int i, SDL_Rect r, bool focus) {
                     const auto &v = wizard_->versions[shown[i]];
                     bool selected = v.id == wizard_->minecraft;
                     row(r, focus, selected ? raised : panel);
                     if (selected)
                         draw_.rounded({r.x + 6, r.y + 12, 4, r.h - 24}, 2, accent);
                     draw_.text(v.id, r.x + 20, r.y + 12, 19, foreground, 180);
                     draw_.text(v.released.substr(0, 10), r.x + 200, r.y + 15, 15, muted);
                     bool ok = v.java_major == 21;
                     badge(r.x + r.w - (ok ? 92 : 132), r.y + 12, ok ? "Java 21" : trf("Java {0} · no", {std::to_string(v.java_major)}), ok ? accent : muted, 13);
                 },
                 [this, shown](int i) {
                     wizard_->minecraft = wizard_->versions[shown[i]].id;
                     loadLoaderVersions();
                 },
                 [this, shown](int i) { return "mc:" + wizard_->versions[shown[i]].id; });
        button({272, 606, 470, 46}, tr("Enter version manually"), [this] {
            edit(tr("Minecraft version"), wizard_->minecraft.empty() ? "1.21.1" : wizard_->minecraft, 40, [this](std::string version) {
                if (version.empty() || version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != std::string::npos)
                    throw Error("version", "Enter a Minecraft version number, for example 1.21.1");
                wizard_->minecraft = version;
                loadLoaderVersions();
            });
        });
        int x = 760;
        if (!w.editing)
            button({x, 94, 496, 44}, w.name.empty() ? tr("Name: from version") : trf("Name: {0}", {w.name}), [this] {
                edit(tr("Instance name"), wizard_->name, 120, [this](std::string name) { wizard_->name = std::move(name); });
            });
        else
            draw_.text(trf("Current: {0}", {gameText(w.target.game)}), x, 108, 16, muted, 496);
        const LoaderKind loaders[] = {LoaderKind::Vanilla, LoaderKind::Fabric, LoaderKind::Quilt, LoaderKind::Forge, LoaderKind::NeoForge};
        for (int i = 0; i < 5; ++i) {
            auto kind = loaders[i];
            button({x + (i % 3) * 168, 152 + (i / 3) * 52, 160, 44}, loaderText(kind), [this, kind] {
                wizard_->loader = kind;
                wizard_->loader_version.clear();
                loadLoaderVersions();
            }, true, w.loader == kind);
        }
        SDL_Rect right{x, 260, 496, 336};
        if (w.loader == LoaderKind::Vanilla)
            empty(right, tr("No loader"), tr("Plain Minecraft. You can add a loader later in the instance settings."));
        else if (w.loader_versions.empty())
            empty(right, tr(w.loader_loading ? "Loading loader versions…" : "No versions"),
                  w.loader_error.empty() ? tr("The loader is not available for this Minecraft version.") : w.loader_error);
        else
            grid("wizard:loader", right, static_cast<int>(w.loader_versions.size()), 1, 46, 6,
                 [this](int i, SDL_Rect r, bool focus) {
                     const auto &v = wizard_->loader_versions[i];
                     bool selected = v.version == wizard_->loader_version;
                     row(r, focus, selected ? raised : panel);
                     if (selected)
                         draw_.rounded({r.x + 6, r.y + 10, 4, r.h - 20}, 2, accent);
                     draw_.text(v.version, r.x + 20, r.y + 9, 18, foreground, r.w - 140);
                     if (!v.stable)
                         badge(r.x + r.w - 100, r.y + 10, tr("beta"), warning, 13);
                 },
                 [this](int i) { wizard_->loader_version = wizard_->loader_versions[i].version; },
                 [this](int i) { return "loader:" + wizard_->loader_versions[i].version; });
        std::string summary = w.minecraft.empty() ? tr("No version selected")
                                                  : "Minecraft " + w.minecraft + " · " + loaderText(w.loader) +
                                                        (w.loader == LoaderKind::Vanilla || w.loader_version.empty() ? "" : " " + w.loader_version);
        draw_.text(summary, x, 618, 16, muted, 250);
        button({x + 266, 606, 230, 46}, tr(w.editing ? "Apply" : "Create"), [this] { finishWizard(); }, !w.minecraft.empty(), false, true);
        button({1104, 26, 152, 44}, tr("Cancel"), [this] {
            wizard_.reset();
            focus_ = -1;
        });
        hint("A", tr("Select"));
    }
    void viewerScreen() {
        draw_.box({0, 0, 1280, 720}, {0, 0, 0, 255});
        if (shots_.empty()) {
            viewer_.reset();
            return;
        }
        int index = std::clamp(*viewer_, 0, static_cast<int>(shots_.size()) - 1);
        viewer_ = index;
        const auto &shot = shots_[index];
        if (auto *texture = images_.get(shot.path, 1280, 720))
            draw_.image(texture, {0, 0, 1280, 720});
        else
            draw_.text(tr("Loading…"), 580, 340, 22, muted);
        draw_.box({0, 0, 1280, 52}, {0, 0, 0, 150});
        draw_.text(shot.name, 24, 10, 19, foreground, 900);
        auto counter = std::to_string(index + 1) + " / " + std::to_string(shots_.size());
        draw_.text(counter, 1256 - draw_.width(counter, 18), 12, 18, muted);
        // Preload neighbours so switching is immediate.
        if (index + 1 < static_cast<int>(shots_.size()))
            images_.get(shots_[index + 1].path, 1280, 720);
        if (index > 0)
            images_.get(shots_[index - 1].path, 1280, 720);
        hints_.clear();
        hint("L/R", tr("Browse"));
        hint("X", tr("Delete"));
        hint("B", tr("Close"));
    }
    void logScreen() {
        auto &log = *log_;
        draw_.box({0, 0, 1280, 720}, background);
        draw_.text(logName(log.title), 24, 14, 22, foreground, 900);
        auto status = countText(static_cast<long>(log.lines.size()), "line", "lines", "строка", "строки", "строк");
        draw_.text(status, 1256 - draw_.width(status, 15), 20, 15, muted);
        const int visible = 30, line_h = 20;
        log.top = std::clamp(log.top, 0, std::max(0, static_cast<int>(log.lines.size()) - visible));
        draw_.rounded({16, 56, 1248, visible * line_h + 16}, 10, panel);
        for (int i = 0; i < visible && log.top + i < static_cast<int>(log.lines.size()); ++i) {
            auto line = log.lines[log.top + i];
            std::size_t cut = 0;
            for (int s = 0; s < log.shift && cut < line.size(); ++s) {
                ++cut;
                while (cut < line.size() && (static_cast<unsigned char>(line[cut]) & 192) == 128)
                    ++cut;
            }
            line = line.substr(cut);
            Color color = line.find("ERROR") != std::string::npos || line.find("Exception") != std::string::npos ? danger
                          : line.find("WARN") != std::string::npos                                                ? warning
                                                                                                                  : foreground;
            draw_.text(line, 28, 64 + i * line_h, 14, color, 1224);
        }
        hints_.clear();
        hint("D-pad", tr("Scroll"));
        hint("L/R", tr("Pages"));
        hint("Y", tr("To end"));
        hint("B", tr("Close"));
    }
    void modalScreen() {
        auto &m = *modal_;
        actions_.clear();
        draw_.box({0, 0, 1280, 720}, {0, 0, 0, 170});
        int count = static_cast<int>(m.items.size());
        int text_h = m.text.empty() ? 0 : 76;
        int h = 96 + text_h + count * 56;
        h = std::min(h, 660);
        SDL_Rect box{340, (720 - h) / 2, 600, h};
        draw_.rounded(box, 16, raised);
        draw_.text(m.title, box.x + 28, box.y + 22, 24, foreground, box.w - 56);
        if (!m.text.empty())
            draw_.wrap(m.text, box.x + 28, box.y + 64, 16, muted, box.w - 56, 3);
        SDL_Rect list{box.x + 24, box.y + 76 + text_h, box.w - 48, box.h - 96 - text_h};
        grid("modal:" + m.title, list, count, 1, 48, 8,
             [this, &m](int i, SDL_Rect r, bool focus) {
                 const auto &item = m.items[i];
                 draw_.rounded(r, 10, focus ? (item.danger ? Color{120, 50, 50} : panel) : background);
                 if (focus)
                     draw_.roundedOutline({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 12, item.danger ? danger : accent, 3);
                 draw_.text(item.label, r.x + 18, r.y + 9, 19, item.danger ? danger : item.enabled ? foreground : muted, r.w - 36);
             },
             [this](int i) {
                 auto call = modal_->items[i].call;
                 if (!modal_->items[i].enabled)
                     return;
                 closeMenu();
                 if (call)
                     call();
             },
             [&m](int i) { return "menu:" + m.items[i].label; });
        hints_.clear();
        hint("A", tr("Select"));
        hint("B", tr("Close"));
    }
    void toastsAndFooter() {
        draw_.box({240, 672, 1040, 48}, {10, 17, 26});
        int x = 272;
        for (const auto &h : hints_) {
            int w = std::max(28, draw_.width(h.first, 14) + 12);
            draw_.rounded({x, 682, w, 28}, 14, raised);
            label(h.first, x + (w - draw_.width(h.first, 14)) / 2, 696, 14, foreground);
            label(h.second, x + w + 8, 696, 16, muted);
            x += w + draw_.width(h.second, 16) + 24;
        }
        auto right = task_.active ? task_.title : tr("+  Exit");
        draw_.text(right, 1256 - std::min(380, draw_.width(right, 15)), 686, 15, task_.active ? accent : muted, 380);
        Uint32 now = SDL_GetTicks();
        while (!toasts_.empty() && toasts_.front().until < now)
            toasts_.pop_front();
        int y = 612;
        for (auto it = toasts_.rbegin(); it != toasts_.rend() && y > 480; ++it) {
            const auto &t = *it;
            int w = std::min(640, draw_.width(t.text, 17) + 44);
            SDL_Rect r{1260 - w, y, w, 48};
            draw_.rounded(r, 12, {38, 54, 72, 250});
            draw_.rounded({r.x, r.y, 6, r.h}, 3, t.color);
            label(t.text, r.x + 22, r.y + 24, 17, foreground, w - 34);
            y -= 56;
        }
    }
    void render() {
        actions_.clear();
        lists_.clear();
        hints_.clear();
        group_counter_ = 0;
        images_.pump();
        draw_.clear();
        if (viewer_ && detail_ && tab_ == Tab::Screenshots)
            viewerScreen();
        else if (log_)
            logScreen();
        else {
            sidebarPanel();
            if (wizard_)
                wizardScreen();
            else if (inspected_)
                inspectionScreen();
            else if (detail_ && browsing_content_)
                browseScreen();
            else if (detail_)
                instancePage();
            else
                switch (page_) {
                case Page::Library: library(); break;
                case Page::Catalog: browseScreen(); break;
                case Page::Files: filesScreen(); break;
                case Page::Tasks: tasksScreen(); break;
                case Page::Settings: settingsScreen(); break;
                }
            hint("B", tr("Back"));
        }
        if (modal_)
            modalScreen();
        toastsAndFooter();
        if (focus_ < 0) {
            // Prefer the first list item of the page, then its first control.
            focus_ = !modal_ && content_start_ < static_cast<int>(actions_.size()) ? content_start_ : 0;
            for (int i = focus_; i < static_cast<int>(actions_.size()); ++i)
                if (actions_[i].group >= 0 && actions_[i].enabled) {
                    focus_ = i;
                    break;
                }
        }
        if (focus_ >= static_cast<int>(actions_.size()))
            focus_ = std::max(0, static_cast<int>(actions_.size()) - 1);
        if (editing_) {
            draw_.box({0, 0, 1280, 720}, {0, 0, 0, 190});
            draw_.rounded({218, 206, 844, 303}, 16, raised);
            draw_.text(dialog_title_, 251, 239, 26, foreground, 774);
            draw_.rounded({250, 296, 780, 72}, 10, background);
            draw_.text(secret_ ? std::string(std::min<std::size_t>(dialog_.size(), 42), '*') : dialog_, 271, 316, 24, foreground, 736);
            draw_.text(tr("Enter: save    Esc: cancel"), 251, 414, 21, muted);
        }
    }

    // ----------------------------------------------------------------- input
    void changePage(Page page) {
        page_ = page;
        closeDetail();
        inspected_.reset();
        wizard_.reset();
        focus_ = -1;
        if (page == Page::Files && directory_.empty())
            browse(browser_root_);
        if (page == Page::Catalog)
            packs_browse_.project.reset();
    }
    void browse(const std::string &path) {
        requireSafePath(path);
        DIR *raw = ::opendir(path.c_str());
        if (!raw)
            throw Error("browser", "Could not open the folder");
        std::unique_ptr<DIR, int (*)(DIR *)> directory(raw, ::closedir);
        std::vector<FileEntry> entries;
        while (auto *entry = ::readdir(directory.get())) {
            std::string name = entry->d_name;
            if (name.empty() || name[0] == '.' || !validRelativePath(name))
                continue;
            auto full = path.back() == '/' ? path + name : path + "/" + name;
            struct stat st {};
            if (::lstat(full.c_str(), &st))
                continue;
            bool folder = S_ISDIR(st.st_mode);
            auto suffix = name.substr(name.find_last_of('.') == std::string::npos ? name.size() : name.find_last_of('.'));
            std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
            if (folder || (S_ISREG(st.st_mode) && (suffix == ".zip" || suffix == ".mrpack")))
                entries.push_back({name, full, folder});
            if (entries.size() >= 4096)
                break;
        }
        std::sort(entries.begin(), entries.end(),
                  [](const FileEntry &a, const FileEntry &b) { return a.directory != b.directory ? a.directory > b.directory : a.name < b.name; });
        directory_ = path;
        files_ = std::move(entries);
        focus_ = -1;
    }
    void move(int dx, int dy) {
        if (actions_.empty())
            return;
        if (focus_ < 0 || focus_ >= static_cast<int>(actions_.size()))
            focus_ = 0;
        const auto &current = actions_[focus_];
        auto r = current.rect;
        double best = 1e20;
        int selected = focus_;
        for (std::size_t i = 0; i < actions_.size(); ++i) {
            const auto &candidate = actions_[i];
            if (!candidate.enabled || static_cast<int>(i) == focus_)
                continue;
            SDL_Rect visible;
            // Hidden list rows are reachable only from inside their own list.
            if (!SDL_IntersectRect(&candidate.rect, &candidate.clip, &visible) && (candidate.group < 0 || candidate.group != current.group))
                continue;
            auto c = candidate.rect;
            int x = c.x + c.w / 2 - r.x - r.w / 2, y = c.y + c.h / 2 - r.y - r.h / 2;
            if ((dx && x * dx <= 0) || (dy && y * dy <= 0))
                continue;
            // Prefer overlap on the perpendicular axis so rows and columns line up.
            bool aligned = dx ? (c.y < r.y + r.h && r.y < c.y + c.h) : (c.x < r.x + r.w && r.x < c.x + c.w);
            double score = dx ? std::abs(x) + std::abs(y) * 3.5 : std::abs(y) + std::abs(x) * 3.5;
            if (!aligned)
                score += 400;
            if (score < best) {
                best = score;
                selected = static_cast<int>(i);
            }
        }
        focus_ = selected;
    }
    void page(int direction) {
        for (int i = 0; i < 4; ++i)
            move(0, direction);
    }
    void activate(int index) {
        if (index >= 0 && index < static_cast<int>(actions_.size()) && actions_[index].enabled) {
            auto call = actions_[index].call;
            call();
        }
    }
    void back() {
        if (editing_) {
            editing_ = false;
            SDL_StopTextInput();
            dialog_.clear();
            edit_done_ = {};
        } else if (modal_)
            closeMenu();
        else if (viewer_)
            viewer_.reset();
        else if (log_)
            log_.reset();
        else if (wizard_)
            wizard_.reset();
        else if (inspected_)
            inspected_.reset();
        else if (detail_ && browsing_content_ && content_browse_.project)
            content_browse_.project.reset();
        else if (detail_ && browsing_content_)
            browsing_content_ = false;
        else if (detail_)
            closeDetail();
        else if (page_ == Page::Catalog && packs_browse_.project)
            packs_browse_.project.reset();
        else if (page_ == Page::Files && directory_ != browser_root_) {
            auto parent = directory_.substr(0, directory_.find_last_of('/'));
            browse(parent.size() < browser_root_.size() ? browser_root_ : parent);
            return;
        } else if (page_ != Page::Library)
            changePage(Page::Library);
        focus_ = modal_ ? focus_ : -1;
    }
    // X: context menu for the focused item.
    void secondary() {
        if (modal_ || focus_ < 0 || focus_ >= static_cast<int>(actions_.size()))
            return;
        if (viewer_)
            return deleteShot(*viewer_);
        const auto &label = actions_[focus_].label;
        auto index = [&](const std::string &prefix) { return label.compare(0, prefix.size(), prefix) == 0; };
        if (index("instance:")) {
            for (const auto &item : instances_)
                if ("instance:" + item.name == label)
                    return instanceMenu(item);
        } else if (index("mod:")) {
            for (const auto &mod : mods_)
                if ("mod:" + mod.filename == label)
                    return modMenu(mod);
        } else if (index("pack:")) {
            for (const auto &pack : packs_)
                if ("pack:" + pack.filename == label)
                    return packMenu(pack, tabKind(tab_));
        } else if (index("shot:")) {
            for (std::size_t i = 0; i < shots_.size(); ++i)
                if ("shot:" + shots_[i].name == label)
                    return deleteShot(static_cast<int>(i));
        } else if (index("world:") || index("server:")) {
            activate(focus_);
        }
    }
    // Y: quick action for the current screen.
    void tertiary() {
        if (log_) {
            log_->top = static_cast<int>(log_->lines.size());
            return;
        }
        if (modal_ || wizard_)
            return;
        if (focus_ >= 0 && focus_ < static_cast<int>(actions_.size()) && actions_[focus_].label.compare(0, 9, "instance:") == 0) {
            for (const auto &item : instances_)
                if ("instance:" + item.name == actions_[focus_].label)
                    return play(item);
        }
        if ((page_ == Page::Catalog && !detail_) || (detail_ && browsing_content_))
            edit(tr("Search"), browse().query, 128, [this](std::string s) {
                browse().query = std::move(s);
                search();
            });
    }
    void shoulder(int direction) {
        if (log_) {
            log_->top += direction * 28;
            return;
        }
        if (viewer_) {
            viewer_ = std::clamp(*viewer_ + direction, 0, static_cast<int>(shots_.size()) - 1);
            return;
        }
        if (modal_ || wizard_)
            return;
        if (detail_ && !browsing_content_) {
            selectTab(static_cast<Tab>((static_cast<int>(tab_) + tab_count + direction) % tab_count));
            return;
        }
        changePage(static_cast<Page>((static_cast<int>(page_) + 5 + direction) % 5));
    }
    void pad(SDL_GameControllerButton key) {
        if (log_) {
            switch (key) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: log_->top -= 1; return;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: log_->top += 1; return;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: log_->shift = std::max(0, log_->shift - 40); return;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: log_->shift += 40; return;
            default: break;
            }
        }
        if (viewer_ && (key == SDL_CONTROLLER_BUTTON_DPAD_LEFT || key == SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            return shoulder(key == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
        switch (key) {
        case SDL_CONTROLLER_BUTTON_A:
            if (viewer_)
                break;
            activate(focus_);
            break;
        case SDL_CONTROLLER_BUTTON_B: back(); break;
        case SDL_CONTROLLER_BUTTON_X: secondary(); break;
        case SDL_CONTROLLER_BUTTON_Y: tertiary(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move(-1, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move(1, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(0, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(0, 1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: shoulder(-1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: shoulder(1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: page(-1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: page(1); break;
        case SDL_CONTROLLER_BUTTON_START:
            if (task_.active) {
                exit_requested_ = true;
                task_.cancelled = true;
                query_.cancelled = true;
                changePage(Page::Tasks);
            } else
                quit_ = true;
            break;
        default: break;
        }
    }
    void openControllers() {
#ifndef __SWITCH__
        for (auto *c : controllers_)
            SDL_GameControllerClose(c);
        controllers_.clear();
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i)) {
                auto *c = SDL_GameControllerOpen(i);
                if (c)
                    controllers_.push_back(c);
            }
#endif
    }
    ListArea *listAt(SDL_Point point) {
        for (auto it = lists_.rbegin(); it != lists_.rend(); ++it)
            if (SDL_PointInRect(&point, &it->area))
                return &*it;
        return nullptr;
    }
    void tap(SDL_Point point) {
        for (std::size_t i = actions_.size(); i-- > 0;) {
            const auto &action = actions_[i];
            if (SDL_PointInRect(&point, &action.rect) && SDL_PointInRect(&point, &action.clip)) {
                focus_ = static_cast<int>(i);
                activate(static_cast<int>(i));
                break;
            }
        }
    }
    void events() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit_ = true;
                task_.cancelled = true;
                query_.cancelled = true;
            } else if (editing_) {
                if (event.type == SDL_TEXTINPUT && dialog_.size() + std::strlen(event.text.text) <= edit_limit_)
                    dialog_ += event.text.text;
                if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_RETURN)
                        acceptEdit();
                    else if (event.key.keysym.sym == SDLK_ESCAPE)
                        back();
                    else if (event.key.keysym.sym == SDLK_BACKSPACE && !dialog_.empty()) {
                        auto n = dialog_.size() - 1;
                        while (n && (static_cast<unsigned char>(dialog_[n]) & 192) == 128)
                            --n;
                        dialog_.resize(n);
                    }
                }
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_UP: pad(SDL_CONTROLLER_BUTTON_DPAD_UP); break;
                case SDLK_DOWN: pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN); break;
                case SDLK_LEFT: pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT); break;
                case SDLK_RIGHT: pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); break;
                case SDLK_RETURN: pad(SDL_CONTROLLER_BUTTON_A); break;
                case SDLK_ESCAPE: pad(SDL_CONTROLLER_BUTTON_B); break;
                case SDLK_x: pad(SDL_CONTROLLER_BUTTON_X); break;
                case SDLK_y: pad(SDL_CONTROLLER_BUTTON_Y); break;
                case SDLK_PAGEUP: page(-1); break;
                case SDLK_PAGEDOWN: page(1); break;
                case SDLK_TAB: shoulder((event.key.keysym.mod & KMOD_SHIFT) ? -1 : 1); break;
                default: break;
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                // SDL_RenderSetLogicalSize already scales mouse events.
                tap({event.button.x, event.button.y});
            } else if (event.type == SDL_MOUSEWHEEL) {
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                float sx = 0, sy = 0;
                SDL_RenderWindowToLogical(draw_.renderer(), mx, my, &sx, &sy);
                if (log_)
                    log_->top -= event.wheel.y * 3;
                else if (auto *list = listAt({static_cast<int>(sx), static_cast<int>(sy)}))
                    scroll_[list->id].target -= event.wheel.y * 80.0f;
            } else if (event.type == SDL_FINGERDOWN) {
                touch_start_ = {static_cast<int>(event.tfinger.x * 1280), static_cast<int>(event.tfinger.y * 720)};
                touch_dragged_ = false;
                auto *list = listAt(touch_start_);
                touch_list_ = list ? list->id : std::string();
            } else if (event.type == SDL_FINGERMOTION) {
                int dy = static_cast<int>(event.tfinger.dy * 720);
                SDL_Point now{static_cast<int>(event.tfinger.x * 1280), static_cast<int>(event.tfinger.y * 720)};
                if (std::abs(now.y - touch_start_.y) > 14 || std::abs(now.x - touch_start_.x) > 14)
                    touch_dragged_ = true;
                if (touch_dragged_ && !touch_list_.empty()) {
                    auto &state = scroll_[touch_list_];
                    state.target -= static_cast<float>(dy);
                    state.current = state.target;
                }
            } else if (event.type == SDL_FINGERUP) {
                if (!touch_dragged_) {
                    if (viewer_) {
                        int x = static_cast<int>(event.tfinger.x * 1280);
                        x < 320 ? shoulder(-1) : x > 960 ? shoulder(1) : back();
                    } else
                        tap(touch_start_);
                }
            }
#ifndef __SWITCH__
            else if (event.type == SDL_CONTROLLERBUTTONDOWN)
                pad(static_cast<SDL_GameControllerButton>(event.cbutton.button));
            else if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED)
                openControllers();
#endif
        }
#ifndef __SWITCH__
        if (!editing_ && SDL_GetTicks() >= next_axis_)
            for (auto *c : controllers_) {
                int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
                if (std::abs(x) > 18000 || std::abs(y) > 18000) {
                    if (std::abs(x) > std::abs(y))
                        pad(x > 0 ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                    else
                        pad(y > 0 ? SDL_CONTROLLER_BUTTON_DPAD_DOWN : SDL_CONTROLLER_BUTTON_DPAD_UP);
                    next_axis_ = SDL_GetTicks() + 160;
                    break;
                }
            }
#endif
    }
    void finished(Job &job, bool heavy) {
        Apply apply;
        std::string error, code;
        auto title = job.title;
        if (!job.take(apply, error, code))
            return;
        if (!heavy && queued_query_) {
            auto next = std::move(*queued_query_);
            queued_query_.reset();
            query_.start(next.first, std::move(next.second));
            return; // A superseded query's result is stale.
        }
        if (!error.empty()) {
            // Cancelled queries were replaced on purpose; nothing to report.
            if (!heavy && (code == "cancelled" || code == "catalog.cancelled"))
                return;
            toast(error, danger);
            if (heavy)
                remember(title, error, false);
            packs_browse_.loading = content_browse_.loading = false;
            packs_browse_.versions_loading = content_browse_.versions_loading = false;
            tab_loading_ = false;
            return;
        }
        if (apply)
            apply();
        if (heavy && !pending_launch_ && !processor_launch_ && page_ == Page::Tasks && !task_.active)
            remember(title, tr("Done"), true);
    }

  public:
    Application(const std::string &root, bool preview, int heap_override = 0)
        : storage_(root), images_(draw_.renderer(), joinPath(root, "cache/icons")), preview_(preview) {
        storage_.initialize();
        storage_.cleanupTransactions();
        settings_ = storage_.loadSettings();
        setLanguage(parseLanguage(settings_.language));
        if (!heap_override && settings_.heap_mib > heap_budget_.max_mib) {
            settings_.heap_mib = heap_budget_.max_mib;
            storage_.saveSettings(settings_);
            toast(trf("Java memory was limited to the available maximum: {0} MB", {std::to_string(heap_budget_.max_mib)}), info);
        }
        if (heap_override) {
            if (heap_override < 256 || heap_override > heap_budget_.max_mib)
                throw Error("prepare_heap", "Java maximum in this mode: " + std::to_string(heap_budget_.max_mib) + " MB");
            settings_.heap_mib = heap_override;
            storage_.saveSettings(settings_);
#ifdef __SWITCH__
            const auto line = "MINESWITCH_SETTINGS heap_mib=" + std::to_string(heap_override) + " persisted=1";
            mineswitch_frontend_diagnostic(line.c_str());
#endif
        }
        profiles_ = storage_.loadProfiles();
        if (profiles_.empty()) {
            profiles_.push_back({newId(), "Player"});
            storage_.saveProfiles(profiles_);
        }
        if (settings_.profile_id.empty()) {
            settings_.profile_id = profiles_.front().id;
            storage_.saveSettings(settings_);
        }
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        refresh();
#ifdef __SWITCH__
        browser_root_ = "sdmc:/";
#else
        browser_root_ = joinPath(storage_.root(), "imports");
        createDirectories(browser_root_);
#endif
        if (preview_) {
            instances_.clear();
            const char *names[] = {"Minecraft 1.21.1", "With friends", "Fabulously Optimized", "New instance"};
            for (int i = 0; i < 4; ++i) {
                Instance demo;
                demo.id = "preview" + std::to_string(i);
                demo.name = tr(names[i]);
                demo.game.minecraft_version = "1.21.1";
                demo.game.loader.kind = i == 0 || i == 3 ? LoaderKind::Vanilla : LoaderKind::Fabric;
                demo.game.loader.version = i == 0 || i == 3 ? "" : "0.16.5";
                demo.state = i == 3 ? InstanceState::NeedsPreparation : InstanceState::Ready;
                demo.last_played = i == 3 ? 0 : static_cast<std::int64_t>(std::time(nullptr)) - i * 7200 - 300;
                demo.icon = "preset:" + std::to_string(i);
                instances_.push_back(demo);
            }
            profiles_.front().username = "Player";
        }
        openControllers();
    }
    ~Application() {
        for (auto *c : controllers_)
            SDL_GameControllerClose(c);
    }
#ifdef MINESWITCH_UI_TEST
    void smokeTest(const std::string &archive, const std::string &screenshot);
    void showcase(const std::string &directory);
#endif
    void frame() {
        finished(task_, true);
        finished(query_, false);
        if (exit_requested_ && !task_.active && !query_.active) {
            quit_ = true;
            return;
        }
        render();
    }
    std::optional<LaunchPlan> run(const std::string &screenshot = {}, unsigned frames = 0) {
#ifdef __SWITCH__
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        PadState input;
        padInitializeDefault(&input);
#endif
        unsigned count = 0;
        while (!quit_ && !pending_launch_) {
#ifdef __SWITCH__
            if (!appletMainLoop()) {
                quit_ = true;
                task_.cancelled = true;
                query_.cancelled = true;
                break;
            }
#endif
            try {
                frame();
                if (quit_)
                    break;
                events();
#ifdef __SWITCH__
                padUpdate(&input);
                auto keys = padGetButtonsDown(&input);
                const std::pair<u64, SDL_GameControllerButton> mapping[] = {
                    {HidNpadButton_A, SDL_CONTROLLER_BUTTON_A},
                    {HidNpadButton_B, SDL_CONTROLLER_BUTTON_B},
                    {HidNpadButton_X, SDL_CONTROLLER_BUTTON_X},
                    {HidNpadButton_Y, SDL_CONTROLLER_BUTTON_Y},
                    {HidNpadButton_Up, SDL_CONTROLLER_BUTTON_DPAD_UP},
                    {HidNpadButton_Down, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
                    {HidNpadButton_Left, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
                    {HidNpadButton_Right, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
                    {HidNpadButton_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
                    {HidNpadButton_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
                    {HidNpadButton_ZL, SDL_CONTROLLER_BUTTON_LEFTSTICK},
                    {HidNpadButton_ZR, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
                    {HidNpadButton_Plus, SDL_CONTROLLER_BUTTON_START}};
                for (const auto &m : mapping)
                    if (keys & m.first)
                        pad(m.second);
                auto stick = padGetStickPos(&input, 0);
                if (SDL_GetTicks() >= next_axis_ && (std::abs(stick.x) > 18000 || std::abs(stick.y) > 18000)) {
                    if (std::abs(stick.x) > std::abs(stick.y))
                        pad(stick.x > 0 ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                    else
                        pad(stick.y > 0 ? SDL_CONTROLLER_BUTTON_DPAD_UP : SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                    next_axis_ = SDL_GetTicks() + 160;
                }
                // Right stick scrolls the list under the focus continuously.
                auto right = padGetStickPos(&input, 1);
                if (std::abs(right.y) > 9000 && focus_ >= 0 && focus_ < static_cast<int>(actions_.size())) {
                    SDL_Point centre{actions_[focus_].rect.x + actions_[focus_].rect.w / 2, actions_[focus_].rect.y + actions_[focus_].rect.h / 2};
                    if (log_)
                        log_->top -= right.y / 9000;
                    else if (auto *list = listAt(centre))
                        scroll_[list->id].target -= right.y / 1500.0f;
                }
#endif
                if (!screenshot.empty() && (!frames || count + 1 >= frames))
                    draw_.screenshot(screenshot);
                draw_.present();
            } catch (const Error &e) {
                toast(errorText(e.code, e.what()), danger);
            } catch (const std::exception &e) {
                toast(tr(e.what()), danger);
            }
            ++count;
            if (frames && count >= frames)
                break;
            SDL_Delay(16);
        }
        return quit_ ? std::nullopt : pending_launch_;
    }

    // Test hooks
    bool clickLabel(const std::string &label) {
        render();
        for (std::size_t i = 0; i < actions_.size(); ++i)
            if (actions_[i].label == label && actions_[i].enabled) {
                focus_ = static_cast<int>(i);
                activate(static_cast<int>(i));
                return true;
            }
        return false;
    }
    bool hasLabel(const std::string &label) {
        render();
        return std::any_of(actions_.begin(), actions_.end(), [&](const Action &a) { return a.label == label; });
    }
};

#ifdef MINESWITCH_UI_TEST
// README screenshots: real catalog data and a real modpack import (needs network).
void Application::showcase(const std::string &directory) {
    auto finish = [this] {
        frame();
        Uint32 deadline = SDL_GetTicks() + 900000;
        while ((task_.active || query_.active) && SDL_GetTicks() < deadline) {
            frame();
            draw_.present();
            SDL_Delay(5);
        }
        frame();
        if (task_.active || query_.active)
            throw Error("showcase", "worker did not finish");
    };
    auto snap = [this, &directory](const std::string &name) {
        // Icons decode on a worker; give them time, then drop transient toasts.
        for (int i = 0; i < 150; ++i) {
            frame();
            SDL_Delay(20);
        }
        toasts_.clear();
        frame();
        draw_.screenshot(directory + "/" + name + ".bmp");
        std::printf("SHOWCASE %s\n", name.c_str());
    };
    auto click = [this](const std::string &label) {
        if (!clickLabel(label))
            throw Error("showcase", "missing control " + label);
    };
    CatalogClient catalog(http_, settings_);
    auto pack = [&](const std::string &slug) {
        auto project = catalog.project(Provider::Modrinth, slug);
        auto versions = catalog.versions(Provider::Modrinth, project.id, "1.21.1");
        if (versions.empty())
            throw Error("showcase", "no 1.21.1 version of " + slug);
        changePage(Page::Catalog);
        packs_browse_.project = project;
        downloadPack(versions.front());
        finish();
        import();
        finish();
        closeDetail();
    };
    pack("fabulously-optimized");
    pack("additive");
    auto make = [this](const std::string &name, LoaderKind loader, const std::string &version, int icon, std::int64_t played) {
        Instance item;
        item.id = newId();
        item.name = name;
        item.root = joinPath(storage_.root(), "instances/" + item.id);
        item.game.minecraft_version = "1.21.1";
        item.game.loader = {loader, version};
        item.icon = "preset:" + std::to_string(icon);
        item.last_played = played;
        createDirectories(joinPath(item.root, "game/mods"));
        storage_.saveInstance(item);
        return item;
    };
    auto now = static_cast<std::int64_t>(std::time(nullptr));
    make("Survival with friends", LoaderKind::Fabric, "0.16.14", 0, now - 3600 * 5);
    make("Vanilla 1.21.1", LoaderKind::Vanilla, "", 4, now - 86400 * 3);
    make("Tech pack", LoaderKind::NeoForge, "21.1.242", 2, 0);
    for (auto item : storage_.listInstances())
        if (item.name.find("Fabulously") != std::string::npos) {
            item.last_played = now - 600;
            storage_.saveInstance(item);
        }
    refresh();
    changePage(Page::Library);
    snap("library");
    auto main = std::find_if(instances_.begin(), instances_.end(), [](const Instance &i) { return i.name.find("Fabulously") != std::string::npos; });
    if (main == instances_.end())
        throw Error("showcase", "imported pack missing");
    openInstance(*main);
    finish();
    snap("overview");
    click("tab:Mods");
    finish();
    snap("mods");
    click("+ Add");
    finish();
    snap("mod-catalog");
    back();
    closeDetail();
    changePage(Page::Catalog);
    packs_browse_ = Browse{};
    packs_browse_.sort = SearchSort::Downloads;
    search();
    finish();
    snap("catalog");
    changePage(Page::Library);
    click("+ Create");
    finish();
    snap("wizard");
    wizard_.reset();
    changePage(Page::Settings);
    snap("settings");
}
void Application::smokeTest(const std::string &archive, const std::string &screenshot) {
    auto check = [](bool value, const char *name) {
        if (!value)
            throw Error("ui_test", name);
    };
    auto type = [this](const std::string &value) {
        dialog_ = value;
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = SDLK_RETURN;
        SDL_PushEvent(&event);
        events();
    };
    auto finish = [this, &check] {
        // Screens start their own loads while rendering, so draw one frame first.
        frame();
        Uint32 deadline = SDL_GetTicks() + 15000;
        while ((task_.active || query_.active) && SDL_GetTicks() < deadline) {
            frame();
            draw_.present();
            SDL_Delay(5);
        }
        frame();
        if (task_.active || query_.active)
            std::fprintf(stderr, "STUCK task=%s query=%s\n", task_.active ? task_.title.c_str() : "-", query_.active ? query_.title.c_str() : "-");
        check(!task_.active && !query_.active, "worker completed within timeout");
    };
    auto click = [this, &check](const std::string &label) { check(clickLabel(label), label.c_str()); };
    // Screen captures for visual review next to the final screenshot.
    auto snap = [this, &screenshot](const std::string &name) {
        if (screenshot.empty())
            return;
        for (int i = 0; i < 60; ++i) {
            frame();
            SDL_Delay(20);
        }
        auto base = screenshot.substr(0, screenshot.find_last_of('.'));
        draw_.screenshot(base + "-" + name + ".bmp");
    };
    check(instances_.empty(), "test root starts without instances");
    render();
    check(focus_ >= content_start_, "initial focus starts in the page content");
    // New instance through the wizard, offline: the manual version path.
    click("+ Create");
    check(wizard_.has_value(), "create opens the wizard");
    finish();
    snap("wizard");
    click("Enter version manually");
    check(editing_, "manual version opens keyboard");
    type("1.21.1");
    check(wizard_->minecraft == "1.21.1", "manual version is selected");
    click("Create");
    check(instances_.size() == 1 && detail_ && detail_->game.minecraft_version == "1.21.1" && !wizard_, "new instance is persisted");
    finish();
    snap("instance");
    // Rename via overview, then the per-instance memory override.
    click("Name");
    type("Тестовая");
    check(storage_.listInstances().front().name == "Тестовая", "rename persists");
    click("tab:Settings");
    click("Java memory");
    check(modal_.has_value(), "memory opens a menu");
    snap("menu");
    click("menu:512 MB");
    check(!modal_ && detail_->heap_mib == 512 && storage_.listInstances().front().heap_mib == 512, "instance memory override persists");
    // Servers tab writes a real servers.dat that reads back.
    click("tab:Servers");
    finish();
    click("+ Add server");
    type("example.org:25565");
    type("Example");
    finish();
    check(servers_.size() == 1 && servers_[0].address == "example.org:25565" && servers_[0].name == "Example", "server is saved to servers.dat");
    snap("servers");
    back();
    check(!detail_, "back closes the instance");
    // Profiles and global memory.
    changePage(Page::Settings);
    click("setting:+ Add profile");
    check(editing_, "add profile opens keyboard");
    type("Explorer");
    check(profile().username == "Explorer" && storage_.loadProfiles().size() == 2, "new offline profile selected and saved");
    snap("settings");
    int previous = settings_.heap_mib;
    click("setting:Default memory: " + std::to_string(previous) + " MB");
    click("menu:768 MB");
    check(settings_.heap_mib == 768 && storage_.loadSettings().heap_mib == 768, "memory setting persists");
    // Interface language: Russian applies at once and persists, then back to English.
    click("setting:Language / Язык: English");
    check(modal_.has_value(), "language opens a menu");
    click("menu:Русский");
    check(language() == Language::Russian && storage_.loadSettings().language == "ru", "Russian language persists");
    check(hasLabel("Сборки") && !hasLabel("Library") && hasLabel("setting:Память по умолчанию: 768 МБ"), "Russian labels render");
    snap("settings-ru");
    changePage(Page::Library);
    render();
    check(hasLabel("+ Создать"), "Russian library button");
    changePage(Page::Settings);
    click("setting:Language / Язык: Русский");
    click("menu:English");
    check(language() == Language::English && storage_.loadSettings().language == "en" && hasLabel("Library"), "English language restored");
    // Import an archive: background inspection while browsing.
    changePage(Page::Files);
    inspect(archive);
    changePage(Page::Library);
    check(task_.active && page_ == Page::Library, "UI can navigate during a background job");
    finish();
    check(inspected_.has_value(), "valid pack reaches import review");
    snap("inspection");
    import();
    finish();
    check(instances_.size() == 2 && detail_ && detail_->state == InstanceState::NeedsPreparation, "import commits the pack without claiming game readiness");
    check(readTextFile(joinPath(detail_->root, "game/config/example.txt")) == "client settings\n", "client overrides are visible after installation");
    auto imported = *detail_;
    click("tab:Mods");
    finish();
    check(mods_.size() == 1 && mods_[0].enabled, "instance mod manager reads real JAR metadata");
    snap("mods");
    click("mod:fixture.jar");
    finish();
    check(!mods_[0].enabled && pathExists(joinPath(imported.root, "game/mods/fixture.jar.disabled")), "UI disable persists without deleting mod");
    click("mod:fixture.jar.disabled");
    finish();
    check(mods_[0].enabled && pathExists(joinPath(imported.root, "game/mods/fixture.jar")), "UI enable restores mod");
    click("+ Add");
    check(browsing_content_ && content_browse_.kind == ContentKind::Mod && detail_->game.loader.kind == LoaderKind::Fabric,
          "mod catalog retains instance loader");
    finish();
    snap("catalog");
    back();
    check(detail_ && !browsing_content_ && tab_ == Tab::Mods, "catalog back returns to installed mods");
    click("tab:Worlds");
    finish();
    check(worlds_.size() == 1 && worlds_[0].name == "Fixture World" && worlds_[0].game_type == 1, "worlds tab reads level.dat");
    snap("worlds");
    click("world:FixtureWorld");
    check(modal_.has_value(), "world opens its menu");
    click("menu:Copy world");
    finish();
    check(worlds_.size() == 2, "world copy appears");
    click("tab:Resource packs");
    finish();
    check(packs_.size() == 1 && packs_[0].description == "Fixture pack", "resource pack metadata is read");
    snap("packs");
    click("pack:fixture-pack.zip");
    finish();
    check(!packs_[0].enabled, "resource pack disable persists");
    click("tab:Logs");
    finish();
    check(!logs_.empty(), "logs are listed");
    openLog(logs_.front());
    finish();
    check(log_ && !log_->lines.empty(), "log viewer reads lines");
    snap("log");
    back();
    // Copy and delete with confirmation.
    duplicate(imported);
    type("Копия");
    finish();
    check(storage_.listInstances().size() == 3, "instance copy is created");
    auto copies = storage_.listInstances();
    auto copy = *std::find_if(copies.begin(), copies.end(), [](const Instance &i) { return i.name == "Копия"; });
    check(pathExists(joinPath(copy.root, "game/mods/fixture.jar")), "copy contains the mods");
    removeInstance(copy);
    check(modal_.has_value(), "delete asks for confirmation");
    click("menu:Delete forever");
    finish();
    check(storage_.listInstances().size() == 2, "confirmed delete removes the copy");
    exportZip(imported);
    finish();
    check(pathExists(joinPath(storage_.root(), "exports")), "export writes a ZIP");
    closeDetail();
    changePage(Page::Library);
    render();
    check(storage_.listInstances().size() == 2, "library survives a storage reload");
    // Cancellation and failures surface as toasts and task history.
    task("Cancellation check", [](ProgressCallback, const std::atomic_bool *stop) -> Apply {
        while (!stop->load())
            SDL_Delay(2);
        throw Error("cancelled", "Cancelled");
    });
    changePage(Page::Tasks);
    click("Cancel task");
    finish();
    check(!toasts_.empty() && toasts_.back().text.find("cancelled") != std::string::npos, "cancel action reaches the worker and reports completion");
    task("Async failure", [](ProgressCallback, const std::atomic_bool *) -> Apply { throw Error("fixture", "Ordinary test job failure"); });
    openInstance(instances_.front());
    finish();
    check(!history_.empty() && !history_.front().ok, "background failure is recorded");
    snap("history");
    auto example = instances_.front();
    task("Preparation review", [this, example](ProgressCallback, const std::atomic_bool *) -> Apply {
        LaunchPlan plan;
        plan.preparation_only = true;
        return [this, example, plan] { prepared(example, plan, false); };
    });
    finish();
    check(processor_launch_ && page_ == Page::Tasks && !detail_, "processor preparation review replaces an unrelated detail page");
    click("Later");
    check(!processor_launch_ && page_ == Page::Library, "processor preparation can be deferred");
    for (bool processors : {false, true}) {
        pending_launch_ = LaunchPlan{};
        task("Full validation review", [this, example, processors](ProgressCallback, const std::atomic_bool *) -> Apply {
            LaunchPlan plan;
            plan.preparation_only = processors;
            return [this, example, plan] { prepared(example, plan, true); };
        });
        check(!pending_launch_, "new validation supersedes a prepared launch in the same frame");
        finish();
        check(!processor_launch_ && !pending_launch_, "explicit full validation does not launch game or processors");
    }
    task("Exit during download", [](ProgressCallback, const std::atomic_bool *stop) -> Apply {
        while (!stop->load())
            SDL_Delay(2);
        throw Error("cancelled", "Cancelled");
    });
    pad(SDL_CONTROLLER_BUTTON_START);
    check(exit_requested_ && task_.cancelled, "Plus requests cancellation before leaving an active job");
    check(!run({}, 100) && quit_ && !task_.active, "Plus exits after the worker has safely finished");
    quit_ = false;
    exit_requested_ = false;
    pending_launch_ = LaunchPlan{};
    SDL_Event exit{};
    exit.type = SDL_QUIT;
    SDL_PushEvent(&exit);
    events();
    check(!run({}, 1), "explicit exit wins over a completed launch plan");
    quit_ = false;
    pending_launch_.reset();
    changePage(Page::Library);
    for (int i = 0; i < 20; ++i) {
        render();
        SDL_Delay(5);
    }
    if (!screenshot.empty())
        draw_.screenshot(screenshot);
    draw_.present();
    std::printf("UI_SMOKE_PASS wizard, instance tabs, servers, worlds, packs, logs, mods, copy/delete/export, "
                "profiles, settings, language, import, jobs, cancellation, exit precedence\n");
}
#endif

char *owned(const std::string &value) {
    auto *copy = static_cast<char *>(std::malloc(value.size() + 1));
    if (!copy)
        throw std::bad_alloc();
    std::memcpy(copy, value.c_str(), value.size() + 1);
    return copy;
}
} // namespace
} // namespace mineswitch

namespace {
std::string frontend_error;
}
extern "C" const char *mineswitch_frontend_error(void) { return frontend_error.c_str(); }
extern "C" void mineswitch_free_launch(MineSwitchLaunch *launch) {
    if (!launch)
        return;
    std::free(launch->main_class);
    std::free(launch->options_file);
    std::free(launch->working_directory);
    std::free(launch->preparation_summary);
    if (launch->argv)
        for (int i = 0; i < launch->argc; ++i)
            std::free(launch->argv[i]);
    std::free(launch->argv);
    *launch = {};
}
extern "C" int mineswitch_frontend(const char *root, MineSwitchLaunch *launch, int heap_override_mib) {
    if (!root || !launch)
        return -1;
    *launch = {};
    try {
        std::optional<mineswitch::LaunchPlan> plan;
        {
            mineswitch::Application app(root, false, heap_override_mib);
            plan = app.run();
        }
        if (!plan)
            return 0;
        std::string options;
        for (const auto &item : plan->jvm_options) {
            if (item.find_first_of("\r\n") != std::string::npos)
                throw mineswitch::Error("launch_options", "Invalid JVM option");
            options += item + "\n";
        }
        std::string options_file = mineswitch::joinPath(root, "active-launch.options");
        mineswitch::atomicWriteText(options_file, options);
        launch->main_class = mineswitch::owned(plan->main_class);
        launch->options_file = mineswitch::owned(options_file);
        launch->working_directory = mineswitch::owned(plan->working_directory);
        launch->preparation_summary = mineswitch::owned(plan->preparation_summary);
        launch->preparation_only = plan->preparation_only;
        launch->argc = static_cast<int>(plan->game_arguments.size());
        launch->argv = static_cast<char **>(std::calloc(launch->argc, sizeof(char *)));
        if (launch->argc && !launch->argv)
            throw std::bad_alloc();
        for (int i = 0; i < launch->argc; ++i)
            launch->argv[i] = mineswitch::owned(plan->game_arguments[i]);
        return 1;
    } catch (const std::exception &e) {
        frontend_error = e.what();
        std::fprintf(stderr, "FRONTEND_ERROR %s\n", e.what());
        mineswitch_free_launch(launch);
        return -1;
    }
}
#ifndef __SWITCH__
int main(int argc, char **argv) {
    std::string root, screenshot;
#ifdef MINESWITCH_UI_TEST
    std::string test_archive, showcase;
#endif
    bool preview = false;
    unsigned frames = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--root" && i + 1 < argc)
            root = argv[++i];
        else if (a == "--preview")
            preview = true;
#ifdef MINESWITCH_UI_TEST
        else if (a == "--test-archive" && i + 1 < argc)
            test_archive = argv[++i];
        else if (a == "--showcase" && i + 1 < argc)
            showcase = argv[++i];
#endif
        else if (a == "--screenshot" && i + 1 < argc)
            screenshot = argv[++i];
        else if (a == "--frames" && i + 1 < argc)
            frames = std::strtoul(argv[++i], nullptr, 10);
        else {
            std::fprintf(stderr, "Usage: %s --root ABSOLUTE_DIRECTORY [--preview] [--frames N] [--screenshot file.bmp]\n", argv[0]);
            return 2;
        }
    }
    if (root.empty()) {
        std::fprintf(stderr, "--root is required for the desktop launcher\n");
        return 2;
    }
    try {
        mineswitch::Application app(root, preview);
#ifdef MINESWITCH_UI_TEST
        if (!test_archive.empty()) {
            app.smokeTest(test_archive, screenshot);
            return 0;
        }
        if (!showcase.empty()) {
            app.showcase(showcase);
            return 0;
        }
#endif
        app.run(screenshot, frames);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
#endif
