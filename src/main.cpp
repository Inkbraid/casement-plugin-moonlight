// casement-moonlight: Casement's plugin for Sunshine (GameStream) hosts.

#include <memory>
#include <string>

#include "casement/plugin.h"
#include "moonlight/options.h"
#include "moonlight/session.h"

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0"
#endif

namespace {

std::unique_ptr<casement::Session> start(casement::Host& host, const casement::Connect& c) {
    std::string error;
    auto options = moonlight::parse_options(c.address, error);
    if (!options) host.finish(casement::State::failed, error);
    return moonlight::start_session(host, *options);
}

}  // namespace

int main() {
    casement::Info info{"MOONLIGHT", "Moonlight", PLUGIN_VERSION, {"cursor", "audio"}, 1, 1};
    return casement::run(info, start);
}
