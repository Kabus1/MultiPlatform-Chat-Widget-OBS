// Minimal self-contained unit tests (no network): run with `ctest` or directly.
#include "core/BotFilter.hpp"
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
    testSettingsSanitize();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
