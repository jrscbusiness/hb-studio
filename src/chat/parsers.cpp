#include "chat/parsers.hpp"

#include "core/json.hpp"

namespace hb::chat {
namespace {

using Json = hb::core::json::Value;

const Json* member(const Json* o, const char* k) { return o ? o->get(k) : nullptr; }
std::string str(const Json* o, const char* k) {
    const Json* v = member(o, k);
    return v && v->isStr() ? v->s : std::string();
}
bool flag(const Json* o, const char* k) {
    const Json* v = member(o, k);
    return v && v->isBool() && v->b;
}

bool parseObj(std::string_view json, Json& root) {
    return hb::core::json::parse(json, root) && root.isObj();
}

}  // namespace

bool parseYouTubeChatPage(std::string_view json, std::int64_t receivedMs, YouTubeChatPage& out) {
    out = YouTubeChatPage{};
    Json root;
    if (!parseObj(json, root)) return false;
    const Json* items = root.get("items");
    if (!items || !items->isArr()) return false;
    out.nextPageToken = str(&root, "nextPageToken");
    if (const Json* p = root.get("pollingIntervalMillis"); p && p->isNum() && p->n >= 0 && p->n < 3.6e6)
        out.pollingIntervalMillis = static_cast<std::int64_t>(p->n);
    out.offline = !str(&root, "offlineAt").empty();

    for (const Json& it : items->a) {
        const Json* snip = it.get("snippet");
        const Json* auth = it.get("authorDetails");
        if (!it.isObj() || !snip || !auth) { ++out.skippedEvents; continue; }
        const std::string type = str(snip, "type");
        ChatMessage m;
        m.platform = Platform::YouTube;
        m.id = sanitizeText(str(&it, "id"), 128);
        m.authorId = sanitizeText(str(auth, "channelId"), 64);
        m.author = sanitizeText(str(auth, "displayName"), 64);
        m.avatarUrl = sanitizeText(str(auth, "profileImageUrl"), 300);
        if (m.avatarUrl.rfind("https://", 0) != 0) m.avatarUrl.clear();  // only ever hand https URLs to the UI
        if (flag(auth, "isChatOwner")) m.badges |= kBadgeOwner;
        if (flag(auth, "isChatModerator")) m.badges |= kBadgeMod;
        if (flag(auth, "isChatSponsor")) m.badges |= kBadgeMember;
        if (flag(auth, "isVerified")) m.badges |= kBadgeVerified;
        m.tsMs = receivedMs;
        if (std::int64_t t = 0; parseIso8601Ms(str(snip, "publishedAt"), t)) m.tsMs = t;

        std::string text;
        if (type == "textMessageEvent") {
            text = str(snip, "displayMessage");
            if (text.empty()) text = str(member(snip, "textMessageDetails"), "messageText");
        } else if (type == "superChatEvent") {
            const Json* d = member(snip, "superChatDetails");
            text = "[Super Chat " + str(d, "amountDisplayString") + "]";
            const std::string c = str(d, "userComment");
            if (!c.empty()) text += " " + c;
        } else if (type == "superStickerEvent") {
            const Json* d = member(snip, "superStickerDetails");
            text = "[Super Sticker " + str(d, "amountDisplayString") + "]";
        } else if (type == "newSponsorEvent") {
            text = "[Novo membro do canal]";
        } else if (type == "memberMilestoneChatEvent") {
            const Json* d = member(snip, "memberMilestoneChatDetails");
            text = "[Marco de membro] " + str(d, "userComment");
        } else {
            ++out.skippedEvents;  // messageDeleted, userBanned, poll, tombstone, chatEnded, ...
            continue;
        }
        m.text = sanitizeText(text, 500);
        if (m.id.empty() || m.author.empty() || m.text.empty()) { ++out.skippedEvents; continue; }
        out.messages.push_back(std::move(m));
    }
    return true;
}

bool parseYouTubeActiveChatId(std::string_view json, std::string& chatId) {
    chatId.clear();
    Json root;
    if (!parseObj(json, root)) return false;
    const Json* items = root.get("items");
    if (!items || !items->isArr()) return false;
    if (items->a.empty()) return true;  // video not found / not visible: chatId stays empty
    chatId = sanitizeText(str(member(&items->a[0], "liveStreamingDetails"), "activeLiveChatId"), 200);
    return true;
}

bool parseGoogleError(std::string_view json, ApiError& out) {
    out = ApiError{};
    Json root;
    if (!parseObj(json, root)) return false;
    const Json* e = root.get("error");
    if (!e || !e->isObj()) return false;
    if (const Json* c = e->get("code"); c && c->isNum()) out.code = static_cast<int>(c->n);
    out.message = sanitizeText(str(e, "message"), 300);
    if (const Json* errs = e->get("errors"); errs && errs->isArr() && !errs->a.empty())
        out.reason = sanitizeText(str(&errs->a[0], "reason"), 64);
    return true;
}

bool parseFacebookComments(std::string_view json, std::int64_t receivedMs, GraphCommentsPage& out) {
    out = GraphCommentsPage{};
    Json root;
    if (!parseObj(json, root)) return false;
    const Json* data = root.get("data");
    if (!data || !data->isArr()) return false;
    for (const Json& c : data->a) {
        ChatMessage m;
        m.platform = Platform::Facebook;
        m.id = sanitizeText(str(&c, "id"), 128);
        m.text = sanitizeText(str(&c, "message"), 500);
        if (m.id.empty() || m.text.empty()) continue;  // photo/sticker-only comments have no message
        const Json* from = c.get("from");
        m.author = sanitizeText(str(from, "name"), 64);
        m.authorId = sanitizeText(str(from, "id"), 64);
        if (m.author.empty()) m.author = "Usu\xC3\xA1rio do Facebook";
        m.avatarUrl = sanitizeText(str(member(member(from, "picture"), "data"), "url"), 300);
        if (m.avatarUrl.rfind("https://", 0) != 0) m.avatarUrl.clear();
        m.tsMs = receivedMs;
        if (std::int64_t t = 0; parseIso8601Ms(str(&c, "created_time"), t)) m.tsMs = t;
        out.messages.push_back(std::move(m));
    }
    return true;
}

bool parseGraphError(std::string_view json, GraphError& out) {
    out = GraphError{};
    Json root;
    if (!parseObj(json, root)) return false;
    const Json* e = root.get("error");
    if (!e || !e->isObj()) return false;
    if (const Json* c = e->get("code"); c && c->isNum()) out.code = static_cast<int>(c->n);
    if (const Json* c = e->get("error_subcode"); c && c->isNum()) out.subcode = static_cast<int>(c->n);
    out.message = sanitizeText(str(e, "message"), 300);
    return true;
}

bool parseInstagramLiveWebhook(std::string_view json, std::int64_t receivedMs, std::vector<ChatMessage>& out) {
    Json root;
    if (!parseObj(json, root)) return false;
    const Json* entries = root.get("entry");
    if (!entries || !entries->isArr()) return false;
    for (const Json& e : entries->a) {
        const Json* changes = e.get("changes");
        if (!changes || !changes->isArr()) continue;
        for (const Json& ch : changes->a) {
            if (str(&ch, "field") != "live_comments") continue;
            const Json* v = ch.get("value");
            if (!v || !v->isObj()) continue;
            ChatMessage m;
            m.platform = Platform::Instagram;
            m.id = sanitizeText(str(v, "id"), 128);
            m.text = sanitizeText(str(v, "text"), 500);
            const Json* from = v->get("from");
            m.author = sanitizeText(str(from, "username"), 64);
            m.authorId = sanitizeText(str(from, "id"), 64);
            if (m.id.empty() || m.text.empty() || m.author.empty()) continue;
            m.tsMs = receivedMs;
            out.push_back(std::move(m));
        }
    }
    return true;
}

}  // namespace hb::chat
