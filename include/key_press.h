/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * key_press.c: key press handler
 *
 */
#pragma once

#include <config.h>

/**
 * There was a key press. We compare this key code with our bindings table and pass
 * the bound action to parse_command(). deviceid is the XInput2 master keyboard
 * which pressed the key (the binding runs as its seat), or
 * XCB_INPUT_DEVICE_ALL_MASTER for core-protocol events.
 *
 */
void handle_key_press(xcb_key_press_event_t *event, xcb_input_device_id_t deviceid);

/**
 * Kills the commanderror i3-nagbar process, if any.
 *
 * Called when reloading/restarting, since the user probably fixed their wrong
 * keybindings.
 *
 * If wait_for_it is set (restarting), this function will waitpid(), otherwise,
 * ev is assumed to handle it (reloading).
 *
 */
void kill_commanderror_nagbar(bool wait_for_it);
