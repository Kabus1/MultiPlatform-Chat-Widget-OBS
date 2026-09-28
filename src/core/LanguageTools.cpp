#include "core/LanguageTools.hpp"

#include "core/Util.hpp"

#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace usc::lang {

namespace {

bool isArabicLetter(char32_t c) {
    return (c >= 0x0620 && c <= 0x064A) || (c >= 0x066E && c <= 0x06D3) || c == 0x06D5 ||
           (c >= 0x06FA && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0x08A0 && c <= 0x08FF) ||
           (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFC);
}

// Letters used by Persian / Urdu but not in Arabic writing. گ چ ڤ are left
// out on purpose: Iraqi, Gulf and Maghrebi dialects use them ("شگد", "چا").
bool isPersianUrduLetter(char32_t c) {
    switch (c) {
    case 0x067E: // پ
    case 0x0698: // ژ
    case 0x06A9: // ک (keheh)
    case 0x06CC: // ی (Farsi yeh)
    case 0x06D2: // ے
    case 0x0679: // ٹ
    case 0x0688: // ڈ
    case 0x0691: // ڑ
    case 0x06BA: // ں
    case 0x06C0: // ۀ
    case 0x06BE: // ھ
        return true;
    default:
        return false;
    }
}

// Letters that only Arabic uses (Persian/Urdu write ی and ک instead).
bool isArabicOnlyLetter(char32_t c) {
    return c == 0x0629 /* ة */ || c == 0x064A /* ي */ || c == 0x0649 /* ى */ || c == 0x0643 /* ك */;
}

bool isLatinLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7);
}

bool isOtherLetter(char32_t c) {
    // Rough: any non-Latin, non-Arabic character in a letter-ish block.
    return (c >= 0x0370 && c <= 0x05FF) ||  // Greek, Cyrillic, Armenian, Hebrew
           (c >= 0x0900 && c <= 0x0DFF) ||  // Indic
           (c >= 0x0E00 && c <= 0x0EFF) ||  // Thai, Lao
           (c >= 0x1100 && c <= 0x11FF) ||  // Hangul Jamo
           (c >= 0x3040 && c <= 0x30FF) ||  // Kana
           (c >= 0x4E00 && c <= 0x9FFF) ||  // CJK
           (c >= 0xAC00 && c <= 0xD7AF);    // Hangul
}

// Common English chat slang / abbreviations -> plain English.
const std::unordered_map<std::string, std::string>& slangTable() {
    static const std::unordered_map<std::string, std::string> t = {
        {"lol", "haha"}, {"lmao", "haha"}, {"lmfao", "haha"}, {"rofl", "haha"}, {"xd", "haha"},
        {"lul", "haha"}, {"kekw", "haha"}, {"hahaha", "haha"}, {"hehe", "haha"},
        {"brb", "be right back"}, {"bbl", "be back later"}, {"afk", "away from keyboard"},
        {"idk", "I don't know"}, {"idc", "I don't care"}, {"ik", "I know"}, {"ikr", "I know, right"},
        {"imo", "in my opinion"}, {"imho", "in my honest opinion"}, {"tbh", "honestly"},
        {"tbf", "to be fair"}, {"ngl", "honestly"}, {"fr", "for real"}, {"frfr", "for real"},
        {"istg", "I swear to God"}, {"nvm", "never mind"}, {"np", "no problem"}, {"ty", "thank you"},
        {"tysm", "thank you so much"}, {"tyvm", "thank you very much"}, {"thx", "thanks"},
        {"thnx", "thanks"}, {"tnx", "thanks"}, {"pls", "please"}, {"plz", "please"}, {"plox", "please"},
        {"u", "you"}, {"ur", "your"}, {"r", "are"}, {"y", "why"}, {"k", "okay"}, {"kk", "okay"},
        {"okie", "okay"}, {"omg", "oh my god"}, {"omfg", "oh my god"}, {"wtf", "what the hell"},
        {"wth", "what the hell"}, {"gg", "good game"}, {"ggs", "good games"}, {"ggwp", "good game, well played"},
        {"gl", "good luck"}, {"glhf", "good luck, have fun"}, {"hf", "have fun"}, {"wp", "well played"},
        {"ez", "easy"}, {"gtg", "got to go"}, {"g2g", "got to go"}, {"ttyl", "talk to you later"},
        {"btw", "by the way"}, {"asap", "as soon as possible"}, {"dm", "private message"},
        {"dms", "private messages"}, {"irl", "in real life"}, {"rn", "right now"}, {"atm", "at the moment"},
        {"bc", "because"}, {"cuz", "because"}, {"coz", "because"}, {"cos", "because"}, {"smh", "disappointed"},
        {"tho", "though"}, {"gonna", "going to"}, {"wanna", "want to"}, {"gotta", "have to"},
        {"ya", "you"}, {"yea", "yes"}, {"yeah", "yes"}, {"yep", "yes"}, {"yup", "yes"}, {"nah", "no"},
        {"nope", "no"}, {"bro", "brother"}, {"bruh", "brother"}, {"sis", "sister"}, {"fam", "friends"},
        {"pog", "amazing"}, {"poggers", "amazing"}, {"pogchamp", "amazing"}, {"op", "overpowered"},
        {"noob", "beginner"}, {"n00b", "beginner"}, {"nub", "beginner"}, {"mvp", "most valuable player"},
        {"ily", "I love you"}, {"ilysm", "I love you so much"}, {"hbu", "how about you"},
        {"wbu", "what about you"}, {"wyd", "what are you doing"}, {"hru", "how are you"},
        {"sup", "what's up"}, {"wassup", "what's up"}, {"wsp", "what's up"}, {"wsg", "what's good"},
        {"dw", "don't worry"}, {"jk", "just kidding"}, {"ofc", "of course"}, {"def", "definitely"},
        {"prob", "probably"}, {"probs", "probably"}, {"obv", "obviously"}, {"tmr", "tomorrow"},
        {"tmrw", "tomorrow"}, {"2day", "today"}, {"2nite", "tonight"}, {"gn", "good night"},
        {"gm", "good morning"}, {"lmk", "let me know"}, {"fyi", "for your information"},
        {"iirc", "if I remember correctly"}, {"sus", "suspicious"}, {"w/", "with"}, {"w/o", "without"},
        {"b4", "before"}, {"gr8", "great"}, {"l8r", "later"}, {"m8", "mate"}, {"sry", "sorry"},
        {"srry", "sorry"}, {"luv", "love"}, {"cya", "see you"}, {"cu", "see you"}, {"ttys", "talk to you soon"},
        {"hmu", "contact me"}, {"ffs", "for god's sake"}, {"stfu", "shut up"}, {"af", "very"},
        {"lit", "amazing"}, {"goat", "greatest of all time"}, {"clutch", "amazing play"},
        {"ggez", "good game, easy"}, {"rip", "rest in peace"}, {"hype", "excitement"},
    };
    return t;
}

// Case-sensitive entries: only meaningful in capitals ("W stream", "huge L").
const std::unordered_map<std::string, std::string>& capsTable() {
    static const std::unordered_map<std::string, std::string> t = {
        {"W", "win"}, {"L", "loss"}, {"Ws", "wins"}, {"Ls", "losses"}, {"GOAT", "greatest of all time"},
    };
    return t;
}

// Single letters and words that exist in other languages: only expanded when
// the message is otherwise recognisably English.
const std::unordered_set<std::string>& ambiguous() {
    static const std::unordered_set<std::string> s = {"u", "r", "y", "k", "ya", "def", "prob", "sup", "cu",
                                                      "cos", "sis", "fam", "lit", "goat", "af", "op", "rip"};
    return s;
}

const std::unordered_set<std::string>& englishWords() {
    static const std::unordered_set<std::string> s = {
        "the", "you", "i", "is", "are", "am", "what", "this", "that", "and", "to", "it", "my", "me", "in",
        "on", "for", "so", "not", "do", "can", "have", "just", "like", "good", "love", "yes", "no", "was",
        "we", "he", "she", "they", "how", "why", "when", "where", "who", "your", "of", "a", "an", "be",
        "with", "at", "all", "but", "if", "or", "stream", "game", "play", "nice", "great", "hello", "hi",
        "hey", "thanks", "thank", "please", "bro", "guys", "chat", "streamer", "now", "today", "here",
        "there", "cool", "wow", "man", "dude", "really", "very", "much", "more", "one", "know", "think",
        "want", "get", "got", "go", "going", "see", "look", "win", "won", "lost", "lose", "again",
        "best", "better", "bad", "fun", "funny", "happy", "sad", "welcome", "back", "time", "first",
    };
    return s;
}

// Abbreviations that stand in for a single word inside a sentence ("u r the
// best"): never separated by commas, unlike standalone phrases (gg, brb, idk).
bool isGrammarWord(const std::string& low) {
    static const std::unordered_set<std::string> s = {
        "u", "ur", "r", "y", "ya", "w/", "w/o", "b4", "bc", "cuz", "coz", "cos", "gonna", "wanna", "gotta",
        "tho", "af", "pls", "plz", "plox", "luv", "2day", "2nite", "tmr", "tmrw", "l8r", "rn", "atm", "m8",
        "gr8", "def", "prob", "probs", "obv", "k", "kk", "okie", "yea", "yeah", "yep", "yup", "nah", "nope"};
    return s.count(low) > 0;
}

std::string stripPunct(const std::string& token, std::string& prefix, std::string& suffix) {
    size_t b = 0, e = token.size();
    auto punct = [](char c) { return c == '.' || c == ',' || c == '!' || c == '?' || c == '"' || c == '(' || c == ')' || c == ';' || c == ':'; };
    while (b < e && punct(token[b])) ++b;
    while (e > b && punct(token[e - 1])) --e;
    prefix = token.substr(0, b);
    suffix = token.substr(e);
    return token.substr(b, e - b);
}

} // namespace

ScriptStats scriptStats(const std::string& text) {
    ScriptStats s;
    for (char32_t c : util::utf8Decode(text)) {
        if (isArabicLetter(c)) {
            ++s.arabic;
            if (isPersianUrduLetter(c)) ++s.persianUrdu;
            if (isArabicOnlyLetter(c)) ++s.arabicOnly;
        } else if (isLatinLetter(c)) {
            ++s.latin;
        } else if (isOtherLetter(c)) {
            ++s.other;
        }
    }
    return s;
}

bool isArabicText(const std::string& text) {
    ScriptStats s = scriptStats(text);
    if (s.arabic == 0) return false;
    // Mostly Arabic script, and not clearly Persian / Urdu spelling. Any
    // Arabic-only letter (ي ة ى ك) settles it as Arabic.
    return s.arabic >= s.latin + s.other && (s.arabicOnly > 0 || s.persianUrdu == 0);
}

bool isLatinOnly(const std::string& text) {
    ScriptStats s = scriptStats(text);
    return s.latin > 0 && s.arabic == 0 && s.other == 0;
}

SlangResult expandSlang(const std::string& text, const nlohmann::json& custom) {
    SlangResult r;
    std::unordered_map<std::string, std::string> user;
    if (custom.is_array()) {
        for (const auto& e : custom) {
            if (!e.is_string()) continue;
            std::string line = e.get<std::string>();
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = util::toLower(util::trim(line.substr(0, eq)));
            std::string v = util::trim(line.substr(eq + 1));
            if (!k.empty() && !v.empty()) user[k] = v;
        }
    }

    // First pass: tokenise and decide whether the message is English-like.
    std::vector<std::string> tokens;
    {
        std::istringstream in(text);
        std::string t;
        while (in >> t) tokens.push_back(t);
    }
    int englishHits = 0, slangHits = 0, ambiguousHits = 0;
    for (const auto& t : tokens) {
        std::string pre, suf;
        std::string core = stripPunct(t, pre, suf);
        if (core.empty() || !isLatinOnly(core)) continue;
        ++r.words;
        std::string low = util::toLower(core);
        if (englishWords().count(low)) ++englishHits;
        if (user.count(low) || capsTable().count(core) ||
            (slangTable().count(low) && !ambiguous().count(low)))
            ++slangHits;
        else if (ambiguous().count(low))
            ++ambiguousHits;
    }
    // Ambiguous tokens ("u", "r", "y") only count once there is other clear
    // English evidence, so "hola y adios" stays Spanish.
    int evidence = englishHits + slangHits;
    if (evidence > 0) evidence += ambiguousHits;
    r.englishLike = r.words > 0 && evidence * 2 >= r.words;

    // Second pass: expand. Consecutive expanded phrases get a comma between
    // them ("gg wp ngl" -> "good game, well played, honestly") so the
    // translator treats them as separate phrases, not one odd sentence.
    std::string out;
    bool prevExpandedOpen = false; // previous token was a phrase expansion with no punctuation after it
    for (const auto& t : tokens) {
        std::string pre, suf;
        std::string core = stripPunct(t, pre, suf);
        std::string low = util::toLower(core);
        std::string rep;
        if (auto u = user.find(low); u != user.end()) rep = u->second;
        else if (auto c = capsTable().find(core); c != capsTable().end() && (r.englishLike || tokens.size() <= 3)) rep = c->second;
        else if (auto s = slangTable().find(low); s != slangTable().end() && (!ambiguous().count(low) || r.englishLike))
            rep = s->second;
        if (!rep.empty()) {
            ++r.replaced;
            if (prevExpandedOpen && pre.empty()) out += ',';
        }
        out += (out.empty() ? "" : " ") + pre + (rep.empty() ? core : rep) + suf;
        prevExpandedOpen = !rep.empty() && suf.empty() && !isGrammarWord(low);
    }
    r.text = out;
    return r;
}

} // namespace usc::lang
