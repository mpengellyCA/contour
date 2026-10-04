// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace contour::embed
{

/// The configuration a host gets when it supplies none.
///
/// Everything here is a default being switched OFF, and each line is one a host cannot leave to
/// Contour's own defaults: the status line draws inside the item, the key bindings open Contour's
/// windows, and the permissions let a child reach the host's clipboard and fonts.
inline constexpr std::string_view BuiltinConfig = R"yaml(
default_profile: embed
live_config: false
spawn_new_process: false
on_mouse_select: Nothing
profiles:
    embed:
        show_title_bar: false
        bell:
            sound: "off"
        history:
            limit: 10000
        scrollbar:
            position: Hidden
        permissions:
            capture_buffer: deny
            change_font: deny
            display_host_writable_statusline: deny
            write_clipboard: deny
        status_line:
            display: none
input_mapping: []
)yaml";

} // namespace contour::embed
