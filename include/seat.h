/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * seat.h: Multiseat support. A seat is a named group of XInput2 master
 * pointer/keyboard pairs which has its own focused container and its own X11
 * keyboard focus, so that several people (or a keyboard and a touchscreen)
 * can drive different windows at the same time.
 *
 */
#pragma once

#include <config.h>

#include <xcb/xinput.h>

#include "data.h"

/** One `xinput create-master <name>` pair belonging to a seat. The master
 * pointer is named "<name> pointer" and its paired master keyboard
 * "<name> keyboard"; the special name "core" stands for the Virtual core
 * pointer/keyboard pair. */
struct seat_input {
    char *name;
    /** Resolved XInput2 master device ids, or SEAT_DEVICE_NONE while the
     * master does not exist on the server. */
    xcb_input_device_id_t pointer;
    xcb_input_device_id_t keyboard;

    TAILQ_ENTRY(seat_input) inputs;
};

#define SEAT_DEVICE_NONE ((xcb_input_device_id_t)0)

typedef enum {
    /** The seat may focus containers on every output (the default). */
    SEAT_OUTPUTS_ALL = 0,
    /** The seat is inactive: its pointer never focuses anything and its
     * keyboards follow the default seat's focus. */
    SEAT_OUTPUTS_NONE,
    /** The seat may only focus containers on the outputs in `outputs`. */
    SEAT_OUTPUTS_NAMED,
} seat_output_mode_t;

typedef struct Seat {
    char *name;

    TAILQ_HEAD(seat_inputs_head, seat_input) inputs;

    seat_output_mode_t output_mode;
    SLIST_HEAD(seat_outputs_head, output_name) outputs;

    /** The container this seat has focused. Mirrors the global `focused`
     * while this seat is the current seat, see seat_make_current(). */
    Con *focused;
    /** X11 window which currently holds this seat's keyboard focus (or which
     * we last asked the server to focus); XCB_NONE forces a refocus. */
    xcb_window_t focused_id;
    /** Last X11 window we actually focused, kept separately because
     * focused_id gets reset to XCB_NONE to force a refocus. */
    xcb_window_t last_focused;
    /** Where to warp this seat's pointer in the next x_push_changes(). */
    Rect *warp_to;
    /** Current XKB group of this seat's keyboards. */
    int xkb_group;

    TAILQ_ENTRY(Seat) seats;
} Seat;

/** All runtime seats, the implicit default seat always being first. */
extern TAILQ_HEAD(seats_head, Seat) seats;

/** Seats declared in the configuration file (via the seat directive). Turned
 * into runtime seats by seat_apply_config(). */
extern struct seats_head seat_configs;

/** The implicit seat which owns the Virtual core pointer/keyboard pair and
 * every master device not claimed by another seat. */
extern Seat *default_seat;

/** The seat whose input event or command is currently being handled. The
 * global `focused` always belongs to this seat. */
extern Seat *current_seat;

/** The seat which most recently produced device input. Commands which arrive
 * without a device (IPC) run as this seat. */
extern Seat *last_active_seat;

/**
 * Creates the default seat. Must run before the configuration is loaded and
 * before any focus is set.
 *
 */
void seat_init(void);

/**
 * Makes the given seat the current one: the global `focused` and
 * `focused_id` are stored into the previous current seat and loaded from
 * the new one. A seat which never had a focus inherits the previous seat's.
 *
 */
void seat_make_current(Seat *seat);

/**
 * seat_make_current() for a seat which just produced device input; also
 * records it as the last active seat (the one IPC commands run as).
 *
 */
void seat_make_active(Seat *seat);

/**
 * Stores the global `focused`/`focused_id` back into the current seat so
 * that current_seat->focused is up to date (e.g. before iterating over all
 * seats).
 *
 */
void seat_store_current(void);

/**
 * Returns the runtime seat with the given name, or NULL.
 *
 */
Seat *seat_by_name(const char *name);

/**
 * Allocates a seat with the given name and no inputs/outputs (output mode
 * SEAT_OUTPUTS_ALL) and appends it to the given list.
 *
 */
Seat *seat_new(struct seats_head *list, const char *name);

/**
 * Appends an input (an `xinput create-master` name, or "core") to the seat.
 * The input is removed from any other seat in the same list first, since a
 * master pair belongs to exactly one seat.
 *
 */
void seat_add_input(struct seats_head *list, Seat *seat, const char *input);

/**
 * Removes all inputs from the seat.
 *
 */
void seat_clear_inputs(Seat *seat);

/**
 * Sets the output mode of a seat. For SEAT_OUTPUTS_NAMED, call
 * seat_add_output() afterward for every output name.
 *
 */
void seat_set_output_mode(Seat *seat, seat_output_mode_t mode);

/**
 * Appends an output name to a SEAT_OUTPUTS_NAMED seat.
 *
 */
void seat_add_output(Seat *seat, const char *output);

/**
 * Frees a seat and everything it owns. The seat must already be unlinked
 * from its list.
 *
 */
void seat_free(Seat *seat);

/**
 * Turns the seats declared in the configuration into runtime seats:
 * existing seats with the same name get the configured inputs/outputs,
 * new ones are created, and seats which are neither configured anymore nor
 * the default seat are removed. Resolves devices afterward.
 *
 */
void seat_apply_config(void);

/**
 * Removes a runtime seat. Its inputs go back to the default seat. The
 * default seat cannot be removed.
 *
 */
void seat_remove(Seat *seat);

/**
 * (Re-)resolves the master device ids of every seat input from the master
 * devices currently known to the X server. Call after XIHierarchyChanged,
 * after a config reload and after seat inputs were changed at runtime.
 *
 */
void seat_resolve_devices(void);

/**
 * Returns the seat which owns the given XInput2 master device (pointer or
 * keyboard). Unknown devices (including the XCB_INPUT_DEVICE_ALL_MASTER
 * placeholder used for core-protocol fallback events) belong to the default
 * seat.
 *
 */
Seat *seat_for_device(xcb_input_device_id_t deviceid);

/**
 * Like seat_for_device(), for key events: the keyboards of an inactive seat
 * act as the default seat's (their focus follows it, see x_push_changes()).
 *
 */
Seat *seat_for_keyboard(xcb_input_device_id_t deviceid);

/**
 * Returns the master keyboard paired with the given master pointer, or
 * SEAT_DEVICE_NONE if the pointer is not known.
 *
 */
xcb_input_device_id_t seat_keyboard_for_pointer(xcb_input_device_id_t pointer);

/**
 * Returns true if the seat may focus containers on the given output
 * container (CT_OUTPUT).
 *
 */
bool seat_owns_output(Seat *seat, Con *output);

/**
 * Returns true if the seat may focus the given container: always for a free
 * roaming seat, never for an inactive seat, and only on its own outputs for
 * a restricted seat.
 *
 */
bool seat_may_focus(Seat *seat, Con *con);

/**
 * Returns true if the seat is inactive (has no outputs).
 *
 */
bool seat_is_inactive(Seat *seat);

/**
 * Makes sure the seat's focused container is usable: set (inheriting the
 * default seat's focus), on a visible workspace and, for a restricted seat,
 * on one of its outputs; re-points it otherwise. Call seat_store_current()
 * before and reload `focused`/`focused_id` from current_seat afterward.
 *
 */
void seat_repair_focus(Seat *seat);

/**
 * Called before a container is detached and freed: every other seat whose
 * focus is (inside) the container is pointed at what would be focused next.
 *
 */
void seat_con_closing(Con *con);

/**
 * Called by workspace_show() once `next` (on the newly shown workspace) is
 * focused: every other seat whose focus lived on the now hidden workspace
 * follows to `next`, since an unmapped window cannot hold keyboard focus.
 *
 */
void seat_workspace_hidden(Con *old_ws, Con *next);

/**
 * Returns true if any active seat focuses the container or a container
 * inside it (the multiseat version of `con == focused ||
 * con_inside_focused(con)`).
 *
 */
bool seat_focuses_con(Con *con);

/**
 * Returns the seat one of whose master keyboards currently has X11 focus
 * on the given window (queried from the server), or NULL if none does.
 *
 */
Seat *seat_with_keyboard_focus(xcb_window_t window);

/**
 * Returns the seat's first resolved master pointer, or SEAT_DEVICE_NONE.
 *
 */
xcb_input_device_id_t seat_first_pointer(Seat *seat);

/**
 * Queries the position of the seat's pointer (the core pointer when
 * XInput2 isn't in use or the seat has no pointer). Returns false if the
 * position could not be determined.
 *
 */
bool seat_query_pointer(Seat *seat, int16_t *x, int16_t *y);

/**
 * Sets every seat's initial focus at startup: the focused container of the
 * output its pointer is on (restricted seats: one of their outputs), and
 * activates the default seat's.
 *
 */
void seat_init_focus(void);
