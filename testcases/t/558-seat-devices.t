#!perl
# vim:ts=4:sw=4:expandtab
#
# Please read the following documents before working on tests:
# • https://build.i3wm.org/docs/testsuite.html
#   (or docs/testsuite)
#
# • https://build.i3wm.org/docs/lib-i3test.html
#   (alternatively: perldoc ./testcases/lib/i3test.pm)
#
# • https://build.i3wm.org/docs/ipc.html
#   (or docs/ipc)
#
# • https://i3wm.org/downloads/modern_perl_a4.pdf
#   (unless you are already familiar with Perl)
#
# Verifies multiseat behaviour which needs a real second XInput2 master
# pointer/keyboard pair: that a seat has its own focus, that a click from its
# pointer does not move another seat's focus, that a seat with focus disabled
# never takes focus, and that every seat resumes where it stood on a workspace
# when that workspace is shown again.
#
# The surface which does not need extra devices is covered by 557-seat.t.
use i3test i3_config => <<EOT;
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1
fake-outputs 1024x768+0+0
EOT

use constant TYPE_GET_SEATS => 13;

my $master = 'i3test-guest';

BEGIN {
    eval { require i3test::SeatXTEST; i3test::SeatXTEST->import; 1 }
        or plan skip_all => 'i3test::SeatXTEST unavailable (needs Inline::C, libXtst, libXi)';
}

plan skip_all => 'xinput(1) not available'
    if system('command -v xinput >/dev/null 2>&1') != 0;

my $i3 = i3(get_socket_path());
$i3->connect->recv;

sub get_seats {
    return $i3->message(TYPE_GET_SEATS, "")->recv;
}

sub seat_named {
    my ($name) = @_;
    my @match = grep { $_->{name} eq $name } @{get_seats()};
    return $match[0];
}

# The container id a seat currently focuses, as reported by GET_SEATS.
sub seat_focused {
    my ($name) = @_;
    my $seat = seat_named($name);
    return defined($seat) ? $seat->{focused} : undef;
}

# Finds the container for the given X11 window id and returns [$con, $rect].
sub con_for_window {
    my ($win) = @_;
    my $search;
    $search = sub {
        my ($con) = @_;
        return $con if defined($con->{window}) && $con->{window} == $win;
        for my $child (@{$con->{nodes}}, @{$con->{floating_nodes}}) {
            my $found = $search->($child);
            return $found if defined($found);
        }
        return undef;
    };
    return $search->($i3->get_tree->recv);
}

sub con_id_for_window {
    my $con = con_for_window(@_);
    return defined($con) ? $con->{id} : undef;
}

# Clicks into the middle of the given window using the given master pointer.
sub click_window {
    my ($deviceid, $win) = @_;
    my $con = con_for_window($win);
    return 0 unless defined($con);
    my $r = $con->{rect};
    my $ok = seat_xtest_click($deviceid, 1,
                              $r->{x} + int($r->{width} / 2),
                              $r->{y} + int($r->{height} / 2));
    sync_with_i3;
    return $ok;
}

################################################################################
# Create a second master pair and give it its own seat.
################################################################################

unless (seat_xtest_create_master($master)) {
    plan skip_all => "could not run `xinput create-master $master`";
}

# i3 picks new master devices up via XIHierarchyChanged, without a restart.
sync_with_i3;
cmd "seat guest input $master";

my $guest = seat_named('guest');
if (!defined($guest) ||
    !defined($guest->{inputs}) ||
    !defined($guest->{inputs}->[0]->{pointer})) {
    seat_xtest_remove_master($master);
    plan skip_all => 'XInput2 master devices are not usable on this server';
}

my $pointer = $guest->{inputs}->[0]->{pointer};
ok($pointer > 0, "guest seat resolved its master pointer (device $pointer)");
ok(defined($guest->{inputs}->[0]->{keyboard}),
   'guest seat resolved its master keyboard');

# Whatever happens below, hand the devices back before leaving.
END {
    seat_xtest_remove_master($master) if defined($master);
}

################################################################################
# Each seat has its own focus: a click from the guest pointer focuses a window
# for the guest seat without moving the default seat's focus.
################################################################################

my $ws1 = fresh_workspace;
my $left = open_window;
my $right = open_window;
sync_with_i3;

my $left_id = con_id_for_window($left->id);
my $right_id = con_id_for_window($right->id);

# The default seat focuses the most recently opened window.
is(seat_focused('default'), $right_id, 'default seat focuses the right window');

SKIP: {
    skip 'per-device XTEST input not working on this server', 10
        unless click_window($pointer, $left->id);

    is(seat_focused('guest'), $left_id,
       'a click from the guest pointer focuses that window for the guest seat');
    is(seat_focused('default'), $right_id,
       'the default seat keeps its own focus');
    isnt(seat_focused('guest'), seat_focused('default'),
         'the two seats focus different containers at the same time');

    # Note: $x->input_focus (core GetInputFocus) is not a useful probe here.
    # i3 gives each seat's keyboards their own XI2 focus and makes a seat's
    # pointer the ClientPointer of the client it focuses, so the core reply
    # depends on which master the *querying* connection is associated with.
    # GET_SEATS is the observable which actually says what each seat focuses.

    is(seat_named('guest')->{workspace}, $ws1,
       'the guest seat reports the workspace its focus is on');

    ############################################################################
    # Both seats are named on the containers they focus.
    ############################################################################

    is_deeply(con_for_window($right->id)->{seats}, ['default'],
              'the right window names only the default seat');
    is_deeply(con_for_window($left->id)->{seats}, ['guest'],
              'the left window names only the guest seat');

    ############################################################################
    # Every seat resumes where it stood on a workspace when it is shown again.
    # The two seats share the output, so a workspace switch drags both along;
    # coming back must put each of them where it was, not both onto the same
    # container.
    ############################################################################

    my $ws2 = fresh_workspace;
    open_window;
    sync_with_i3;

    cmd "workspace $ws1";
    sync_with_i3;

    is(seat_focused('default'), $right_id,
       'the default seat resumed where it stood on the workspace');
    is(seat_focused('guest'), $left_id,
       'the guest seat resumed where it stood on the workspace');

    ############################################################################
    # With focus disabled, the seat never takes focus: it follows the default
    # seat instead (an unfocused seat's keyboards have to point somewhere),
    # and its clicks change nobody's focus.
    ############################################################################

    cmd 'seat guest focus disabled';
    sync_with_i3;
    is(seat_focused('guest'), seat_focused('default'),
       'a seat with focus disabled follows the default seat');

    click_window($pointer, $left->id);
    is(seat_focused('default'), $right_id,
       "a disabled seat's click does not move the default seat's focus");
    is(seat_focused('guest'), $right_id,
       'a seat with focus disabled never takes focus itself');
}

cmd 'seat guest remove';
ok(!defined(seat_named('guest')), 'seat remove handed the devices back');

does_i3_live;

done_testing;
