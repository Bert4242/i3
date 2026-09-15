package i3test::SeatXTEST;
# vim:ts=4:sw=4:expandtab

use strict;
use warnings;
use v5.10;

use Test::More;

use Exporter 'import';
our @EXPORT = qw(
    seat_xtest_create_master
    seat_xtest_remove_master
    seat_xtest_button_press
    seat_xtest_button_release
    seat_xtest_click
);

=encoding utf-8

=head1 NAME

i3test::SeatXTEST - per-device (per-seat) XTEST fake input

=head1 DESCRIPTION

C<i3test::XTEST> (see there) only fakes input for the I<core> pointer and
keyboard: C<xcb_test_fake_input()> with C<deviceid = XCB_NONE> always targets
whichever master is currently the core pointer/keyboard, no matter which
master device id you would like to drive. Multiseat tests need to fake input
I<from> a specific, non-core master pointer, so that i3 sees the click as
coming from a particular seat (C<seat_for_device()> in i3's C<src/seat.c>
resolves the acting seat purely from the XI2 C<deviceid> the event arrived
on).

This module wraps libXtst's device-specific XTEST calls
(C<XTestFakeDeviceMotionEvent>/C<XTestFakeDeviceButtonEvent>). Those take an
C<XDevice *> opened via Xlib's legacy C<XOpenDevice()> — but C<XOpenDevice>
predates MPX and cannot open a I<master> device at all (confirmed
experimentally: C<XListInputDevices> doesn't even enumerate masters, only
slaves, and opening a master id fails with C<XI_BadDevice>). Every master
i3 (or C<xinput create-master>) creates gets its own paired, permanently
attached slave named C<"<name> XTEST pointer">/C<"<name> XTEST keyboard">
specifically for this purpose — X attributes events faked through that slave
to whichever master it is attached to. So this module takes the I<master>
device id (the same numeric id i3 reports as C<inputs[].pointer> in a
C<GET_SEATS> reply — the one C<seat_for_device()> in i3's C<src/seat.c>
resolves the acting seat from) and resolves it to that paired XTEST slave
via C<XIQueryDevice()> before opening it.

=cut

use Inline C => Config => LIBS => '-lX11 -lXtst -lXi', CCFLAGS => '';
use Inline C => <<'END_OF_C_CODE';
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/XInput.h>

static Display *seat_dpy = NULL;

static Display *seat_xtest_dpy() {
    if (seat_dpy == NULL) {
        seat_dpy = XOpenDisplay(NULL);
        if (seat_dpy == NULL) {
            fprintf(stderr, "seat_xtest: could not open display\n");
        }
    }
    return seat_dpy;
}

/* Finds the slave pointer permanently attached to and paired with the given
 * master device (named "<name> XTEST pointer", auto-created by the server
 * alongside every master) - see the module docs above for why this
 * indirection is necessary. Returns its device id, or -1 if not found. */
static int seat_xtest_resolve_pointer_slave(Display *dpy, int master_id) {
    int ndevices;
    XIDeviceInfo *info = XIQueryDevice(dpy, XIAllDevices, &ndevices);
    if (info == NULL) {
        return -1;
    }
    int result = -1;
    static const char suffix[] = " XTEST pointer";
    for (int i = 0; i < ndevices; i++) {
        XIDeviceInfo *d = &info[i];
        if (d->attachment != master_id || d->use != XISlavePointer) {
            continue;
        }
        size_t nlen = strlen(d->name), slen = strlen(suffix);
        if (nlen >= slen && strcmp(d->name + nlen - slen, suffix) == 0) {
            result = d->deviceid;
            break;
        }
    }
    XIFreeDeviceInfo(info);
    return result;
}

/* Fakes an absolute-position motion event for the given XI I<master>
 * pointer device id, then optionally a button press/release at that
 * position. is_press: 1 = press, 0 = release. Returns false on any X
 * error or if the device has no XTEST slave to fake input through. */
static bool seat_xtest_input(int deviceid, int button, int is_press, int x, int y) {
    Display *dpy = seat_xtest_dpy();
    if (dpy == NULL) {
        return false;
    }

    int slave_id = seat_xtest_resolve_pointer_slave(dpy, deviceid);
    if (slave_id < 0) {
        fprintf(stderr, "seat_xtest: no XTEST pointer slave attached to master %d\n", deviceid);
        return false;
    }

    XDevice *dev = XOpenDevice(dpy, (XID)slave_id);
    if (dev == NULL) {
        fprintf(stderr, "seat_xtest: XOpenDevice(%d) failed\n", slave_id);
        return false;
    }

    int axes[2] = {x, y};
    if (!XTestFakeDeviceMotionEvent(dpy, dev, False, 0, axes, 2, 0)) {
        fprintf(stderr, "seat_xtest: XTestFakeDeviceMotionEvent failed\n");
        XCloseDevice(dpy, dev);
        return false;
    }
    XFlush(dpy);

    bool ok = true;
    if (is_press >= 0) {
        if (!XTestFakeDeviceButtonEvent(dpy, dev, (unsigned int)button, (Bool)is_press, NULL, 0, 0)) {
            fprintf(stderr, "seat_xtest: XTestFakeDeviceButtonEvent failed\n");
            ok = false;
        }
        XFlush(dpy);
    }

    XCloseDevice(dpy, dev);
    return ok;
}

bool seat_xtest_button_press(int deviceid, int button, int x, int y) {
    return seat_xtest_input(deviceid, button, 1, x, y);
}

bool seat_xtest_button_release(int deviceid, int button, int x, int y) {
    return seat_xtest_input(deviceid, button, 0, x, y);
}
END_OF_C_CODE

=head1 EXPORT

=head2 seat_xtest_create_master($name)

Creates a new XI2 master pointer/keyboard pair via C<xinput create-master>,
named C<"$name pointer"> / C<"$name keyboard">. i3 picks such pairs up
automatically (see the multiseat section of the user guide); use
C<seat input $name> in the test config, or C<i3-msg "seat $name input
$name"> at runtime, to assign the pair to a seat.

Returns true on success.

=cut

sub seat_xtest_create_master {
    my ($name) = @_;
    return system('xinput', 'create-master', $name) == 0;
}

=head2 seat_xtest_remove_master($name)

Removes the master pointer/keyboard pair created by
C<seat_xtest_create_master($name)>. Its slaves (and the seat's focus) fall
back to the default seat, same as when a seat is removed at runtime.

=cut

sub seat_xtest_remove_master {
    my ($name) = @_;
    return system('xinput', 'remove-master', "$name pointer") == 0;
}

=head2 seat_xtest_button_press($deviceid, $button, $x, $y)

Fakes an absolute motion to (C<$x>, C<$y>) followed by a ButtonPress, both
originating from the XI2 master pointer with the given numeric C<$deviceid>
(as reported in a C<GET_SEATS> reply's C<inputs[].pointer>) rather than the
core pointer.

Returns false on any X error, true otherwise.

=head2 seat_xtest_button_release($deviceid, $button, $x, $y)

Same as C<seat_xtest_button_press>, but fakes a ButtonRelease.

=cut

=head2 seat_xtest_click($deviceid, $button, $x, $y)

Convenience wrapper: a full click (press then release) at (C<$x>, C<$y>)
from the given device id.

=cut

sub seat_xtest_click {
    my ($deviceid, $button, $x, $y) = @_;
    my $ok = seat_xtest_button_press($deviceid, $button, $x, $y);
    $ok = seat_xtest_button_release($deviceid, $button, $x, $y) && $ok;
    return $ok;
}

=head1 AUTHOR

Written for the multiseat click-confinement feature (see C<seat_may_click_output>
in C<src/seat.c>).

=cut

1
