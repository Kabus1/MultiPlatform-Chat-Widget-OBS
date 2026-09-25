#include "core/BotFilter.hpp"

#include "core/Util.hpp"

#include <algorithm>

namespace usc {

using nlohmann::json;

std::string BotFilter::normalizeName(const std::string& name) {
    std::string n = util::toLower(util::trim(name));
    if (!n.empty() && n[0] == '@') n.erase(0, 1);
    n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
    return n;
}

namespace {

bool inList(const json& list, const std::string& a, const std::string& b) {
    if (!list.is_array()) return false;
    for (const auto& item : list) {
        if (!item.is_string()) continue;
        std::string n = BotFilter::normalizeName(item.get<std::string>());
        if (!n.empty() && (n == a || n == b)) return true;
    }
    return false;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

std::string BotFilter::reason(const ChatMessage& m, const json& bots) {
    if (!bots.is_object() || !bots.value("enabled", true)) return {};
    const std::string user = normalizeName(m.username);
    const std::string display = normalizeName(m.displayName);

    if (inList(bots.value("allowedUsers", json::array()), user, display)) return {};
    if (inList(bots.value("knownBots", json::array()), user, display)) return "known bot";
    if (inList(bots.value("customBots", json::array()), user, display)) return "custom bot list";

    if (bots.value("useBadges", true) &&
        std::find(m.roles.begin(), m.roles.end(), "bot") != m.roles.end())
        return "bot badge";

    if (bots.value("nameSuffixRule", true)) {
        for (const std::string& n : {user, display}) {
            // Only a literal "bot" ending ("nightbot", "my_bot"); very short
            // names are ignored. Real users caught by this go in allowedUsers.
            if (n.size() > 4 && (endsWith(n, "bot") || endsWith(n, "_bot") || endsWith(n, "-bot")))
                return "name ends with bot";
        }
    }

    if (bots.value("hideCommands", false)) {
        std::string text = util::trim(m.plainText());
        if (!text.empty() && text[0] == '!') return "chat command";
    }
    return {};
}

} // namespace usc
