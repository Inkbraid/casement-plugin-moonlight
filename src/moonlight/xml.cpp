#include "moonlight/xml.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <pugixml.hpp>

namespace moonlight {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

template <class T>
std::optional<T> number(std::string_view s) {
    s = trim(s);
    T value{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
    return value;
}

bool iequals(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        auto l = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        return l(x) == l(y);
    });
}

/// Parses the document and finds its <root>; nullptr result when that fails.
pugi::xml_node load(pugi::xml_document& doc, std::string_view xml) {
    pugi::xml_parse_result r = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!r) return {};
    return doc.child("root");
}

Status status_of(pugi::xml_node root) {
    Status s;
    pugi::xml_attribute code = root.attribute("status_code");
    // GFE has been seen sending 0xFFFFFFFF, so read it unsigned.
    if (auto u = number<std::uint32_t>(code.value())) s.code = static_cast<int>(*u);
    else if (auto i = number<int>(code.value())) s.code = *i;
    s.message = root.attribute("status_message").value();
    return s;
}

}  // namespace

Reply Reply::parse(std::string_view xml) {
    pugi::xml_document doc;
    pugi::xml_node root = load(doc, xml);
    if (!root) throw ReplyError("The host's reply isn't GameStream XML.");
    Status status = status_of(root);
    if (status.code != 200) throw HostError(status.code, status.message);

    Reply reply;
    struct Walker : pugi::xml_tree_walker {
        std::vector<Element>* out;
        bool for_each(pugi::xml_node& node) override {
            if (node.type() == pugi::node_element) out->push_back({node.name(), node.child_value()});
            return true;
        }
    } walker;
    walker.out = &reply.all_;
    root.traverse(walker);
    for (pugi::xml_node child : root.children()) {
        if (child.type() != pugi::node_element) continue;
        std::vector<Element> elements;
        for (pugi::xml_node grandchild : child.children())
            if (grandchild.type() == pugi::node_element)
                elements.push_back({grandchild.name(), grandchild.child_value()});
        reply.children_.emplace_back(child.name(), std::move(elements));
    }
    return reply;
}

std::optional<std::string> Reply::text(std::string_view name) const {
    for (const Element& e : all_)
        if (e.name == name) return e.text;
    return std::nullopt;
}

std::string Reply::get(std::string_view name) const { return text(name).value_or(std::string()); }

std::vector<std::vector<Reply::Element>> Reply::groups(std::string_view name) const {
    std::vector<std::vector<Element>> out;
    for (const auto& [child, elements] : children_)
        if (child == name) out.push_back(elements);
    return out;
}

Status reply_status(std::string_view xml) {
    pugi::xml_document doc;
    pugi::xml_node root = load(doc, xml);
    if (!root) return {};
    return status_of(root);
}

int ServerInfo::generation() const {
    std::string_view v = appversion;
    auto dot = v.find('.');
    return number<int>(v.substr(0, dot)).value_or(0);
}

bool ServerInfo::nvidia_software() const { return state.find("MJOLNIR") != std::string::npos; }

ServerInfo parse_serverinfo(std::string_view xml) {
    Reply r = Reply::parse(xml);
    ServerInfo s;
    s.hostname = r.get("hostname");
    s.uniqueid = std::string(trim(r.get("uniqueid")));
    s.appversion = std::string(trim(r.get("appversion")));
    s.gfeversion = std::string(trim(r.get("GfeVersion")));
    s.state = std::string(trim(r.get("state")));
    s.https_port = number<std::uint16_t>(r.get("HttpsPort")).value_or(0);
    s.external_port = number<std::uint16_t>(r.get("ExternalPort")).value_or(0);
    // Hosts that don't say can at least stream H.264.
    s.codec_support = number<int>(r.get("ServerCodecModeSupport")).value_or(0x0001);
    s.paired = trim(r.get("PairStatus")) == "1";
    // GFE keeps currentgame set to the last game after it ends; only a busy
    // host is really running it.
    if (s.state.ends_with("_SERVER_BUSY")) s.current_game = number<int>(r.get("currentgame")).value_or(0);
    if (s.uniqueid.empty()) throw ReplyError("The host's serverinfo has no uniqueid.");
    if (s.appversion.empty()) throw ReplyError("The host's serverinfo has no appversion.");
    return s;
}

std::vector<App> parse_applist(std::string_view xml) {
    Reply r = Reply::parse(xml);
    std::vector<App> apps;
    for (const auto& elements : r.groups("App")) {
        App app;
        bool has_id = false, has_title = false;
        for (const auto& e : elements) {
            if (e.name == "AppTitle") {
                app.title = e.text;
                has_title = true;
            } else if (e.name == "ID") {
                if (auto id = number<int>(e.text)) {
                    app.id = *id;
                    has_id = true;
                }
            } else if (e.name == "IsHdrSupported") {
                app.hdr = trim(e.text) == "1";
            }
        }
        if (!has_id || !has_title) throw ReplyError("The host's app list has an app without a name or an ID.");
        apps.push_back(std::move(app));
    }
    return apps;
}

const App* find_app(const std::vector<App>& apps, std::string_view name) {
    name = trim(name);
    for (const App& app : apps)
        if (iequals(trim(app.title), name)) return &app;
    return nullptr;
}

std::string parse_launch(std::string_view xml) {
    // Like Moonlight, only the status counts: hosts say no with a status
    // code and a message.
    Reply r = Reply::parse(xml);
    return std::string(trim(r.get("sessionUrl0")));
}

}  // namespace moonlight
