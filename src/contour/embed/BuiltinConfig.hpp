// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace contour::embed
{

/// The configuration a host gets when it supplies none.
///
/// Everything here is a default being switched OFF, and each line is one a host cannot leave to
/// Contour's own defaults: the status line draws inside the item, the key bindings open Contour's
/// windows, and the permissions let a child reach the host's clipboard and fonts. A bell neither
/// sounds nor raises a window alert, and with the bell off DECPS plays nothing. No image is drawn,
/// and no fold gutter is reserved beside the grid: every column of the item is a column of the page.
///
/// The frozen modes are the ones by which a child takes something that is the host's: the page
/// width (DECCOLM, ?3, and ?40 which allows it) and the pointer (?9, ?1000 to ?1003, ?2029). A
/// host that wants its child to have the mouse says so in a file of its own.
inline constexpr std::string_view BuiltinConfig = R"yaml(
default_profile: embed
live_config: false
spawn_new_process: false
on_mouse_select: Nothing
images:
    display: false
folding:
    enabled: false
    show_markers: false
profiles:
    embed:
        show_title_bar: false
        bell:
            sound: "off"
            alert: false
        frozen_dec_modes:
            3: false
            9: false
            40: false
            1000: false
            1001: false
            1002: false
            1003: false
            2029: false
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
