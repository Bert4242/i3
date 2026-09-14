/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * xinput.c: XInput2 support. Lets a designated ("ignored") master pointer
 * click/drag/scroll windows without ever stealing i3's focus, via the
 * focus_ignore_pointer configuration directive.
 *
 * Client windows are grabbed and frame/decoration windows have their
 * events selected via XInput2 instead of the core protocol so that
 * button-press events carry a device id (see xcb_grab_buttons() /
 * FRAME_EVENT_MASK in the pre-XInput2 code). handle_button_press() is told
 * which device originated the click; route_click() uses that to decide
 * whether to skip focusing.
 *
 * Master pointer ids are not stable across X server restarts and the
 * ignored pointer may be created or destroyed at runtime (e.g. a
 * touchscreen being (un)docked), so device name resolution happens once at
 * startup and again on every XIHierarchyChanged event and config reload,
 * rather than being cached permanently.
 *
 */
#include "all.h"

bool xinput_supported = false;
uint8_t xinput_opcode = 0;
xcb_input_device_id_t xinput_last_event_device = XCB_INPUT_DEVICE_ALL_MASTER;

/* OR'd into FRAME_EVENT_MASK and ROOT_EVENT_MASK (see include/xcb.h). Starts
 * out as the pre-XInput2 core button bits so that windows created before
 * xinput_init() has run (or on a server where it turns out XInput2 isn't
 * usable) keep receiving button press/release the old way. xinput_init()
 * clears this to 0 once XInput2 button delivery is confirmed working, so
 * that later window creations don't select button events twice (once per
 * protocol). */
uint32_t xinput_core_button_fallback_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;

static xcb_input_device_id_t *ignored_devices = NULL;
static size_t num_ignored_devices = 0;

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

/*
 * Frees any previously resolved ignore list and re-resolves
 * focus_ignore_pointer device names against the master pointers currently
 * known to the X server. Safe to call at any time, including when XInput2
 * is not supported (in which case it is a no-op) or when
 * focus_ignore_pointers is empty.
 *
 */
void xinput_reresolve_ignored_pointers(void) {
    FREE(ignored_devices);
    num_ignored_devices = 0;

    if (!xinput_supported || TAILQ_EMPTY(&focus_ignore_pointers)) {
        return;
    }

    xcb_input_xi_query_device_cookie_t cookie = xcb_input_xi_query_device(conn, XCB_INPUT_DEVICE_ALL_MASTER);
    xcb_input_xi_query_device_reply_t *reply = xcb_input_xi_query_device_reply(conn, cookie, NULL);
    if (reply == NULL) {
        ELOG("XIQueryDevice failed, focus_ignore_pointer directives will have no effect\n");
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

        struct focus_ignore_pointer *ignored;
        TAILQ_FOREACH (ignored, &focus_ignore_pointers, focus_ignore_pointers) {
            if ((int)strlen(ignored->name) != name_len || strncmp(ignored->name, name, name_len) != 0) {
                continue;
            }

            DLOG("focus_ignore_pointer \"%s\" resolved to XInput2 device id %d\n", ignored->name, info->deviceid);
            ignored_devices = srealloc(ignored_devices, (num_ignored_devices + 1) * sizeof(xcb_input_device_id_t));
            ignored_devices[num_ignored_devices++] = info->deviceid;
            break;
        }
    }

    free(reply);
}

/*
 * Returns true if the given device id belongs to a currently resolved
 * focus_ignore_pointer master pointer.
 *
 */
bool xinput_pointer_is_ignored(xcb_input_device_id_t deviceid) {
    for (size_t i = 0; i < num_ignored_devices; i++) {
        if (ignored_devices[i] == deviceid) {
            return true;
        }
    }
    return false;
}

void xinput_init(void) {
    const xcb_query_extension_reply_t *extreply = xcb_get_extension_data(conn, &xcb_input_id);
    if (!extreply->present) {
        DLOG("XInput extension is not present on this server, focus_ignore_pointer will have no effect\n");
        xinput_supported = false;
        return;
    }

    xcb_input_xi_query_version_cookie_t cookie = xcb_input_xi_query_version(conn, 2, 2);
    xcb_input_xi_query_version_reply_t *version = xcb_input_xi_query_version_reply(conn, cookie, NULL);
    if (version == NULL || version->major_version < 2) {
        DLOG("XInput2 (>= 2.0) is not supported by this server, focus_ignore_pointer will have no effect\n");
        free(version);
        xinput_supported = false;
        return;
    }
    DLOG("XInput %d.%d negotiated\n", version->major_version, version->minor_version);
    free(version);

    xinput_opcode = extreply->major_opcode;
    xinput_supported = true;

    /* This runs before tree_init()/manage_existing_windows(), i.e. before
     * any client or frame window exists yet, so it's safe to clear this
     * now: every window created from here on will select/grab button
     * events via XInput2 instead (xinput_grab_buttons(),
     * xinput_select_button_events(), and root below), and
     * FRAME_EVENT_MASK/ROOT_EVENT_MASK must stop asking for core button
     * delivery too, or every click would be delivered twice. */
    xinput_core_button_fallback_mask = 0;

    /* Select, in one request:
     *  - XIHierarchyChanged on all devices, so that a focus_ignore_pointer
     *    master which appears or disappears after startup (docking, or an
     *    autostart script creating it after i3 has already come up) is
     *    picked up without requiring an i3 restart.
     *  - button press (not release: root has no use for it, see the
     *    ROOT_EVENT_MASK comment in include/xcb.h) on all (current and
     *    future) master pointers, so that clicks on the root window
     *    (empty desktop) carry a device id too, the same as client and
     *    frame window clicks. */
    struct {
        xcb_input_event_mask_t header;
        uint32_t mask;
    } root_masks[2] = {
        {.header = {.deviceid = XCB_INPUT_DEVICE_ALL, .mask_len = 1}, .mask = XCB_INPUT_XI_EVENT_MASK_HIERARCHY},
        {.header = {.deviceid = XCB_INPUT_DEVICE_ALL_MASTER, .mask_len = 1}, .mask = XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS},
    };
    xcb_input_xi_select_events(conn, root, 2, (xcb_input_event_mask_t *)root_masks);

    /* The root window's core event mask was already set once (in main(),
     * before this function runs) using the pre-negotiation fallback value
     * of xinput_core_button_fallback_mask. Re-apply it now that the
     * fallback is cleared, so root doesn't keep double-selecting button
     * events at the core protocol level too. */
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){ROOT_EVENT_MASK});

    xinput_reresolve_ignored_pointers();
}

void xinput_select_button_events(xcb_connection_t *conn, xcb_window_t window) {
    if (!xinput_supported) {
        return;
    }

    xcb_input_event_mask_t *mask = scalloc(1, sizeof(xcb_input_event_mask_t) + sizeof(uint32_t));
    mask->deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
    mask->mask_len = 1;
    *((uint32_t *)(mask + 1)) = XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE;
    xcb_input_xi_select_events(conn, window, 1, mask);
    free(mask);
}

void xinput_grab_buttons(xcb_connection_t *conn, xcb_window_t window, int *buttons) {
    if (!xinput_supported) {
        return;
    }

    const uint32_t modifiers[] = {XCB_INPUT_MODIFIER_MASK_ANY};
    const uint32_t mask[] = {XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS};

    for (int i = 0; buttons[i] > 0; i++) {
        xcb_input_xi_passive_grab_device(
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

void xinput_handle_event(xcb_generic_event_t *event) {
    xcb_ge_generic_event_t *generic = (xcb_ge_generic_event_t *)event;

    switch (generic->event_type) {
        case XCB_INPUT_BUTTON_PRESS:
        case XCB_INPUT_BUTTON_RELEASE: {
            xcb_input_button_press_event_t *xi_event = (xcb_input_button_press_event_t *)event;
            xcb_button_press_event_t translated;
            xinput_translate_button_event(xi_event, &translated);
            handle_button_press(&translated, xi_event->deviceid);
            break;
        }

        case XCB_INPUT_HIERARCHY:
            DLOG("XIHierarchyChanged event, re-resolving focus_ignore_pointer devices\n");
            xinput_reresolve_ignored_pointers();
            break;
    }
}
