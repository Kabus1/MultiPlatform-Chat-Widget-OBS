#include "core/TtsTextFilter.hpp"

#include "core/EmoteRegistry.hpp"
#include "core/Util.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace usc {

namespace {

// Common Twitch/Kick/BTTV/FFZ/7TV emote names that appear as plain words.
const std::unordered_set<std::string>& builtinWords() {
    static const std::unordered_set<std::string> words = [] {
        const char* list[] = {
            "kappa", "keepo", "kappapride", "lul", "lulw", "omegalul", "kekw", "kek", "pog", "pogchamp",
            "poggers", "pogu", "poggies", "pogo", "biblethump", "residentsleeper", "notlikethis", "wutface",
            "dansgame", "seemsgood", "trihard", "kreygasm", "4head", "failfish", "babyrage", "swiftrage",
            "pjsalt", "heyguys", "coolstorybob", "cmonbruh", "jebaited", "sourpls", "monkas", "monkaw",
            "monkahmm", "monkagiga", "pepega", "pepelaugh", "pepehands", "peped", "pepepls", "pepejam",
            "pepesmile", "pepeds", "feelsbadman", "feelsgoodman", "feelsstrongman", "feelsweirdman",
            "feelsokayman", "sadge", "madge", "bedge", "widepeepohappy", "widepeeposad", "peepohappy",
            "peeposad", "peepoclap", "catjam", "copium", "hopium", "gigachad",
            "ezclap", "weirdchamp", "pogslide", "modcheck", "nodders", "nopers", "hypers",
            "5head", "3head", "ayaya", "forsene", "gachigasm", "kekl", "kekwait", "omegaroll",
            "pausechamp", "pauseman", "residentsleeper", "smoge", "susge", 
            "ratjam", "blobdance", "kappahd", "minik", "lulwut", "d:", "xd", "xdd", "xddd",
        };
        std::unordered_set<std::string> s;
        for (const char* w : list) s.insert(w);
        return s;
    }();
    return words;
}

bool isEmojiOrSymbol(char32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) ||  // emoji, pictographs, flags, symbols
           (c >= 0x2600 && c <= 0x27BF) ||    // misc symbols + dingbats
           (c >= 0x2B00 && c <= 0x2BFF) ||    // arrows / stars
           (c >= 0x2190 && c <= 0x21FF) ||    // arrows
           (c >= 0x2300 && c <= 0x23FF) ||    // misc technical (⌚ ⏰ ⏩)
           (c >= 0x2500 && c <= 0x25FF) ||    // box drawing, blocks, shapes (ASCII art)
           (c >= 0x2800 && c <= 0x28FF) ||    // braille art
           (c >= 0xFE00 && c <= 0xFE0F) ||    // variation selectors
           (c >= 0xE0000 && c <= 0xE007F) ||  // tag characters (flag sequences)
           (c >= 0xE000 && c <= 0xF8FF) ||    // private use (custom emote fonts)
           c == 0x200D || c == 0x20E3 || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299 ||
           c == 0x00A9 || c == 0x00AE || c == 0x2122 || c == 0xFFFD;
}

// Strips punctuation around a token for word matching ("KEKW!" -> "KEKW").
std::string core(const std::string& token) {
    size_t b = 0, e = token.size();
    auto punct = [](char c) { return c == '.' || c == ',' || c == '!' || c == '?' || c == '"' || c == '\'' || c == '(' || c == ')' || c == ';'; };
    while (b < e && punct(token[b])) ++b;
    while (e > b && punct(token[e - 1])) --e;
    return token.substr(b, e - b);
}

} // namespace

bool TtsTextFilter::isBuiltinEmoteWord(const std::string& word) {
    return builtinWords().count(util::toLower(word)) > 0;
}

bool TtsTextFilter::looksLikeEmoteCode(const std::string& w) {
    // Twitch/7TV naming convention: lowercase prefix (>= 3) followed by a
    // capitalised part, ASCII only: "xqcL", "catJAM", "monkaS", "widepeepoHappy".
    if (w.size() < 4) return false;
    size_t i = 0;
    while (i < w.size() && ((w[i] >= 'a' && w[i] <= 'z') || (w[i] >= '0' && w[i] <= '9'))) ++i;
    if (i < 3 || i >= w.size() || !(w[i] >= 'A' && w[i] <= 'Z')) return false;
    return std::all_of(w.begin(), w.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool TtsTextFilter::containsLink(const std::string& text) {
    static const std::regex scheme(R"((https?|ftp)://|www\.)", std::regex::icase);
    // Bare domains: word.tld or word.tld/path, for TLDs chat spam actually uses.
    static const std::regex domain(
        R"((^|[^A-Za-z0-9@])[A-Za-z0-9][A-Za-z0-9-]*(\.[A-Za-z0-9-]+)*\.)"
        R"((com|net|org|gg|tv|io|ly|co|app|xyz|live|link|info|dev|ru|de|uk|fr|es|nl|pl|tr|sa|ae|eg|ma|dz|iq|jo|kw|qa|)"
        R"(tk|ml|ga|cf|shop|store|site|online|top|be|ca|br|ai|gl|cc|ws|biz|fun|club|click|pro|vip|win|stream|chat)"
        R"()(?![A-Za-z0-9])(/\S*)?)",
        std::regex::icase);
    return std::regex_search(text, scheme) || std::regex_search(text, domain);
}

std::string TtsTextFilter::stripLinks(const std::string& text) {
    static const std::regex url(R"(((https?|ftp)://|www\.)\S+)", std::regex::icase);
    return util::trim(std::regex_replace(text, url, ""));
}

std::string TtsTextFilter::clean(const std::string& input, const Options& opt) {
    // 1) Inline emote tokens that survived parsing.
    static const std::regex kickToken(R"(\[(emote|emoji):[^\]]*\])", std::regex::icase);
    static const std::regex ytShortcode(R"(:[A-Za-z][A-Za-z0-9_+\-]*:)");
    std::string text = std::regex_replace(input, kickToken, " ");
    text = std::regex_replace(text, ytShortcode, " ");

    // 2) Emoji / symbols.
    if (opt.stripEmoji) {
        auto cps = util::utf8Decode(text);
        std::vector<char32_t> kept;
        kept.reserve(cps.size());
        for (char32_t c : cps) kept.push_back(isEmojiOrSymbol(c) ? U' ' : c);
        text = util::utf8Encode(kept, 0, kept.size());
    }

    // 3) Emote words, token by token.
    std::unordered_set<std::string> extra;
    if (opt.extraEmoteWords.is_array())
        for (const auto& w : opt.extraEmoteWords)
            if (w.is_string()) extra.insert(util::toLower(util::trim(w.get<std::string>())));

    std::istringstream in(text);
    std::string token, out;
    while (in >> token) {
        std::string c = core(token);
        if (c.empty()) {
            // Pure punctuation: keep it only if it is short ("?" "!!").
            if (token.size() <= 3) out += (out.empty() ? "" : " ") + token;
            continue;
        }
        std::string lower = util::toLower(c);
        bool emote = builtinWords().count(lower) || extra.count(lower) ||
                     (opt.registry && opt.registry->contains(opt.platform, opt.channelId, c)) ||
                     (opt.guessEmoteWords && looksLikeEmoteCode(c));
        if (!emote) out += (out.empty() ? "" : " ") + token;
    }

    // 4) Leftover noise: nothing but punctuation / digits is not worth reading.
    bool hasLetter = false;
    for (char32_t c : util::utf8Decode(out))
        if (c > 0x7F || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z')) { hasLetter = true; break; }
    return hasLetter ? util::trim(out) : std::string();
}

} // namespace usc
