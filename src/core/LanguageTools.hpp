#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace usc::lang {

struct ScriptStats {
    size_t arabic = 0;        // Arabic-script letters
    size_t persianUrdu = 0;   // letters only used by Persian / Urdu (پ ژ ک ی ے ٹ ڈ ڑ ں ۀ ھ)
    size_t arabicOnly = 0;    // letters only used by Arabic (ي ة ى ك)
    size_t latin = 0;         // A-Z a-z and Latin accents
    size_t other = 0;         // letters of any other script
};

ScriptStats scriptStats(const std::string& text);

// True when the message is written in Arabic (any dialect, casual spelling,
// e.g. "هاي", "شلونك"). Such text is never translated or rewritten: script
// is checked locally instead of trusting Google's language detection, which
// often labels short dialect words as Persian/Urdu.
bool isArabicText(const std::string& text);

// True for text made only of Latin letters (plus digits / punctuation).
bool isLatinOnly(const std::string& text);

struct SlangResult {
    std::string text;      // text with abbreviations expanded ("idk" -> "I don't know")
    int replaced = 0;      // how many tokens were expanded
    int words = 0;         // Latin words in the message
    bool englishLike = false; // looks like English (slang / common English words)
};

// Expands English chat abbreviations and slang so Google Translate produces a
// real translation ("gg" -> "good game" -> "لعبة جيدة") instead of echoing
// the abbreviation back. `custom` is a list of "abbr=expansion" strings.
SlangResult expandSlang(const std::string& text, const nlohmann::json& custom = nlohmann::json::array());

} // namespace usc::lang
