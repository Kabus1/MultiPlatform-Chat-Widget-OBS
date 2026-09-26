// Minimal self-contained unit tests (no network): run with `ctest` or directly.
#include "core/BotFilter.hpp"
#include "core/EmoteRegistry.hpp"
#include "core/TtsTextFilter.hpp"
#include "core/MessagePipeline.hpp"
#include "core/Settings.hpp"
#include "core/Util.hpp"
#include "platforms/KickClient.hpp"
#include "platforms/TwitchClient.hpp"
#include "platforms/YouTubeClient.hpp"

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++failures;                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                            \
    } while (0)

using namespace usc;
using nlohmann::json;

void testTwitchParse() {
    // Emote offsets are code points: "é" is one code point but two bytes.
    std::string raw = "@badges=moderator/1,subscriber/6;color=#FF0000;display-name=Café;emotes=25:6-10;id=abc;"
                      "tmi-sent-ts=1700000000000;user-id=42 :cafe!cafe@cafe.tmi.twitch.tv PRIVMSG #chan :héllo Kappa !";
    auto line = TwitchClient::parseLine(raw);
    CHECK(line.command == "PRIVMSG");
    CHECK(line.params.size() == 2);
    auto m = TwitchClient::toChatMessage(line);
    CHECK(m.displayName == "Café");
    CHECK(m.username == "cafe");
    CHECK(m.channel == "chan");
    CHECK(m.userColor == "#FF0000");
    CHECK(m.timestamp == 1700000000000LL);
    CHECK(m.roles.size() == 2);
    CHECK(m.parts.size() == 3);
    CHECK(m.parts[0].text == "héllo ");
    CHECK(m.parts[1].type == MessagePart::Type::Emote && m.parts[1].text == "Kappa");
    CHECK(m.parts[2].text == " !");
    CHECK(m.speakableText() == "héllo !");

    auto action = TwitchClient::toChatMessage(TwitchClient::parseLine(":a!a@a PRIVMSG #c :\x01" "ACTION waves\x01"));
    CHECK(action.plainText() == "waves");

    auto ping = TwitchClient::parseLine("PING :tmi.twitch.tv");
    CHECK(ping.command == "PING" && ping.params.back() == "tmi.twitch.tv");
}

void testKickParse() {
    json data = {{"id", "uuid-1"},
                 {"content", "hi [emote:37226:KEKW] there"},
                 {"sender", {{"id", 7}, {"username", "RocketFan"}, {"slug", "rocketfan"},
                             {"identity", {{"color", "#00FF00"}, {"badges", json::array({{{"type", "moderator"}}})}}}}}};
    auto m = KickClient::parseChatEvent(data);
    CHECK(m.has_value());
    CHECK(m->displayName == "RocketFan");
    CHECK(m->parts.size() == 3);
    CHECK(m->parts[1].url == "https://files.kick.com/emotes/37226/fullsize");
    CHECK(m->roles.size() == 1 && m->roles[0] == "moderator");
}

void testYouTubeIds() {
    CHECK(YouTubeClient::extractVideoId("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=1") == "dQw4w9WgXcQ");
    CHECK(YouTubeClient::extractVideoId("https://youtu.be/dQw4w9WgXcQ") == "dQw4w9WgXcQ");
    CHECK(YouTubeClient::extractVideoId("https://youtube.com/live/dQw4w9WgXcQ?feature=share") == "dQw4w9WgXcQ");
    CHECK(YouTubeClient::extractVideoId(" dQw4w9WgXcQ ") == "dQw4w9WgXcQ");
}

ChatMessage sample(const std::string& text) {
    ChatMessage m;
    m.platform = "twitch";
    m.username = m.displayName = "Bob";
    m.parts.push_back({MessagePart::Type::Text, text, ""});
    return m;
}

void testPipelineDecisions() {
    json s = Settings::defaults();
    s["tts"]["enabled"] = true;

    // English message -> Arabic line shown; read as-is in English.
    auto d = MessagePipeline::decide(sample("hello"), s, "hello", true, "مرحبا", "en");
    CHECK(d.showTranslation);
    CHECK(d.tts.size() == 2);
    CHECK(d.tts[0]["text"] == "Bob:");
    CHECK(d.tts[1]["text"] == "hello" && d.tts[1]["lang"] == "en");

    // Translated reading mode.
    s["tts"]["mode"] = "translated";
    d = MessagePipeline::decide(sample("hello"), s, "hello", true, "مرحبا", "en");
    CHECK(d.tts[1]["text"] == "مرحبا" && d.tts[1]["lang"] == "ar");

    // Arabic message: no translation line, read in Arabic.
    d = MessagePipeline::decide(sample("مرحبا"), s, "مرحبا", true, "مرحبا", "ar");
    CHECK(!d.showTranslation);
    CHECK(d.tts[1]["lang"] == "ar");

    // French: only with showForAllLanguages.
    d = MessagePipeline::decide(sample("bonjour"), s, "bonjour", true, "صباح الخير", "fr");
    CHECK(!d.showTranslation);
    s["translation"]["showForAllLanguages"] = true;
    d = MessagePipeline::decide(sample("bonjour"), s, "bonjour", true, "صباح الخير", "fr");
    CHECK(d.showTranslation);

    // Username / message toggles.
    s["tts"]["readUsername"] = false;
    d = MessagePipeline::decide(sample("hello"), s, "hello", true, "مرحبا", "en");
    CHECK(d.tts.size() == 1);
    s["tts"]["readUsername"] = true;
    s["tts"]["readMessage"] = false;
    d = MessagePipeline::decide(sample("hello"), s, "hello", true, "مرحبا", "en");
    CHECK(d.tts.size() == 1 && d.tts[0]["text"] == "Bob:");
    s["tts"]["readMessage"] = true;

    // Long messages are skipped entirely.
    s["tts"]["maxChars"] = 10;
    d = MessagePipeline::decide(sample("this is far too long"), s, "this is far too long", true, "x", "en");
    CHECK(d.tts.is_null());
    s["tts"]["maxChars"] = 200;

    // Commands, ignored users, history, disabled platform.
    CHECK(MessagePipeline::decide(sample("!uptime"), s, "!uptime", true, "", "en").tts.is_null());
    auto bot = sample("hi");
    bot.username = "Nightbot";
    CHECK(MessagePipeline::decide(bot, s, "hi", true, "", "en").tts.is_null());
    auto hist = sample("hi");
    hist.history = true;
    CHECK(MessagePipeline::decide(hist, s, "hi", true, "", "en").tts.is_null());
    s["tts"]["platforms"]["twitch"] = false;
    CHECK(MessagePipeline::decide(sample("hi"), s, "hi", true, "", "en").tts.is_null());
    s["tts"]["platforms"]["twitch"] = true;

    // Detection failed: fall back to script guess, never show a translation.
    d = MessagePipeline::decide(sample("hello"), s, "hello", false, "", "");
    CHECK(!d.showTranslation && d.tts[1]["lang"] == "en");

    CHECK(MessagePipeline::stripLinks("look https://x.com/a here") == "look  here");
}

void testBotFilter() {
    json bots = Settings::defaults()["bots"];
    auto from = [](const std::string& user, const std::string& text = "hi") {
        ChatMessage m;
        m.platform = "kick";
        m.username = m.displayName = user;
        m.parts.push_back({MessagePart::Type::Text, text, ""});
        return m;
    };
    CHECK(BotFilter::reason(from("Nightbot"), bots) == "known bot");
    CHECK(BotFilter::reason(from("@BotRix"), bots) == "known bot");
    CHECK(BotFilter::reason(from("StreamElements"), bots) == "known bot");
    CHECK(BotFilter::reason(from("coolchannel_bot"), bots) == "name ends with bot");
    CHECK(BotFilter::reason(from("RealViewer"), bots).empty());
    CHECK(BotFilter::reason(from("bot"), bots).empty()); // too short for the suffix rule

    auto badge = from("Helper");
    badge.roles.push_back("bot");
    CHECK(BotFilter::reason(badge, bots) == "bot badge");

    bots["allowedUsers"] = json::array({"Abbot"});
    CHECK(BotFilter::reason(from("abbot"), bots).empty());

    bots["customBots"] = json::array({"MyHelper"});
    CHECK(BotFilter::reason(from("myhelper"), bots) == "custom bot list");

    CHECK(BotFilter::reason(from("viewer", "!uptime"), bots).empty());
    bots["hideCommands"] = true;
    CHECK(BotFilter::reason(from("viewer", "!uptime"), bots) == "chat command");

    bots["enabled"] = false;
    CHECK(BotFilter::reason(from("Nightbot"), bots).empty());
}

void testTtsTextFilter() {
    TtsTextFilter::Options o;
    // Emoji, Kick tokens, YouTube shortcodes, built-in and guessed emote words.
    CHECK(TtsTextFilter::clean("hello 😂😂 world 🇸🇦", o) == "hello world");
    CHECK(TtsTextFilter::clean("gg [emote:37226:KEKW] nice", o) == "gg nice");
    CHECK(TtsTextFilter::clean("hi :face-blue-smiling: there", o) == "hi there");
    CHECK(TtsTextFilter::clean("KEKW that was OMEGALUL funny", o) == "that was funny");
    CHECK(TtsTextFilter::clean("catJAM monkaS xqcL lets go", o) == "lets go");
    CHECK(TtsTextFilter::clean("I love my iPhone and eBay", o) == "I love my iPhone and eBay"); // not emote codes
    CHECK(TtsTextFilter::clean("😂 KEKW 🔥", o).empty());      // nothing left to read
    CHECK(TtsTextFilter::clean("⣿⣿⣿⣿ ⠀⠀", o).empty());         // braille ASCII art
    CHECK(TtsTextFilter::clean("السلام عليكم 🌹", o) == "السلام عليكم");
    o.extraEmoteWords = json::array({"myEmote"});
    CHECK(TtsTextFilter::clean("wow myemote", o) == "wow");
    o.guessEmoteWords = false;
    CHECK(TtsTextFilter::clean("lirikHYPE", o) == "lirikHYPE"); // guessing off

    EmoteRegistry reg;
    reg.add("twitch:42", {{"Clap", "https://cdn/1"}});
    TtsTextFilter::Options r;
    r.registry = &reg;
    r.platform = "twitch";
    r.channelId = "42";
    CHECK(TtsTextFilter::clean("nice Clap stream", r) == "nice stream");
    r.channelId = "other";
    CHECK(TtsTextFilter::clean("nice Clap stream", r) == "nice Clap stream"); // channel-specific

    // Link detection.
    CHECK(TtsTextFilter::containsLink("go to https://example.com now"));
    CHECK(TtsTextFilter::containsLink("join discord.gg/abc"));
    CHECK(TtsTextFilter::containsLink("www.site.org"));
    CHECK(TtsTextFilter::containsLink("follow me on instagram.com/x"));
    CHECK(TtsTextFilter::containsLink("youtu.be/dQw4w9WgXcQ"));
    CHECK(!TtsTextFilter::containsLink("version 3.5 is out, e.g. today"));
    CHECK(!TtsTextFilter::containsLink("email me at name@site"));
    CHECK(!TtsTextFilter::containsLink("just do it. ok"));
}

void testEmoteRegistry() {
    json seventv = {{"emote_set", {{"emotes", json::array({{{"id", "abc"}, {"name", "catJAM"}}})}}}};
    auto l = EmoteRegistry::parse7tv(seventv);
    CHECK(l.size() == 1 && l[0].first == "catJAM" && l[0].second == "https://cdn.7tv.app/emote/abc/1x.webp");
    json bttv = {{"channelEmotes", json::array({{{"id", "1"}, {"code", "Clap"}}})},
                 {"sharedEmotes", json::array({{{"id", "2"}, {"code", "RareParrot"}}})}};
    CHECK(EmoteRegistry::parseBttv(bttv).size() == 2);
    json ffz = {{"sets", {{"3", {{"emoticons", json::array({{{"name", "ZrehplaR"}, {"urls", {{"1", "//cdn.ffz/1"}}}}})}}}}}};
    auto f = EmoteRegistry::parseFfz(ffz);
    CHECK(f.size() == 1 && f[0].second == "https://cdn.ffz/1");

    EmoteRegistry reg;
    reg.add("", l);
    ChatMessage m = sample("so good catJAM catJAM wow");
    MessagePipeline::markThirdPartyEmotes(m, reg);
    CHECK(m.parts.size() == 5);
    CHECK(m.parts[1].type == MessagePart::Type::Emote && m.parts[1].text == "catJAM");
    CHECK(m.speakableText() == "so good wow");
    CHECK(m.plainText() == "so good catJAM catJAM wow");
}

void testTtsSkipRules() {
    json s = Settings::defaults();
    s["tts"]["enabled"] = true;

    // Messages with links are skipped entirely.
    CHECK(MessagePipeline::decide(sample("check https://x.com/a now"), s, "check https://x.com/a now", true, "", "en").tts.is_null());
    CHECK(MessagePipeline::decide(sample("join discord.gg/abc"), s, "join discord.gg/abc", true, "", "en").tts.is_null());
    s["tts"]["skipLinkMessages"] = false; // then only the link is removed
    auto d = MessagePipeline::decide(sample("check https://x.com/a now"), s, "check https://x.com/a now", true, "", "en");
    CHECK(d.tts.size() == 2 && d.tts[1]["text"] == "check now");
    s["tts"]["skipLinkMessages"] = true;

    // Bots are never read even when the display filter is off.
    s["bots"]["enabled"] = false;
    auto bot = sample("Follow the channel!");
    bot.username = bot.displayName = "Nightbot";
    CHECK(MessagePipeline::decide(bot, s, "Follow the channel!", true, "", "en").tts.is_null());
    bot.username = bot.displayName = "BotRix";
    CHECK(MessagePipeline::decide(bot, s, "Follow the channel!", true, "", "en").tts.is_null());
    s["tts"]["skipBots"] = false;
    CHECK(!MessagePipeline::decide(bot, s, "Follow the channel!", true, "", "en").tts.is_null());
    s["tts"]["skipBots"] = true;

    // Emote-only message: nothing is read (not even the username).
    CHECK(MessagePipeline::decide(sample("KEKW 😂"), s, "KEKW 😂", true, "", "en").tts.is_null());
    // Emotes are removed from the spoken text.
    d = MessagePipeline::decide(sample("that was PogChamp amazing 🔥"), s, "that was PogChamp amazing 🔥", true, "", "en");
    CHECK(d.tts.size() == 2 && d.tts[1]["text"] == "that was amazing");
    // Emoji in usernames are not read; underscores become spaces.
    auto fancy = sample("hello");
    fancy.displayName = "🔥Sara_Gamer🔥";
    d = MessagePipeline::decide(fancy, s, "hello", true, "", "en");
    CHECK(d.tts[0]["text"] == "Sara Gamer:");
}

void testUtil() {
    CHECK(util::utf8Length("سلام") == 4);
    auto parts = util::splitForSpeech(std::string(250, 'a') + " " + std::string(10, 'b'), 180);
    CHECK(parts.size() == 2);
    for (auto& p : parts) CHECK(util::utf8Length(p) <= 180);
    CHECK(util::base64Decode("aGVsbG8=") == "hello");
    CHECK(util::urlEncode("a b&c") == "a%20b%26c");
}

void testSettingsSanitize() {
    auto path = (std::filesystem::temp_directory_path() / "usc_test_settings.json").string();
    Settings s(path);
    json r = s.applyPatch({{"tts", {{"maxChars", "oops"}, {"volume", 0.5}}}, {"bogus", 1}});
    CHECK(r["tts"]["maxChars"] == 200); // wrong type ignored
    CHECK(r["tts"]["volume"] == 0.5);
    CHECK(!r.contains("bogus"));
    Settings reloaded(path);
    reloaded.load();
    CHECK(reloaded.snapshot()["tts"]["volume"] == 0.5);
    std::filesystem::remove(path);
}
} // namespace

int main() {
    testTwitchParse();
    testKickParse();
    testYouTubeIds();
    testPipelineDecisions();
    testUtil();
    testBotFilter();
    testTtsTextFilter();
    testEmoteRegistry();
    testTtsSkipRules();
    testSettingsSanitize();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
