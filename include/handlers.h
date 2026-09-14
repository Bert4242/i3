/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * handlers.c: Small handlers for various events (keypresses, focus changes,
 *             …).
 *
 */
#pragma once

#include <config.h>

/**
 * When the user moves the mouse pointer onto a window, this callback gets
 * called (with core events, or via xinput_handle_event() with translated
 * XInput2 events).
 *
 */
void handle_enter_notify(xcb_enter_notify_event_t *event, uint32_t full_sequence);

/**
 * When the user moves the mouse pointer over a frame or the root window.
 *
 */
void handle_motion_notify(xcb_motion_notify_event_t *event);

#include <xcb/randr.h>

#include "data.h"

extern int randr_base;
extern int xkb_base;
extern int shape_base;

/**
 * Adds the given sequence to the list of events which are ignored.
 * If this ignore should only affect a specific response_type, pass
 * response_type, otherwise, pass -1.
 *
 * Every ignored sequence number gets garbage collected after 5 seconds.
 *
 */
void add_ignore_event(const ignore_event_sequence_t sequence, const int response_type);

/**
 * Checks if the given sequence is ignored and returns true if so.
 *
 */
bool event_is_ignored(const ignore_event_sequence_t sequence, const int response_type);

/**
 * Takes an xcb_generic_event_t and calls the appropriate handler, based on the
 * event type.
 *
 */
void handle_event(int type, xcb_generic_event_t *event);

/**
 * Sets the appropriate atoms for the property handlers after the atoms were
 * received from X11
 *
 */
void property_handlers_init(void);
