/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * seat.c: Multiseat support (see seat.h).
 *
 * The tree keeps a single focus order (focus_head) which every seat shares:
 * the last seat to act decides which workspace is shown on an output and
 * which tab is visible. What is per seat is the focused container itself and
 * the X11 keyboard focus derived from it. To keep the ~200 existing readers
 * of the global `focused` untouched, `focused` is simply swapped whenever the
 * seat being handled changes, see seat_make_current().
 *
 */
#include "all.h"

struct seats_head seats = TAILQ_HEAD_INITIALIZER(seats);
struct seats_head seat_configs = TAILQ_HEAD_INITIALIZER(seat_configs);

Seat *default_seat = NULL;
Seat *current_seat = NULL;
Seat *last_active_seat = NULL;

#define SEAT_DEFAULT_NAME "default"
#define SEAT_CORE_INPUT "core"
#define SEAT_CORE_POINTER_NAME "Virtual core pointer"

void seat_init(void) {
    default_seat = seat_new(&seats, SEAT_DEFAULT_NAME);
    seat_add_input(&seats, default_seat, SEAT_CORE_INPUT);
    current_seat = last_active_seat = default_seat;
}

void seat_store_current(void) {
    current_seat->focused = focused;
    current_seat->focused_id = focused_id;
}

void seat_make_current(Seat *seat) {
    if (seat == current_seat) {
        return;
    }
    seat_store_current();
    DLOG("current seat: \"%s\" -> \"%s\"\n", current_seat->name, seat->name);
    current_seat = seat;
    if (seat->focused == NULL) {
        seat->focused = focused;
    }
    focused = seat->focused;
    focused_id = seat->focused_id;
}

void seat_make_active(Seat *seat) {
    seat_make_current(seat);
    /* An inactive seat cannot focus anything, so commands must never run as
     * it just because it produced the most recent input. */
    if (!seat_is_inactive(seat)) {
        last_active_seat = seat;
    }
}

static Seat *seat_by_name_in(struct seats_head *list, const char *name) {
    Seat *seat;
    TAILQ_FOREACH (seat, list, seats) {
        if (strcmp(seat->name, name) == 0) {
            return seat;
        }
    }
    return NULL;
}

Seat *seat_by_name(const char *name) {
    return seat_by_name_in(&seats, name);
}

Seat *seat_new(struct seats_head *list, const char *name) {
    Seat *seat = scalloc(1, sizeof(Seat));
    seat->name = sstrdup(name);
    TAILQ_INIT(&(seat->inputs));
    SLIST_INIT(&(seat->outputs));
    seat->focus_enabled = true;
    seat->clicks_confined = true;
    seat->output_mode = SEAT_OUTPUTS_ALL;
    TAILQ_INSERT_TAIL(list, seat, seats);
    return seat;
}

static void seat_input_free(struct seat_input *input) {
    FREE(input->name);
    FREE(input);
}

static void seat_remove_input(Seat *seat, const char *name) {
    struct seat_input *input;
    TAILQ_FOREACH (input, &(seat->inputs), inputs) {
        if (strcmp(input->name, name) == 0) {
            TAILQ_REMOVE(&(seat->inputs), input, inputs);
            seat_input_free(input);
            return;
        }
    }
}

void seat_add_input(struct seats_head *list, Seat *seat, const char *name) {
    Seat *other;
    TAILQ_FOREACH (other, list, seats) {
        seat_remove_input(other, name);
    }

    struct seat_input *input = scalloc(1, sizeof(struct seat_input));
    input->name = sstrdup(name);
    input->pointer = input->keyboard = SEAT_DEVICE_NONE;
    TAILQ_INSERT_TAIL(&(seat->inputs), input, inputs);
}

void seat_clear_inputs(Seat *seat) {
    while (!TAILQ_EMPTY(&(seat->inputs))) {
        struct seat_input *input = TAILQ_FIRST(&(seat->inputs));
        TAILQ_REMOVE(&(seat->inputs), input, inputs);
        seat_input_free(input);
    }
}

static void seat_clear_outputs(Seat *seat) {
    while (!SLIST_EMPTY(&(seat->outputs))) {
        struct output_name *output = SLIST_FIRST(&(seat->outputs));
        SLIST_REMOVE_HEAD(&(seat->outputs), names);
        FREE(output->name);
        FREE(output);
    }
}

void seat_set_output_mode(Seat *seat, seat_output_mode_t mode) {
    seat_clear_outputs(seat);
    seat->output_mode = mode;
}

void seat_add_output(Seat *seat, const char *name) {
    struct output_name *output = scalloc(1, sizeof(struct output_name));
    output->name = sstrdup(name);
    /* Keep declaration order: append at the tail. */
    struct output_name *last = SLIST_FIRST(&(seat->outputs));
    if (last == NULL) {
        SLIST_INSERT_HEAD(&(seat->outputs), output, names);
        return;
    }
    while (SLIST_NEXT(last, names) != NULL) {
        last = SLIST_NEXT(last, names);
    }
    SLIST_INSERT_AFTER(last, output, names);
}

void seat_free(Seat *seat) {
    seat_clear_inputs(seat);
    seat_clear_outputs(seat);
    FREE(seat->name);
    FREE(seat);
}

/*
 * Copies the inputs and outputs of `from` onto `to`, taking each input away
 * from whichever runtime seat currently owns it.
 *
 */
static void seat_copy_config(Seat *to, Seat *from) {
    seat_clear_inputs(to);
    struct seat_input *input;
    TAILQ_FOREACH (input, &(from->inputs), inputs) {
        seat_add_input(&seats, to, input->name);
    }

    to->focus_enabled = from->focus_enabled;
    to->clicks_confined = from->clicks_confined;
    seat_set_output_mode(to, from->output_mode);
    struct output_name *output;
    SLIST_FOREACH (output, &(from->outputs), names) {
        seat_add_output(to, output->name);
    }
}

void seat_apply_config(void) {
    /* The default seat always keeps the core pair unless a configured seat
     * claims it explicitly. */
    Seat *config;
    TAILQ_FOREACH (config, &seat_configs, seats) {
        Seat *seat = seat_by_name(config->name);
        if (seat == NULL) {
            seat = seat_new(&seats, config->name);
            seat->focused = focused;
            DLOG("Created seat \"%s\" from config\n", seat->name);
        }
        seat_copy_config(seat, config);
    }

    Seat *seat, *next;
    for (seat = TAILQ_FIRST(&seats); seat != NULL; seat = next) {
        next = TAILQ_NEXT(seat, seats);
        if (seat == default_seat || seat_by_name_in(&seat_configs, seat->name) != NULL) {
            continue;
        }
        DLOG("Removing seat \"%s\", it is no longer configured\n", seat->name);
        seat_remove(seat);
    }

    if (TAILQ_EMPTY(&(default_seat->inputs))) {
        bool core_claimed = false;
        TAILQ_FOREACH (seat, &seats, seats) {
            struct seat_input *input;
            TAILQ_FOREACH (input, &(seat->inputs), inputs) {
                if (strcmp(input->name, SEAT_CORE_INPUT) == 0) {
                    core_claimed = true;
                }
            }
        }
        if (!core_claimed) {
            seat_add_input(&seats, default_seat, SEAT_CORE_INPUT);
        }
    }

    seat_resolve_devices();
}

void seat_remove(Seat *seat) {
    if (seat == default_seat) {
        ELOG("The default seat cannot be removed\n");
        return;
    }

    while (!TAILQ_EMPTY(&(seat->inputs))) {
        struct seat_input *input = TAILQ_FIRST(&(seat->inputs));
        /* seat_add_input() removes (frees) the input from this seat first. */
        char *name = sstrdup(input->name);
        seat_add_input(&seats, default_seat, name);
        free(name);
    }

    if (current_seat == seat) {
        seat_make_current(default_seat);
    }
    if (last_active_seat == seat) {
        last_active_seat = default_seat;
    }

    TAILQ_REMOVE(&seats, seat, seats);
    seat_free(seat);
}

/*
 * Returns the master pointer name for a seat input name, e.g. "asus" ->
 * "asus pointer". The caller frees the result.
 *
 */
static char *seat_input_pointer_name(const char *input) {
    if (strcmp(input, SEAT_CORE_INPUT) == 0) {
        return sstrdup(SEAT_CORE_POINTER_NAME);
    }
    char *name;
    sasprintf(&name, "%s pointer", input);
    return name;
}

/*
 * XKB state is per keyboard: select state notifications for the seat's
 * master keyboard (main.c only does this for the core keyboard), ask for
 * the same per-client flags (XKB state in grabbed key events, detectable
 * autorepeat) and read its current group.
 *
 */
static void seat_init_keyboard(Seat *seat, xcb_input_device_id_t keyboard) {
    if (!xkb_supported) {
        return;
    }

    xcb_xkb_select_events(conn,
                          keyboard,
                          XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
                          0,
                          XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
                          0xff,
                          0xff,
                          NULL);

    const uint32_t mask = XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE |
                          XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED |
                          XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT;
    xcb_xkb_per_client_flags_cookie_t cookie = xcb_xkb_per_client_flags(conn, keyboard, mask, mask, 0, 0, 0);
    xcb_discard_reply(conn, cookie.sequence);

    xcb_xkb_get_state_reply_t *state = xcb_xkb_get_state_reply(conn, xcb_xkb_get_state(conn, keyboard), NULL);
    if (state != NULL) {
        seat->xkb_group = state->group;
        free(state);
    }
}

void seat_resolve_devices(void) {
    Seat *seat;
    struct seat_input *input;
    TAILQ_FOREACH (seat, &seats, seats) {
        TAILQ_FOREACH (input, &(seat->inputs), inputs) {
            input->pointer = input->keyboard = SEAT_DEVICE_NONE;
        }
    }

    if (!xinput_supported) {
        return;
    }

    xcb_input_xi_query_device_cookie_t cookie = xcb_input_xi_query_device(conn, XCB_INPUT_DEVICE_ALL_MASTER);
    xcb_input_xi_query_device_reply_t *reply = xcb_input_xi_query_device_reply(conn, cookie, NULL);
    if (reply == NULL) {
        ELOG("XIQueryDevice failed, seats will have no devices\n");
        return;
    }

    xcb_input_xi_device_info_iterator_t iter = xcb_input_xi_query_device_infos_iterator(reply);
    for (; iter.rem > 0; xcb_input_xi_device_info_next(&iter)) {
        xcb_input_xi_device_info_t *info = iter.data;
        if (info->type != XCB_INPUT_DEVICE_TYPE_MASTER_POINTER) {
            continue;
        }

        const int name_len = xcb_input_xi_device_info_name_length(info);
        const char *name = xcb_input_xi_device_info_name(info);

        TAILQ_FOREACH (seat, &seats, seats) {
            TAILQ_FOREACH (input, &(seat->inputs), inputs) {
                char *pointer_name = seat_input_pointer_name(input->name);
                const bool matches = ((int)strlen(pointer_name) == name_len &&
                                      strncmp(pointer_name, name, name_len) == 0);
                free(pointer_name);
                if (!matches) {
                    continue;
                }
                /* For a master pointer, attachment is its paired master keyboard. */
                input->pointer = info->deviceid;
                input->keyboard = info->attachment;
                DLOG("seat \"%s\" input \"%s\" resolved to XInput2 pointer %d / keyboard %d\n",
                     seat->name, input->name, input->pointer, input->keyboard);
                seat_init_keyboard(seat, input->keyboard);
            }
        }
    }

    free(reply);
}

Seat *seat_for_device(xcb_input_device_id_t deviceid) {
    if (deviceid == SEAT_DEVICE_NONE || deviceid == XCB_INPUT_DEVICE_ALL_MASTER) {
        return default_seat;
    }

    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        struct seat_input *input;
        TAILQ_FOREACH (input, &(seat->inputs), inputs) {
            if (input->pointer == deviceid || input->keyboard == deviceid) {
                return seat;
            }
        }
    }
    return default_seat;
}

Seat *seat_for_keyboard(xcb_input_device_id_t deviceid) {
    Seat *seat = seat_for_device(deviceid);
    return seat_is_inactive(seat) ? default_seat : seat;
}

xcb_input_device_id_t seat_keyboard_for_pointer(xcb_input_device_id_t pointer) {
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        struct seat_input *input;
        TAILQ_FOREACH (input, &(seat->inputs), inputs) {
            if (input->pointer == pointer) {
                return input->keyboard;
            }
        }
    }
    return SEAT_DEVICE_NONE;
}

bool seat_is_inactive(Seat *seat) {
    return !seat->focus_enabled;
}

bool seat_owns_output(Seat *seat, Con *output_con) {
    if (seat->output_mode == SEAT_OUTPUTS_ALL) {
        return true;
    }

    struct output_name *name;
    SLIST_FOREACH (name, &(seat->outputs), names) {
        Output *output = get_output_by_name(name->name, true);
        if (output != NULL && output->con == output_con) {
            return true;
        }
    }
    return false;
}

bool seat_may_focus(Seat *seat, Con *con) {
    if (!seat->focus_enabled) {
        return false;
    }
    if (seat->output_mode == SEAT_OUTPUTS_ALL) {
        return true;
    }
    return seat_owns_output(seat, con_get_output(con));
}

bool seat_may_click_output(Seat *seat, Con *output_con) {
    if (!seat->clicks_confined) {
        return true;
    }
    return seat_owns_output(seat, output_con);
}

/*
 * Returns the container which currently has the focus on the visible
 * workspace of the given output, or NULL if the output has no workspace.
 *
 */
static Con *seat_visible_focus_on_output(Con *output) {
    Con *ws = con_get_fullscreen_con(output, CF_OUTPUT);
    if (ws == NULL) {
        ws = TAILQ_FIRST(&(output_get_content(output)->focus_head));
    }
    if (ws == NULL) {
        return NULL;
    }
    return con_descend_focused(ws);
}

static Con *seat_first_owned_output(Seat *seat) {
    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        if (con_is_internal(output)) {
            continue;
        }
        if (seat_owns_output(seat, output)) {
            return output;
        }
    }
    return NULL;
}

void seat_repair_focus(Seat *seat) {
    if (seat->focused == NULL) {
        seat->focused = (default_seat->focused != NULL ? default_seat->focused : focused);
        seat->focused_id = XCB_NONE;
    }
    Con *f = seat->focused;
    if (f == NULL) {
        return;
    }

    Con *ws = con_get_workspace(f);
    if (ws == NULL) {
        /* Root or output level, nothing to check. */
        return;
    }
    Con *output = con_get_output(f);

    Con *next = NULL;
    if (seat->output_mode == SEAT_OUTPUTS_NAMED && !seat_owns_output(seat, output)) {
        Con *owned = seat_first_owned_output(seat);
        if (owned != NULL) {
            next = seat_visible_focus_on_output(owned);
        }
    } else if (con_is_internal(output)) {
        /* A window moved to the scratchpad lives on the internal output;
         * follow whoever moved it instead. */
        next = focused;
    }

    if (next != NULL && next != f) {
        DLOG("seat \"%s\": focus %p / %s is not usable anymore, re-pointing to %p / %s\n",
             seat->name, f, f->name, next, next->name);
        seat->focused = next;
        seat->focused_id = XCB_NONE;
    }
}

/* Returns the descendant of `con` at (x, y): the child whose rect contains
 * the point, recursively. Tabbed/stacked children all share the same rect,
 * so the first match (the active tab, per con_focus()'s focus_head order)
 * wins there as everywhere else. Falls back to the regular focus order when
 * the point does not land on any child (e.g. a gap or border). */
static Con *con_descend_at_coords(Con *con, int16_t x, int16_t y) {
    if (TAILQ_EMPTY(&(con->focus_head))) {
        return con;
    }
    Con *child;
    TAILQ_FOREACH (child, &(con->focus_head), focused) {
        if (rect_contains(child->rect, x, y)) {
            return con_descend_at_coords(child, x, y);
        }
    }
    return con_descend_at_coords(TAILQ_FIRST(&(con->focus_head)), x, y);
}

/* Returns the container a seat with its pointer at (x, y) should focus on
 * `workspace`: whatever window (floating or tiling) is under that point, or
 * the workspace's regular focus target if the point is not actually on
 * `workspace` (e.g. the pointer query raced with the switch) or it is empty.
 */
static Con *workspace_focus_at(Con *workspace, int16_t x, int16_t y) {
    if (!rect_contains(workspace->rect, x, y)) {
        return con_descend_focused(workspace);
    }
    return con_descend_at_coords(workspace, x, y);
}

/*
 * Called by workspace_show() once a new workspace is shown on an output:
 * every other seat whose focus lived on the now hidden workspace (i.e. was
 * also watching that output) gets pointed at the window under its own
 * pointer on the new workspace, rather than wherever the requesting seat
 * ended up.
 *
 */
void seat_workspace_shown(Con *old_ws, Con *workspace) {
    if (old_ws == NULL) {
        return;
    }
    seat_store_current();
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        if (seat == current_seat || seat->focused == NULL) {
            continue;
        }
        if (con_get_workspace(seat->focused) != old_ws) {
            continue;
        }
        Con *next = NULL;
        int16_t x, y;
        if (seat_query_pointer(seat, &x, &y)) {
            next = workspace_focus_at(workspace, x, y);
        }
        if (next == NULL) {
            next = con_descend_focused(workspace);
        }
        DLOG("seat \"%s\": workspace %s got hidden, following to %p / %s under its own pointer\n",
             seat->name, old_ws->name, next, next->name);
        seat->focused = next;
        seat->focused_id = XCB_NONE;
    }
}

void seat_con_closing(Con *con) {
    seat_store_current();
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        if (seat == current_seat || seat->focused == NULL) {
            continue;
        }
        if (seat->focused != con && !con_has_parent(seat->focused, con)) {
            continue;
        }
        /* con_next_focused() looks at the focus from the perspective of the
         * global `focused`; make that this seat's for the duration. */
        Con *saved = focused;
        focused = seat->focused;
        Con *next = con_next_focused(con);
        focused = saved;
        DLOG("seat \"%s\": focused con %p is closing, next = %p\n", seat->name, con, next);
        seat->focused = next;
        seat->focused_id = XCB_NONE;
    }
}

bool seat_focuses_con(Con *con) {
    seat_store_current();
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        if (seat_is_inactive(seat) || seat->focused == NULL) {
            continue;
        }
        if (seat->focused == con || con_has_parent(con, seat->focused)) {
            return true;
        }
    }
    return false;
}

void seat_invalidate_focus_ids(void) {
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        seat->focused_id = XCB_NONE;
    }
    focused_id = XCB_NONE;
}

xcb_input_device_id_t seat_first_pointer(Seat *seat) {
    struct seat_input *input;
    TAILQ_FOREACH (input, &(seat->inputs), inputs) {
        if (input->pointer != SEAT_DEVICE_NONE) {
            return input->pointer;
        }
    }
    return SEAT_DEVICE_NONE;
}

bool seat_query_pointer(Seat *seat, int16_t *x, int16_t *y) {
    if (xinput_query_pointer(conn, seat_first_pointer(seat), x, y)) {
        return true;
    }
    xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    if (reply == NULL) {
        return false;
    }
    *x = reply->root_x;
    *y = reply->root_y;
    free(reply);
    return true;
}

void seat_init_focus(void) {
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        if (seat_is_inactive(seat)) {
            /* Follows the default seat, see seat_repair_focus(). */
            continue;
        }

        Con *output = NULL;
        int16_t x, y;
        if (seat_query_pointer(seat, &x, &y)) {
            DLOG("seat \"%s\": pointer at %d, %d\n", seat->name, x, y);
            Output *containing = get_output_containing(x, y);
            if (containing != NULL && seat_owns_output(seat, containing->con)) {
                output = containing->con;
            }
        }
        if (output == NULL && seat->output_mode == SEAT_OUTPUTS_NAMED) {
            output = seat_first_owned_output(seat);
        }
        if (output == NULL) {
            output = get_first_output()->con;
        }
        seat->focused = con_descend_focused(output_get_content(output));
        seat->focused_id = XCB_NONE;
    }

    seat_make_current(default_seat);
    con_activate(default_seat->focused);
}

Seat *seat_with_keyboard_focus(xcb_window_t window) {
    if (!xinput_supported) {
        return current_seat;
    }
    Seat *seat;
    TAILQ_FOREACH (seat, &seats, seats) {
        struct seat_input *input;
        TAILQ_FOREACH (input, &(seat->inputs), inputs) {
            if (input->keyboard == SEAT_DEVICE_NONE) {
                continue;
            }
            xcb_input_xi_get_focus_reply_t *reply = xcb_input_xi_get_focus_reply(conn, xcb_input_xi_get_focus(conn, input->keyboard), NULL);
            const bool matches = (reply != NULL && reply->focus == window);
            free(reply);
            if (matches) {
                return seat;
            }
        }
    }
    return NULL;
}
