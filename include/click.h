/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * click.c: Button press (mouse click) events.
 *
 */
#pragma once

#include <config.h>

/**
 * The button press X callback. This function determines whether the floating
 * modifier is pressed and where the user clicked (decoration, border, inside
 * the window).
 *
 * Then, route_click is called on the appropriate con.
 *
 * deviceid is the XInput2 master pointer device the click originated from
 * (see xinput.c), or a value for which xinput_pointer_is_ignored() is
 * always false (e.g. XCB_INPUT_DEVICE_ALL_MASTER) when the event came in
 * over the core protocol (currently only true for root window clicks,
 * which never reach the focus-gating logic in route_click()).
 *
 */
void handle_button_press(xcb_button_press_event_t *event, xcb_input_device_id_t deviceid);
