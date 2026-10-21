// The XML replies of the GameStream control API (Sunshine's nvhttp):
// <root status_code="200" status_message="…">…</root>.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonlight {

/// A reply the host sent but that says no: a status code other than 200,
/// with the host's message.
class HostError : public std::runtime_error {
public:
    HostError(int code, std::string message)
        : std::runtime_error(message.empty() ? "the host answered with status " + std::to_string(code) : message),
          code_(code), message_(std::move(message)) {}
    int code() const { return code_; }
    /// The host's own status message, possibly empty.
    const std::string& host_message() const { return message_; }

private:
    int code_;
    std::string message_;
};

/// A reply that isn't the XML the protocol describes.
class ReplyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A parsed reply: the root element's status and its children's text.
class Reply {
public:
    /// Parses a reply. Throws ReplyError when it isn't XML with a root
    /// element, HostError when its status code isn't 200.
    static Reply parse(std::string_view xml);

    /// The text of the first element with this name anywhere in the reply,
    /// or nothing.
    std::optional<std::string> text(std::string_view name) const;
    /// Like text(), empty when missing.
    std::string get(std::string_view name) const;

    struct Element {
        std::string name;
        std::string text;
    };
    /// The root's children, each with its own children: used for the app
    /// list (<App><AppTitle>…</AppTitle><ID>…</ID></App>).
    std::vector<std::vector<Element>> groups(std::string_view name) const;

private:
    std::vector<Element> all_;  // every element with text, in document order
    std::vector<std::pair<std::string, std::vector<Element>>> children_;
};

/// The status of a reply without throwing: code (0 when unreadable) and
/// message.
struct Status {
    int code = 0;
    std::string message;
};
Status reply_status(std::string_view xml);

struct ServerInfo {
    std::string hostname;
    std::string uniqueid;
    std::string appversion;
    std::string gfeversion;
    std::string state;
    std::uint16_t https_port = 0;
    std::uint16_t external_port = 0;
    int codec_support = 0;
    bool paired = false;
    /// The running app's id; 0 when the host isn't streaming.
    int current_game = 0;

    /// The first number of appversion: 7 and later pair with SHA-256.
    int generation() const;
    /// NVIDIA's own host software (GeForce Experience) rather than Sunshine.
    bool nvidia_software() const;
};

ServerInfo parse_serverinfo(std::string_view xml);

struct App {
    int id = 0;
    std::string title;
    bool hdr = false;
};

std::vector<App> parse_applist(std::string_view xml);

/// The app whose title matches `name`, ignoring case and surrounding
/// spaces.
const App* find_app(const std::vector<App>& apps, std::string_view name);

/// The RTSP session URL from a /launch or /resume reply (empty for hosts
/// that don't send one).
std::string parse_launch(std::string_view xml);

}  // namespace moonlight
