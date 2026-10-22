// HRESULTs in log messages: Windows' text when it has one, and the code.

#pragma once

#include <format>
#include <string>

#include "casement/text.h"

namespace moonlight {

inline std::string hresult_text(long hr) {
    auto code = static_cast<unsigned long>(hr);
    std::string message = casement::system_message(code);
    std::string hex = std::format("0x{:08X}", code);
    return message.starts_with("error ") ? hex : message + " (" + hex + ")";
}

}  // namespace moonlight
