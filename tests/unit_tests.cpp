// Minimal self-contained unit tests (no network): run with `ctest` or directly.
#include "capture/CaptureRelay.hpp"
#include "capture/CaptureTypes.hpp"
#include "capture/Guard.hpp"
#include "core/App.hpp"
#include "core/BotFilter.hpp"
#include "core/EmoteRegistry.hpp"
#include "core/LanguageTools.hpp"
#include "core/TtsTextFilter.hpp"
#include "core/MessagePipeline.hpp"
#include "core/Settings.hpp"
#include "core/Util.hpp"
#include "net/OAuth.hpp"
#include "net/Protobuf.hpp"
#include "platforms/KickClient.hpp"
#include "platforms/TikTokClient.hpp"
#include "integrations/ObsWatcher.hpp"

#include <miniz.h>
#include "platforms/TwitchClient.hpp"
#include "platforms/YouTubeClient.hpp"

#include <filesystem>
#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>

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

void testArabicDetection() {
    CHECK(lang::isArabicText("هاي"));
    CHECK(lang::isArabicText("هاي شباب"));
    CHECK(lang::isArabicText("شلونك"));
    CHECK(lang::isArabicText("شگد حلو"));       // Iraqi گ is still Arabic
    CHECK(lang::isArabicText("چا وين رحت"));    // Gulf / Iraqi چ
    CHECK(lang::isArabicText("هلا والله 😂"));
    CHECK(lang::isArabicText("هاي bro"));       // mostly Arabic
    CHECK(lang::isArabicText("مرحبا"));
    CHECK(!lang::isArabicText("سلام خوبی؟"));   // Persian (Farsi yeh)
    CHECK(!lang::isArabicText("کجایی"));         // Persian
    CHECK(!lang::isArabicText("hello there"));
    CHECK(!lang::isArabicText("good game يا شباب ya all")); // mostly English
}

void testSlang() {
    auto r = lang::expandSlang("idk bro");
    CHECK(r.text == "I don't know, brother" && r.replaced == 2 && r.englishLike);
    CHECK(lang::expandSlang("gg wp").text == "good game, well played");
    CHECK(lang::expandSlang("idk bro, gg wp ngl").text == "I don't know, brother, good game, well played, honestly");
    CHECK(lang::expandSlang("lol").text == "haha");
    CHECK(lang::expandSlang("LMAO!!").text == "haha!!");
    CHECK(lang::expandSlang("W stream").text == "win stream");
    CHECK(lang::expandSlang("tbh u r the best").text == "honestly, you are the best");
    CHECK(lang::expandSlang("ty u r the goat").text == "thank you, you are the greatest of all time");
    CHECK(lang::expandSlang("brb, ty").text == "be right back, thank you");
    // Ambiguous single letters are left alone in non-English text.
    CHECK(lang::expandSlang("hola y adios").text == "hola y adios");
    CHECK(lang::expandSlang("wc all", json::array({"wc = welcome"})).text == "welcome all");
    CHECK(lang::expandSlang("hello everyone").replaced == 0);
}

void testArabicAndSlangDecisions() {
    json s = Settings::defaults();
    s["tts"]["enabled"] = true;

    // Arabic dialect: Google mis-detects as Persian and "translates" -> ignored.
    auto d = MessagePipeline::decide(sample("هاي"), s, "هاي", true, "مرحبًا", "fa");
    CHECK(!d.showTranslation);
    CHECK(d.tts[1]["text"] == "هاي" && d.tts[1]["lang"] == "ar");
    s["tts"]["mode"] = "translated";
    d = MessagePipeline::decide(sample("هاي شباب"), s, "هاي شباب", true, "مرحبا يا شباب", "ar");
    CHECK(d.tts[1]["text"] == "هاي شباب"); // read exactly as written
    s["translation"]["showForAllLanguages"] = true;
    d = MessagePipeline::decide(sample("شلونك"), s, "شلونك", true, "كيف حالك", "ku");
    CHECK(!d.showTranslation && d.tts[1]["text"] == "شلونك");
    s["translation"]["showForAllLanguages"] = false;
    s["tts"]["mode"] = "original";

    // Slang: expanded text was translated -> Arabic line is shown.
    d = MessagePipeline::decide(sample("idk bro"), s, "idk bro", true, "لا أعرف يا أخي", "en", nullptr,
                                "I don't know brother");
    CHECK(d.showTranslation);
    CHECK(d.tts[1]["text"] == "I don't know brother" && d.tts[1]["lang"] == "en");
    s["tts"]["mode"] = "translated";
    d = MessagePipeline::decide(sample("idk bro"), s, "idk bro", true, "لا أعرف يا أخي", "en", nullptr,
                                "I don't know brother");
    CHECK(d.tts[1]["text"] == "لا أعرف يا أخي" && d.tts[1]["lang"] == "ar"); // same text as the display line
    s["tts"]["mode"] = "original";

    // Google echoing the input back is never shown as a "translation".
    d = MessagePipeline::decide(sample("xqc"), s, "xqc", true, "xqc", "en");
    CHECK(!d.showTranslation);
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

void testKickLookup() {
    CHECK(KickClient::normalizeSlug("https://kick.com/QFazek?tab=chat") == "qfazek");
    CHECK(KickClient::normalizeSlug(" @QFazek ") == "qfazek");
    CHECK(KickClient::normalizeSlug("qfazek") == "qfazek");

    // /api/v2/channels/{slug}
    auto r = KickClient::parseLookupResponse(200, R"({"id":1,"user_id":55,"slug":"qfazek","chatroom":{"id":999,"chatable_id":1}})");
    CHECK(r.chatroomId == "999" && r.userId == "55");
    // /api/v2/channels/{slug}/chatroom
    r = KickClient::parseLookupResponse(200, R"({"id":777,"chatable_type":"App\\Models\\Channel","chatable_id":1,"slow_mode":false})");
    CHECK(r.chatroomId == "777");
    // Cloudflare challenge page.
    r = KickClient::parseLookupResponse(403, "<!DOCTYPE html><title>Just a moment...</title>");
    CHECK(r.blocked && r.chatroomId.empty());
    r = KickClient::parseLookupResponse(200, "<html><script>window._cf_chl_opt={}</script>");
    CHECK(r.blocked);
    r = KickClient::parseLookupResponse(404, "{}");
    CHECK(r.notFound);
    // Things a user may paste.
    CHECK(KickClient::parseLookupResponse(200, "  4567890 \n").chatroomId == "4567890");
    r = KickClient::parseLookupResponse(200, "Raw Data Headers\n{\"id\":1,\"user_id\":9,\"chatroom\":{\"id\":321,\"slow_mode\":false}} Pretty-print");
    CHECK(r.chatroomId == "321" && r.userId == "9");

    // Legacy ChatMessageSentEvent shape.
    json legacy = {{"message", {{"id", "abc"}, {"message", "hello [emote:1:Wave]"}}},
                   {"user", {{"id", 5}, {"username", "Viewer"}, {"identity", {{"color", "#FF0000"}}}}}};
    auto m = KickClient::parseChatEvent(legacy);
    CHECK(m && m->displayName == "Viewer" && m->id == "abc" && m->parts.size() == 2 && m->userColor == "#FF0000");
    // Numeric ids are accepted too.
    json v2 = {{"id", 123}, {"content", "hi"}, {"sender", {{"id", 7}, {"username", "A"}, {"slug", "a"}}}};
    m = KickClient::parseChatEvent(v2);
    CHECK(m && m->id == "123" && m->userId == "7");

    CHECK(KickClient::describePusherCode(4001, "").find("key") != std::string::npos);

    // Current Pusher key from web.kick.com/api/v1/realtime/connection.
    auto p = KickClient::parseRealtimeConfig(
        R"({"data":{"connections":[{"provider":"centrifugo","credentials":{"url":"wss://x"}},)"
        R"({"provider":"pusher","credentials":{"app_key":"34bf7a0ff419a2a775b9","cluster":"us2"}}]}})");
    CHECK(p && p->key == "34bf7a0ff419a2a775b9" && p->cluster == "us2");
    CHECK(!KickClient::parseRealtimeConfig(R"({"data":{"connections":[]}})"));
    CHECK(!KickClient::parseRealtimeConfig("<html>Just a moment...</html>"));
    CHECK(!KickClient::parseRealtimeConfig(
        R"({"data":{"connections":[{"provider":"pusher","credentials":{"app_key":"bad key!","cluster":"us2"}}]}})"));
    CHECK(KickClient::isRetiredPusherKey("32cbd69e4b950bf97679"));
    CHECK(!KickClient::isRetiredPusherKey(KickClient::kDefaultPusherKey));
}

std::string gzipForTest(const std::string& in) {
    size_t len = 0;
    void* raw = tdefl_compress_mem_to_heap(in.data(), in.size(), &len, TDEFL_DEFAULT_MAX_PROBES);
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    out.append(static_cast<const char*>(raw), len);
    mz_free(raw);
    auto le32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    le32(static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(in.data()), in.size())));
    le32(static_cast<uint32_t>(in.size()));
    return out;
}

void testTikTokProtocol() {
    // Push frame round trip.
    TikTokClient::PushFrame f;
    f.logId = 123456789;
    f.payloadEncoding = "pb";
    f.payloadType = "msg";
    f.payload = std::string("\x00\x01\x02", 3);
    f.headers["compress_type"] = "gzip";
    auto back = TikTokClient::parsePushFrame(TikTokClient::encodePushFrame(f));
    CHECK(back && back->logId == 123456789 && back->payloadType == "msg" && back->payload == f.payload &&
          back->headers["compress_type"] == "gzip");

    // A chat message, as TikTok sends it: frame -> gzip -> fetch result -> chat.
    std::string user = pb::Writer().varint(1, 42).bytes(3, "Nick Name").bytes(38, "nick_1").str();
    std::string common = pb::Writer().varint(2, 777).str();
    std::string chat = pb::Writer().bytes(1, common).bytes(2, user).bytes(3, "مرحبا from TikTok").str();
    std::string base = pb::Writer().bytes(1, "WebcastChatMessage").bytes(2, chat).str();
    std::string control = pb::Writer().bytes(1, "WebcastControlMessage").bytes(2, pb::Writer().varint(2, 3).str()).str();
    std::string route = pb::Writer().bytes(1, "wrss").bytes(2, "xyz").str();
    std::string result = pb::Writer()
                             .bytes(1, base)
                             .bytes(1, control)
                             .bytes(2, "cursor-1")
                             .bytes(5, "ext-1")
                             .bytes(7, route)
                             .varint(9, 1)
                             .bytes(10, "wss://push.example/ws")
                             .str();
    std::string gz = gzipForTest(result);
    std::string inflated;
    CHECK(TikTokClient::gunzip(gz, inflated) && inflated == result);
    CHECK(!TikTokClient::gunzip("not gzip at all......", inflated));

    auto res = TikTokClient::parseFetchResult(inflated);
    CHECK(res && res->messages.size() == 2 && res->cursor == "cursor-1" && res->internalExt == "ext-1" &&
          res->needsAck && res->routeParams["wrss"] == "xyz" && res->pushServer == "wss://push.example/ws");
    auto m = TikTokClient::parseChatMessage(res->messages[0].second);
    CHECK(m && m->platform == "tiktok" && m->username == "nick_1" && m->displayName == "Nick Name" &&
          m->userId == "42" && m->id == "tt-777" && m->plainText() == "مرحبا from TikTok");
    CHECK(TikTokClient::isStreamEnd(res->messages[1].second));
    CHECK(!TikTokClient::parseChatMessage(pb::Writer().bytes(2, user).str())); // no text

    // Malformed input never crashes.
    CHECK(!TikTokClient::parsePushFrame(std::string("\xff\xff\xff", 3)));

    // Heartbeat / enter-room frames are valid push frames.
    auto hb = TikTokClient::parsePushFrame(TikTokClient::heartbeatFrame("7300000000000000001"));
    CHECK(hb && hb->payloadType == "hb" && hb->payloadEncoding == "pb");
    auto enter = TikTokClient::parsePushFrame(TikTokClient::enterRoomFrame("7300000000000000001"));
    CHECK(enter && enter->payloadType == "im_enter_room");

    // Room lookup responses.
    auto room = TikTokClient::parseRoomInfo(R"({"data":{"user":{"roomId":"7300","status":2},"liveRoom":{"status":2}},"statusCode":0})");
    CHECK(room.found && room.live && room.roomId == "7300");
    room = TikTokClient::parseRoomInfo(R"({"data":{"user":{"roomId":"7300","status":4}},"statusCode":0})");
    CHECK(room.found && !room.live);
    room = TikTokClient::parseRoomInfo(R"(<script>{"liveRoomUserInfo":{"user":{"roomId":"7301","status":2}}}</script>)");
    CHECK(room.found && room.live && room.roomId == "7301");
    room = TikTokClient::parseRoomInfo(R"({"data":{},"statusCode":19881007,"message":"user_not_found"})");
    CHECK(!room.found);
}

std::string hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

void testOAuthHelpers() {
    CHECK(hex(util::sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(util::sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(util::sha256(std::string(1000, 'a'))) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    CHECK(util::base64UrlEncode("\xfb\xff") == "-_8");
    // RFC 7636 appendix B example.
    CHECK(oauth::codeChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    auto v = oauth::makeCodeVerifier();
    CHECK(v.size() >= 43 && v.size() <= 128);

    auto g = oauth::google("id.apps.googleusercontent.com", "s", "http://localhost:8787/auth/youtube/callback");
    std::string url = oauth::buildAuthUrl(g, "st", oauth::codeChallenge(v));
    CHECK(url.find("access_type=offline") != std::string::npos);
    CHECK(url.find("code_challenge_method=S256") != std::string::npos);
    CHECK(url.find("prompt=consent%20select_account") != std::string::npos);
}

void testYouTubeDetection() {
    // Live broadcast wins.
    json active = {{"items", json::array({{{"id", "v1"},
                                            {"snippet", {{"liveChatId", "chat1"}, {"title", "My stream"}}},
                                            {"status", {{"lifeCycleStatus", "live"}}}}})}};
    auto b = YouTubeClient::pickBroadcast(active, true);
    CHECK(b && b->chatId == "chat1" && b->live && b->title == "My stream");

    // Upcoming: preview ("testing") beats a scheduled "ready" one; complete is ignored.
    json upcoming = {{"items", json::array({
                         {{"id", "a"}, {"snippet", {{"liveChatId", "chatReady"}}}, {"status", {{"lifeCycleStatus", "ready"}}}},
                         {{"id", "b"}, {"snippet", {{"liveChatId", "chatTesting"}}}, {"status", {{"lifeCycleStatus", "testing"}}}},
                         {{"id", "c"}, {"snippet", {{"liveChatId", "chatDone"}}}, {"status", {{"lifeCycleStatus", "complete"}}}},
                         {{"id", "d"}, {"snippet", {{"title", "no chat"}}}, {"status", {{"lifeCycleStatus", "liveStarting"}}}},
                     })}};
    b = YouTubeClient::pickBroadcast(upcoming, false);
    CHECK(b && b->chatId == "chatTesting" && !b->live);

    CHECK(!YouTubeClient::pickBroadcast(json{{"items", json::array()}}, true));
    CHECK(!YouTubeClient::pickBroadcast(json::parse("not json", nullptr, false), true));

    CHECK(YouTubeClient::explainApiError("quotaExceeded", "X").find("quota") != std::string::npos);
    CHECK(YouTubeClient::explainApiError("liveStreamingNotEnabled", "Ch").find("'Ch'") != std::string::npos);
    CHECK(YouTubeClient::explainApiError("insufficientPermissions", "").find("Login with YouTube") != std::string::npos);
}

void testViewerConnection() {
    // Signing-free WebSocket URL: viewer identity, room id, no signature params.
    std::string url = TikTokClient::buildDirectWsUrl("7300000000000000001", 123.456);
    CHECK(url.rfind("wss://webcast-ws.tiktok.com/webcast/im/ws_proxy/ws_reuse_supplement/?", 0) == 0);
    CHECK(url.find("room_id=7300000000000000001") != std::string::npos);
    CHECK(url.find("identity=audience") != std::string::npos);
    CHECK(url.find("compress=gzip") != std::string::npos);
    CHECK(url.find("last_rtt=123.456") != std::string::npos);
    CHECK(url.find("X-Bogus") == std::string::npos && url.find("msToken") == std::string::npos &&
          url.find("signature") == std::string::npos);

    CHECK(TikTokClient::extractTtwid({"tt_csrf_token=x; path=/", "ttwid=1%7Cabc%7C123; Path=/; Domain=tiktok.com; HttpOnly"}) ==
          "1%7Cabc%7C123");
    CHECK(TikTokClient::extractTtwid({"msToken=1; path=/"}).empty());
    CHECK(TikTokClient::normalizeUser("https://www.tiktok.com/@IsKorv/live") == "iskorv");
    CHECK(TikTokClient::normalizeUser(" @iskorv ") == "iskorv");

    // Live check: live when either status is 2; roomId "0" is never live.
    auto r = TikTokClient::parseRoomInfo(R"({"statusCode":0,"data":{"user":{"roomId":"73","status":2},"liveRoom":{"status":4}}})");
    CHECK(r.found && r.live);
    r = TikTokClient::parseRoomInfo(R"({"statusCode":0,"data":{"user":{"roomId":"73","status":4},"liveRoom":{"status":2}}})");
    CHECK(r.found && r.live);
    r = TikTokClient::parseRoomInfo(R"({"statusCode":0,"data":{"user":{"roomId":"0","status":2}}})");
    CHECK(r.found && !r.live);
    r = TikTokClient::parseRoomInfo(R"({"statusCode":19881007,"data":{}})");
    CHECK(r.notFound && !r.found);
    r = TikTokClient::parseRoomInfo(R"({"statusCode":10101,"message":"busy"})");
    CHECK(!r.found && !r.notFound && r.error.find("10101") != std::string::npos);

    // STREAM_ENDED (3) and STREAM_ENDED_BAN (4) both end the session; pause (1) does not.
    CHECK(TikTokClient::isStreamEnd(pb::Writer().varint(2, 4).str()));
    CHECK(!TikTokClient::isStreamEnd(pb::Writer().varint(2, 1).str()));

    // obs-websocket v5 authentication (value computed independently in Python).
    CHECK(ObsWatcher::authResponse("supersecretpassword", "lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI=",
                                   "+IxH4CnCiqpX1rM9scsNynZzbOe4KhDeYcTNS3PDaeY=") ==
          "1Ct943GAT+6YQUUX47Ia/ncufilbe6+oD6lY+5kaCu4=");
    CHECK(util::base64Encode("ab") == "YWI=" && util::base64Encode("abc") == "YWJj");
}

void testCaptureDecisions() {
    using namespace usc::capture;
    auto mk = [](int w, int h, uint32_t n, uint32_t d, const char* st) { return Mode{w, h, n, d, st}; };
    std::vector<Mode> modes = {
        mk(3840, 2160, 60, 1, "NV12"),  mk(3840, 2160, 30, 1, "NV12"), mk(1920, 1080, 120, 1, "NV12"),
        mk(1920, 1080, 60, 1, "NV12"),  mk(1920, 1080, 60, 1, "MJPG"), mk(1920, 1080, 60, 1, "NV12"), // duplicate
        mk(1280, 720, 144, 1, "YUY2"),  mk(1920, 1080, 60000, 1001, "YUY2"),
    };
    normalizeModes(modes);
    CHECK(modes.size() == 7);
    CHECK(modes.front().height == 2160 && modes.front().fpsNum == 60);

    RelayConfig cfg;
    std::string note;
    // Match source: the most detail the device offers.
    auto m = chooseMode(modes, cfg, note);
    CHECK(m && m->height == 2160 && m->fps() == 60 && note.empty());
    // 1080p output at 120: capture 1080p120, not 4K.
    cfg.outputResolution = "1080p";
    cfg.outputFps = 120;
    m = chooseMode(modes, cfg, note);
    CHECK(m && m->height == 1080 && m->fps() == 120);
    // 1080p60: cheapest format among exact 60 fps modes, never MJPG.
    cfg.outputFps = 60;
    m = chooseMode(modes, cfg, note);
    CHECK(m && m->height == 1080 && m->fps() == 60 && m->subtype == "NV12");
    // 144 fps is only possible at 720p: reaching the FPS beats resolution.
    cfg.outputFps = 144;
    m = chooseMode(modes, cfg, note);
    CHECK(m && m->height == 720 && m->fps() == 144);
    // A saved mode that is offered is used as-is ...
    cfg.mode = mk(1920, 1080, 60, 1, "MJPG").key();
    m = chooseMode(modes, cfg, note);
    CHECK(m && m->subtype == "MJPG" && note.empty());
    // ... one that is not falls back to auto with an explanation.
    cfg.mode = "2560x1440@240/1:NV12";
    m = chooseMode(modes, cfg, note);
    CHECK(m && m->height == 720 && !note.empty());
    // Never returns something the device does not offer.
    CHECK(!chooseMode({}, cfg, note));

    RelayConfig out;
    CHECK(outputSize(out, 1920, 1080) == std::make_pair(1920, 1080));
    out.outputResolution = "720p";
    CHECK(outputSize(out, 1920, 1080) == std::make_pair(1280, 720));
    out.outputResolution = "2160p";
    CHECK(outputSize(out, 1920, 1080) == std::make_pair(3840, 2160));
    CHECK(outputSize(out, 2560, 1080).first % 2 == 0);

    // 120 fps capped at 60: every other frame, no drift over 10 s.
    FramePacer pacer(60);
    int shown = 0;
    for (int i = 0; i < 1200; ++i) shown += pacer.shouldPresent(int64_t(i) * 1000000 / 120);
    CHECK(shown >= 595 && shown <= 605);
    // 60 fps (with jitter) capped at 60: nothing dropped.
    pacer.reset(60);
    shown = 0;
    for (int i = 0; i < 600; ++i) shown += pacer.shouldPresent(int64_t(i) * 16667 + (i % 2 ? 900 : -900));
    CHECK(shown == 600);
    // No cap: everything.
    pacer.reset(0);
    CHECK(pacer.shouldPresent(0) && pacer.shouldPresent(1));

    RateMeter rm;
    for (int i = 0; i < 60; ++i) rm.tick(10000 + i * 16);
    CHECK(rm.rate(10000 + 60 * 16) > 50);
    CHECK(rm.rate(20000) == 0);

    RelayConfig a = RelayConfig::fromJson({{"enabled", true}, {"deviceId", "x"}, {"outputFps", 120}});
    CHECK(a.enabled && a.deviceId == "x" && a.outputFps == 120 && a.mode == "auto");
    RelayConfig b = a;
    b.deviceName = "renamed";
    CHECK(a.sameSession(b));
    b.outputFps = 60;
    CHECK(!a.sameSession(b));
}

void testCaptureSafety() {
    using namespace usc::capture;
    namespace fs = std::filesystem;
    // A hand-edited / damaged settings file never throws.
    RelayConfig bad = RelayConfig::fromJson(
        {{"enabled", "yes"}, {"deviceId", 5}, {"mode", nullptr}, {"outputFps", "120"}, {"keepAspect", 1}, {"windowTitle", ""}});
    CHECK(!bad.enabled && bad.deviceId.empty() && bad.mode == "auto" && bad.outputFps == 0 && bad.keepAspect &&
          bad.windowTitle == "USC Capture Output");
    CHECK(RelayConfig::fromJson({{"outputFps", -5}}).outputFps == 0);
    CHECK(!RelayConfig::fromJson(nullptr).enabled);

    // The guard turns exceptions into errors.
    std::string err;
    CHECK(guarded([] {}, err) && err.empty());
    CHECK(!guarded([] { throw std::runtime_error("boom"); }, err) && err.find("boom") != std::string::npos);
    CHECK(!guarded([] { throw 42; }, err) && !err.empty());

    // Start -> Stop -> Start cycles, destruction while active, crash marker.
    fs::path dir = fs::temp_directory_path() / "usc_capture_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string marker = (dir / "capture-session.marker").string();
    {
        std::atomic<int> disabled{0}, events{0};
        CaptureRelay relay([&](const nlohmann::json&) { ++events; }, [&] { ++disabled; }, marker);
        relay.start();
        RelayConfig on;
        on.enabled = true;
        on.deviceId = "dev";
        for (int i = 0; i < 5; ++i) {
            relay.apply(on);
            RelayConfig off = on;
            off.enabled = false;
            relay.apply(off);
        }
        relay.apply(on);
        relay.refreshDevices();
        relay.modes("dev");
        for (int i = 0; i < 100 && relay.status()["state"] != "error"; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto st = relay.status();
        CHECK(st["state"] == "error"); // stub backend: a clean error, not a crash
        CHECK(!st["supported"].get<bool>());
        CHECK(!fs::exists(marker)); // a clean failure leaves no crash marker
        CHECK(events > 0);
        // destroyed while enabled
    }
    CHECK(!fs::exists(marker));
    // A leftover marker (process died) is reported once.
    { std::ofstream(marker) << "x"; }
    CHECK(CaptureRelay::consumeCrashMarker(marker));
    CHECK(!fs::exists(marker));
    CHECK(!CaptureRelay::consumeCrashMarker(marker));

    // App launch policy: a saved "enabled" never auto-starts by itself, and a
    // crash marker blocks auto-start even when it is on.
    auto writeSettings = [&](bool autoStart) {
        nlohmann::json j = usc::Settings::defaults();
        j["capture"]["enabled"] = true;
        j["capture"]["autoStart"] = autoStart;
        j["capture"]["deviceId"] = "dev";
        std::ofstream(dir / "settings.json") << j.dump();
    };
    writeSettings(false);
    {
        usc::App app(dir.string(), 18799);
        CHECK(app.settings().snapshot()["capture"]["enabled"] == false);
        CHECK(app.settings().snapshot()["capture"]["deviceId"] == "dev"); // choices are kept
    }
    writeSettings(true);
    { std::ofstream(marker) << "x"; }
    {
        usc::App app(dir.string(), 18799);
        CHECK(app.settings().snapshot()["capture"]["enabled"] == false);
        CHECK(app.capture().status()["detail"].get<std::string>().find("unexpectedly") != std::string::npos);
        CHECK(!fs::exists(marker));
    }
    fs::remove_all(dir);
}

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
    testArabicDetection();
    testSlang();
    testArabicAndSlangDecisions();
    testKickLookup();
    testTikTokProtocol();
    testOAuthHelpers();
    testYouTubeDetection();
    testViewerConnection();
    testSettingsSanitize();
    testCaptureDecisions();
    testCaptureSafety();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
