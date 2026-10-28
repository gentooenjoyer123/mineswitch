#include "mineswitch/i18n.hpp"
#include <atomic>
#include <unordered_map>

namespace mineswitch {
namespace {
std::atomic<Language> current{Language::English};

// English source string -> Russian. Keep keys identical to the literals in code.
const std::unordered_map<std::string, std::string>& russian() {
    static const std::unordered_map<std::string, std::string> table = {
#include "i18n_ru.inc"
    };
    return table;
}
}

void setLanguage(Language value) { current.store(value); }
Language language() { return current.load(); }
Language parseLanguage(const std::string& code) { return code == "ru" ? Language::Russian : Language::English; }
const char* languageCode(Language value) { return value == Language::Russian ? "ru" : "en"; }

const std::string& tr(const std::string& english) {
    if (language() != Language::Russian) return english;
    const auto& table = russian();
    auto found = table.find(english);
    return found == table.end() ? english : found->second;
}

std::string trf(const std::string& english, std::initializer_list<std::string> args) {
    std::string out = tr(english);
    std::size_t index = 0;
    for (const auto& arg : args) {
        const std::string key = "{" + std::to_string(index++) + "}";
        for (auto at = out.find(key); at != std::string::npos; at = out.find(key, at + arg.size()))
            out.replace(at, key.size(), arg);
    }
    return out;
}

std::string countText(long n, const char* one, const char* other,
                      const char* ru_one, const char* ru_few, const char* ru_many) {
    if (language() != Language::Russian) return std::to_string(n) + " " + (n == 1 ? one : other);
    long tens = n % 100, units = n % 10;
    const char* word = tens >= 11 && tens <= 14 ? ru_many : units == 1 ? ru_one : units >= 2 && units <= 4 ? ru_few : ru_many;
    return std::to_string(n) + " " + word;
}
}
