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
        seat_add_input(&seats, default_seat, input->name);
    }

    if (current_seat == seat) {
        current_seat = default_seat;
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
    return seat->output_mode == SEAT_OUTPUTS_NONE;
}

bool seat_owns_output(Seat *seat, Con *output_con) {
    if (seat->output_mode == SEAT_OUTPUTS_ALL) {
        return true;
    }
    if (seat->output_mode == SEAT_OUTPUTS_NONE) {
        return false;
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
    if (seat->output_mode == SEAT_OUTPUTS_ALL) {
        return true;
    }
    if (seat->output_mode == SEAT_OUTPUTS_NONE) {
        return false;
    }
    return seat_owns_output(seat, con_get_output(con));
}
