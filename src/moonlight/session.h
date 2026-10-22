// One Casement tab streaming from a host: connects, pairs when the host
// doesn't know us, starts or resumes the app and runs the stream through
// moonlight-common-c until either side ends it.

#pragma once

#include <memory>

#include "casement/plugin.h"
#include "moonlight/options.h"

namespace moonlight {

/// Starts the session on its own thread; the connection's progress and end
/// are reported to Casement as states.
std::unique_ptr<casement::Session> start_session(casement::Host& host, const Options& options);

}  // namespace moonlight
