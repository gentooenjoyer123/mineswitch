#pragma once
#include <initializer_list>
#include <string>

namespace mineswitch {
// UI language. Source strings are English; Russian is a lookup table.
enum class Language { English, Russian };
void setLanguage(Language);
Language language();
Language parseLanguage(const std::string& code); // "ru" -> Russian, anything else English
const char* languageCode(Language);

// Translation of an English UI string; the input itself when there is none.
const std::string& tr(const std::string& english);
// tr() of a format with {0}, {1}... placeholders, then the substitution.
std::string trf(const std::string& english, std::initializer_list<std::string> args);
// "1 mod" / "2 mods"; Russian uses the three plural forms.
std::string countText(long n, const char* one, const char* other,
                      const char* ru_one, const char* ru_few, const char* ru_many);
}
