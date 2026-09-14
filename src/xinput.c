/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * xinput.c: XInput2 support, the device layer underneath multiseat (see
 * seat.c).
 *
 * Client windows are grabbed and frame/decoration windows have their
 * events selected via XInput2 instead of the core protocol so that
 * button-press events carry a device id (see xcb_grab_buttons() /
 * FRAME_EVENT_MASK in the pre-XInput2 code). handle_button_press() is told
 * which device originated the click, which identifies the seat.
 *
 * Master device ids are not stable across X server restarts and masters may
 * be created or destroyed at runtime (e.g. a touchscreen being (un)docked),
 * so seat device resolution happens once at startup and again on every
 * XIHierarchyChanged event and config reload, rather than being cached
 * permanently.
 *
 */
#include "all.h"

bool xinput_supported = false;
uint8_t xinput_opcode = 0;
xcb_input_device_id_t xinput_last_event_device = XCB_INPUT_DEVICE_ALL_MASTER;

/* OR'd into FRAME_EVENT_MASK and ROOT_EVENT_MASK (see include/xcb.h). Starts
 * out as the pre-XInput2 core pointer bits so that windows created before
 * xinput_init() has run (or on a server where it turns out XInput2 isn't
 * usable) keep receiving pointer events the old way. xinput_init() clears
 * this to 0 once XInput2 delivery is confirmed working, so that later
 * window creations don't select pointer events twice (once per protocol). */
uint32_t xinput_core_fallback_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                     XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW;

/*
 * Converts an XInput2 enter event into the core xcb_enter_notify_event_t
 * shape consumed by handle_enter_notify().
 *
 */
static void xinput_translate_enter_event(const xcb_input_enter_event_t *xi_event, xcb_enter_notify_event_t *event) {
    memset(event, '\0', sizeof(xcb_enter_notify_event_t));
    event->response_type = XCB_ENTER_NOTIFY;
    event->detail = xi_event->detail;
    event->sequence = xi_event->sequence;
    event->time = xi_event->time;
    event->root = xi_event->root;
    event->event = xi_event->event;
    event->child = xi_event->child;
    event->root_x = (int16_t)(xi_event->root_x >> 16);
    event->root_y = (int16_t)(xi_event->root_y >> 16);
    event->event_x = (int16_t)(xi_event->event_x >> 16);
    event->event_y = (int16_t)(xi_event->event_y >> 16);
    event->state = (uint16_t)xi_event->mods.effective;
    /* XInput2 has two more modes (PassiveGrab/PassiveUngrab); like the core
     * grab modes they are simply "not normal" for handle_enter_notify(). */
    event->mode = xi_event->mode;
    event->same_screen_focus = (xi_event->same_screen ? 2 : 0) | (xi_event->focus ? 1 : 0);
}

/*
 * Converts an XInput2 motion event into the core xcb_motion_notify_event_t
 * shape consumed by handle_motion_notify().
 *
 */
static void xinput_translate_motion_notify(const xcb_input_motion_event_t *xi_event, xcb_motion_notify_event_t *event) {
    memset(event, '\0', sizeof(xcb_motion_notify_event_t));
    event->response_type = XCB_MOTION_NOTIFY;
    event->detail = XCB_MOTION_NORMAL;
    event->sequence = xi_event->sequence;
    event->time = xi_event->time;
    event->root = xi_event->root;
    event->event = xi_event->event;
    event->child = xi_event->child;
    event->root_x = (int16_t)(xi_event->root_x >> 16);
    event->root_y = (int16_t)(xi_event->root_y >> 16);
    event->event_x = (int16_t)(xi_event->event_x >> 16);
    event->event_y = (int16_t)(xi_event->event_y >> 16);
    event->state = (uint16_t)xi_event->mods.effective;
    event->same_screen = true;
}

/* Master keyboards which currently hold XInput2 keycode grabs, so that
 * xinput_ungrab_all_keys() ungrabs exactly those (a device which has since
 * been removed took its grabs with it and must be skipped). */
static xcb_input_device_id_t *grabbed_keyboards = NULL;
static size_t num_grabbed_keyboards = 0;

/*
 * Converts an XInput2 key press/release event into the shape of the core
 * xcb_key_press_event_t that handle_key_press() and the binding matching
 * in bindings.c consume. The XKB group is folded into bits 13-14 of the
 * state, which is where the core protocol puts it for us (see the
 * XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE setup in main.c).
 *
 */
static void xinput_translate_key_event(const xcb_input_key_press_event_t *xi_event, xcb_key_press_event_t *event) {
    memset(event, '\0', sizeof(xcb_key_press_event_t));
    event->response_type = (xi_event->event_type == XCB_INPUT_KEY_PRESS) ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
    event->detail = (xcb_keycode_t)xi_event->detail;
    event->sequence = xi_event->sequence;
    event->time = xi_event->time;
    event->root = xi_event->root;
    event->event = xi_event->event;
    event->child = xi_event->child;
    event->root_x = (int16_t)(xi_event->root_x >> 16);
    event->root_y = (int16_t)(xi_event->root_y >> 16);
    event->event_x = (int16_t)(xi_event->event_x >> 16);
    event->event_y = (int16_t)(xi_event->event_y >> 16);
    event->state = (uint16_t)((xi_event->mods.effective & 0x1FFF) | ((xi_event->group.effective & 0x3) << 13));
    event->same_screen = true;
}

/*
 * Converts an XInput2 button press/release event (Fp1616 coordinates, a
 * modifier-state struct) into the shape of the core xcb_button_press_event_t
 * that all of i3's existing click/drag/resize code already consumes. This
 * keeps route_click(), tiling_resize(), floating_drag_window(),
 * resize_graphical_handler() etc. completely unaware that XInput2 is
 * involved at all.
 *
 */
static void xinput_translate_button_event(const xcb_input_button_press_event_t *xi_event, xcb_button_press_event_t *event) {
    memset(event, '\0', sizeof(xcb_button_press_event_t));
    event->response_type = (xi_event->event_type == XCB_INPUT_BUTTON_PRESS) ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
    event->detail = (xcb_button_t)xi_event->detail;
    event->sequence = xi_event->sequence;
    event->time = xi_event->time;
    event->root = xi_event->root;
    event->event = xi_event->event;
    event->child = xi_event->child;
    /* Fp1616: integer part is in the upper 16 bits. i3 does not need
     * sub-pixel precision here, so truncating is fine. */
    event->root_x = (int16_t)(xi_event->root_x >> 16);
    event->root_y = (int16_t)(xi_event->root_y >> 16);
    event->event_x = (int16_t)(xi_event->event_x >> 16);
    event->event_y = (int16_t)(xi_event->event_y >> 16);
    event->state = (uint16_t)xi_event->mods.effective;
    event->same_screen = true;
}

void xinput_init(void) {
    const xcb_query_extension_reply_t *extreply = xcb_get_extension_data(conn, &xcb_input_id);
    if (!extreply->present) {
        DLOG("XInput extension is not present on this server, seats will have no effect\n");
        xinput_supported = false;
        seat_apply_config();
        return;
    }

    xcb_input_xi_query_version_cookie_t cookie = xcb_input_xi_query_version(conn, 2, 2);
    xcb_input_xi_query_version_reply_t *version = xcb_input_xi_query_version_reply(conn, cookie, NULL);
    if (version == NULL || version->major_version < 2) {
        DLOG("XInput2 (>= 2.0) is not supported by this server, seats will have no effect\n");
        free(version);
        xinput_supported = false;
        seat_apply_config();
        return;
    }
    DLOG("XInput %d.%d negotiated\n", version->major_version, version->minor_version);
    free(version);

    xinput_opcode = extreply->major_opcode;
    xinput_supported = true;

    /* This runs before tree_init()/manage_existing_windows(), i.e. before
     * any client or frame window exists yet, so it's safe to clear this
     * now: every window created from here on will select/grab pointer
     * events via XInput2 instead (xinput_grab_buttons(),
     * xinput_select_frame_events(), and root below), and
     * FRAME_EVENT_MASK/ROOT_EVENT_MASK must stop asking for core delivery
     * too, or every event would be delivered twice. */
    xinput_core_fallback_mask = 0;

    xinput_select_root_events(conn, true);

    /* The root window's core event mask was already set once (in main(),
     * before this function runs) using the pre-negotiation fallback value
     * of xinput_core_fallback_mask. Re-apply it now that the fallback is
     * cleared, so root doesn't keep double-selecting pointer events at the
     * core protocol level too. */
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){ROOT_EVENT_MASK});

    seat_apply_config();
}

void xinput_select_root_events(xcb_connection_t *conn, bool pointer) {
    if (!xinput_supported) {
        return;
    }

    /* Select, in one request:
     *  - XIHierarchyChanged on all devices, so that a seat's master which
     *    appears or disappears after startup (docking, or an autostart
     *    script creating it after i3 has already come up) is picked up
     *    without requiring an i3 restart.
     *  - button press (not release: root has no use for it, see the
     *    ROOT_EVENT_MASK comment in include/xcb.h), motion and enter on all
     *    (current and future) master pointers, so that pointer events on
     *    the root window (empty desktop) carry a device id too, the same as
     *    client and frame window events. */
    struct {
        xcb_input_event_mask_t header;
        uint32_t mask;
    } root_masks[2] = {
        {.header = {.deviceid = XCB_INPUT_DEVICE_ALL, .mask_len = 1}, .mask = XCB_INPUT_XI_EVENT_MASK_HIERARCHY},
        {.header = {.deviceid = XCB_INPUT_DEVICE_ALL_MASTER, .mask_len = 1},
         .mask = XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS |
                 (pointer ? (XCB_INPUT_XI_EVENT_MASK_MOTION | XCB_INPUT_XI_EVENT_MASK_ENTER) : 0)},
    };
    xcb_input_xi_select_events(conn, root, 2, (xcb_input_event_mask_t *)root_masks);
}

void xinput_select_frame_events(xcb_connection_t *conn, xcb_window_t window, bool enter) {
    if (!xinput_supported) {
        return;
    }

    struct {
        xcb_input_event_mask_t header;
        uint32_t mask;
    } frame_mask = {
        .header = {.deviceid = XCB_INPUT_DEVICE_ALL_MASTER, .mask_len = 1},
        .mask = XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE |
                XCB_INPUT_XI_EVENT_MASK_MOTION | (enter ? XCB_INPUT_XI_EVENT_MASK_ENTER : 0),
    };
    xcb_input_xi_select_events(conn, window, 1, (xcb_input_event_mask_t *)&frame_mask);
}

bool xinput_query_pointer(xcb_connection_t *conn, xcb_input_device_id_t deviceid, int16_t *x, int16_t *y) {
    if (!xinput_supported || deviceid == SEAT_DEVICE_NONE || deviceid == XCB_INPUT_DEVICE_ALL_MASTER) {
        return false;
    }
    xcb_input_xi_query_pointer_reply_t *reply = xcb_input_xi_query_pointer_reply(conn, xcb_input_xi_query_pointer(conn, root, deviceid), NULL);
    if (reply == NULL) {
        return false;
    }
    *x = (int16_t)(reply->root_x >> 16);
    *y = (int16_t)(reply->root_y >> 16);
    free(reply);
    return true;
}

void xinput_warp_pointer(xcb_connection_t *conn, xcb_input_device_id_t deviceid, xcb_window_t window, int16_t x, int16_t y) {
    if (!xinput_supported || deviceid == SEAT_DEVICE_NONE || deviceid == XCB_INPUT_DEVICE_ALL_MASTER) {
        xcb_warp_pointer(conn, XCB_NONE, window, 0, 0, 0, 0, x, y);
        return;
    }
    xcb_input_xi_warp_pointer(conn, XCB_NONE, window, 0, 0, 0, 0, ((int32_t)x) << 16, ((int32_t)y) << 16, deviceid);
}

void xinput_grab_buttons(xcb_connection_t *conn, xcb_window_t window, int *buttons) {
    if (!xinput_supported) {
        return;
    }

    const uint32_t modifiers[] = {XCB_INPUT_MODIFIER_MASK_ANY};
    const uint32_t mask[] = {XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS};

    for (int i = 0; buttons[i] > 0; i++) {
        /* The request has a reply (the modifier combinations that failed to
         * grab) which we never read; discard it so libxcb does not keep it
         * pending forever. */
        xcb_input_xi_passive_grab_device_cookie_t cookie = xcb_input_xi_passive_grab_device(
            conn,
            XCB_CURRENT_TIME,
            window,
            XCB_NONE,   /* cursor */
            buttons[i], /* detail: the button number */
            XCB_INPUT_DEVICE_ALL_MASTER,
            1, /* num_modifiers */
            1, /* mask_len, in 4-byte units */
            XCB_INPUT_GRAB_TYPE_BUTTON,
            XCB_INPUT_GRAB_MODE_22_SYNC,  /* grab_mode: we replay once we've decided what to do */
            XCB_INPUT_GRAB_MODE_22_ASYNC, /* paired_device_mode: don't also freeze the paired keyboard */
            0,                            /* owner_events */
            mask,
            modifiers);
        xcb_discard_reply(conn, cookie.sequence);
    }
}

void xinput_ungrab_buttons(xcb_connection_t *conn, xcb_window_t window) {
    if (!xinput_supported) {
        return;
    }

    const uint32_t modifiers[] = {XCB_INPUT_MODIFIER_MASK_ANY};
    xcb_input_xi_passive_ungrab_device(
        conn,
        window,
        0, /* detail: AnyButton */
        XCB_INPUT_DEVICE_ALL_MASTER,
        1, /* num_modifiers */
        XCB_INPUT_GRAB_TYPE_BUTTON,
        modifiers);
}

void xinput_grab_key(xcb_connection_t *conn, xcb_input_device_id_t deviceid, uint32_t keycode, const uint32_t *modifiers, uint16_t num_modifiers) {
    const uint32_t mask[] = {XCB_INPUT_XI_EVENT_MASK_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE};
    xcb_input_xi_passive_grab_device_cookie_t cookie = xcb_input_xi_passive_grab_device(
        conn,
        XCB_CURRENT_TIME,
        root,
        XCB_NONE, /* cursor */
        keycode,
        deviceid,
        num_modifiers,
        1, /* mask_len, in 4-byte units */
        XCB_INPUT_GRAB_TYPE_KEYCODE,
        XCB_INPUT_GRAB_MODE_22_ASYNC,
        XCB_INPUT_GRAB_MODE_22_ASYNC,
        0, /* owner_events */
        mask,
        modifiers);
    xcb_discard_reply(conn, cookie.sequence);

    for (size_t i = 0; i < num_grabbed_keyboards; i++) {
        if (grabbed_keyboards[i] == deviceid) {
            return;
        }
    }
    grabbed_keyboards = srealloc(grabbed_keyboards, (num_grabbed_keyboards + 1) * sizeof(xcb_input_device_id_t));
    grabbed_keyboards[num_grabbed_keyboards++] = deviceid;
}

void xinput_ungrab_all_keys(xcb_connection_t *conn) {
    const uint32_t modifiers[] = {XCB_INPUT_MODIFIER_MASK_ANY};
    for (size_t i = 0; i < num_grabbed_keyboards; i++) {
        xcb_input_xi_passive_ungrab_device(
            conn,
            root,
            0, /* detail: any keycode */
            grabbed_keyboards[i],
            1, /* num_modifiers */
            XCB_INPUT_GRAB_TYPE_KEYCODE,
            modifiers);
    }
    FREE(grabbed_keyboards);
    num_grabbed_keyboards = 0;
}

/*
 * Drops a master device which the server just removed from the list of
 * grabbed keyboards: its grabs went away with it, and ungrabbing it would
 * only produce a BadDevice error.
 *
 */
static void xinput_forget_removed_devices(const xcb_input_hierarchy_event_t *event) {
    xcb_input_hierarchy_info_iterator_t iter = xcb_input_hierarchy_infos_iterator(event);
    for (; iter.rem > 0; xcb_input_hierarchy_info_next(&iter)) {
        if (!(iter.data->flags & XCB_INPUT_HIERARCHY_MASK_MASTER_REMOVED)) {
            continue;
        }
        for (size_t i = 0; i < num_grabbed_keyboards; i++) {
            if (grabbed_keyboards[i] == iter.data->deviceid) {
                grabbed_keyboards[i] = grabbed_keyboards[--num_grabbed_keyboards];
                break;
            }
        }
    }
}

void xinput_handle_event(xcb_generic_event_t *event) {
    xcb_ge_generic_event_t *generic = (xcb_ge_generic_event_t *)event;

    switch (generic->event_type) {
        case XCB_INPUT_BUTTON_PRESS:
        case XCB_INPUT_BUTTON_RELEASE: {
            xcb_input_button_press_event_t *xi_event = (xcb_input_button_press_event_t *)event;
            xcb_button_press_event_t translated;
            xinput_translate_button_event(xi_event, &translated);
            seat_make_active(seat_for_device(xi_event->deviceid));
            handle_button_press(&translated, xi_event->deviceid);
            break;
        }

        case XCB_INPUT_KEY_PRESS:
        case XCB_INPUT_KEY_RELEASE: {
            xcb_input_key_press_event_t *xi_event = (xcb_input_key_press_event_t *)event;
            xcb_key_press_event_t translated;
            xinput_translate_key_event(xi_event, &translated);
            handle_key_press(&translated, xi_event->deviceid);
            break;
        }

        case XCB_INPUT_ENTER: {
            xcb_input_enter_event_t *xi_event = (xcb_input_enter_event_t *)event;
            xcb_enter_notify_event_t translated;
            xinput_translate_enter_event(xi_event, &translated);
            seat_make_active(seat_for_device(xi_event->deviceid));
            handle_enter_notify(&translated, xi_event->full_sequence);
            break;
        }

        case XCB_INPUT_MOTION: {
            xcb_input_motion_event_t *xi_event = (xcb_input_motion_event_t *)event;
            xcb_motion_notify_event_t translated;
            xinput_translate_motion_notify(xi_event, &translated);
            seat_make_active(seat_for_device(xi_event->deviceid));
            handle_motion_notify(&translated);
            break;
        }

        case XCB_INPUT_HIERARCHY:
            DLOG("XIHierarchyChanged event, re-resolving seat devices and re-grabbing keys\n");
            xinput_forget_removed_devices((xcb_input_hierarchy_event_t *)event);
            ungrab_all_keys(conn);
            seat_resolve_devices();
            grab_all_keys(conn);
            seat_invalidate_focus_ids();
            ipc_send_seat_event("devices", NULL);
            tree_render();
            break;
    }
}
