/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * xinput.h: XInput2 support, the device layer underneath multiseat (see
 * seat.h).
 *
 */
#pragma once

#include <config.h>

#include <xcb/xinput.h>

#include "data.h"

/** Whether the XInput2 extension is present on the server and was
 * successfully initialized (version negotiated). If false, seats have no
 * effect and input handling falls back to i3's original core-protocol
 * behavior. */
extern bool xinput_supported;

/** Major opcode of the XInput extension. Used to recognize XInput2
 * GenericEvents in the main event dispatcher. Only valid when
 * xinput_supported is true. */
extern uint8_t xinput_opcode;

/** The XInput2 device id of the master pointer that most recently
 * initiated a button press, set by handle_button_press(). drag_pointer()
 * (see drag.c) reads this so that an interactive drag/resize grabs and
 * tracks the SAME master pointer that started it: a plain core
 * xcb_grab_pointer() always targets the primary core pointer regardless
 * of which master pointer actually clicked, which silently breaks
 * dragging with a secondary pointer (e.g. a touch seat) once button
 * presses are grabbed via XInput2. i3 is
 * single-threaded and fully handles one button press before the next can
 * arrive, so a plain global is sufficient here (this mirrors the existing
 * last_timestamp global). */
extern xcb_input_device_id_t xinput_last_event_device;

/** OR'd into FRAME_EVENT_MASK and ROOT_EVENT_MASK (see include/xcb.h) so
 * that those macros ask for core-protocol button press/release delivery
 * exactly when XInput2 isn't (yet, or ever) doing it instead: it starts as
 * the core button bits and xinput_init() clears it to 0 once XInput2
 * button delivery is confirmed working. This keeps every existing
 * FRAME_EVENT_MASK/ROOT_EVENT_MASK use site correct without having to
 * touch each one individually. */
extern uint32_t xinput_core_button_fallback_mask;

/**
 * Queries the XInput2 extension, negotiates a version, subscribes to
 * XIHierarchyChanged events on the root window (so that master devices
 * which appear or disappear at runtime are picked up without an i3
 * restart) and applies the seat configuration (see seat_apply_config()).
 * Called once during startup, after the configuration has been loaded.
 *
 */
void xinput_init(void);

/**
 * Handles a GenericEvent (response_type == XCB_GE_GENERIC) that belongs to
 * the XInput extension (i.e., xinput_opcode matched already by the
 * caller). Dispatches button press/release events into i3's existing
 * click handling (see click.c) and hierarchy-changed events into
 * seat_resolve_devices().
 *
 */
void xinput_handle_event(xcb_generic_event_t *event);

/**
 * Replaces xcb_grab_buttons()'s core xcb_grab_button() passive grab with
 * an XInput2 passive grab on all current and future master pointers, so
 * that button-press events carry a device id (see xinput_handle_event()).
 * Falls back to doing nothing (leaving the window ungrabbed) if XInput2
 * is not supported.
 *
 */
void xinput_grab_buttons(xcb_connection_t *conn, xcb_window_t window, int *buttons);

/**
 * Counterpart to xinput_grab_buttons(), used by regrab_all_buttons().
 *
 */
void xinput_ungrab_buttons(xcb_connection_t *conn, xcb_window_t window);

/**
 * Selects XInput2 button press/release events (in addition to i3's normal
 * core protocol events) on the given frame/decoration window, so that
 * clicks on window decorations also carry a device id.
 *
 */
void xinput_select_button_events(xcb_connection_t *conn, xcb_window_t window);
