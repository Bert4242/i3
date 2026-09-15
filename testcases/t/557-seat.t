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
# Verifies the multiseat surface which does not need extra XInput2 master
# devices: the GET_SEATS reply, the seat config directive and the runtime
# seat commands, and the "seats" arrays on tree nodes and workspaces.
#
# Device-level behaviour (per-seat focus, click confinement) needs actual
# master pointer/keyboard pairs and lives in 558-seat-devices.t.
use i3test i3_config => <<EOT;
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1
fake-outputs 1024x768+0+0,1024x768+1024+0

seat touch focus disabled
EOT

# TODO: use the symbolic name for the command/reply type instead of the
# numerical 13:
use constant TYPE_GET_SEATS => 13;

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

################################################################################
# The implicit default seat always exists and owns the Virtual core pair.
################################################################################

my $seats = get_seats();
is(ref($seats), 'ARRAY', 'GET_SEATS reply is an array');

my $default = seat_named('default');
ok(defined($default), 'the implicit default seat exists');
is($default->{focus_enabled}, JSON::XS::true, 'default seat may focus');
is($default->{outputs}, 'all', 'default seat is not restricted to outputs');
is(ref($default->{inputs}), 'ARRAY', 'default seat has an inputs array');
is($default->{inputs}->[0]->{name}, 'core',
   'default seat owns the Virtual core pair');
ok(defined($default->{focused}), 'default seat focuses a container');
ok(defined($default->{workspace}), 'default seat has a workspace');

################################################################################
# A seat declared in the config file shows up, with its focus switch applied.
################################################################################

my $touch = seat_named('touch');
ok(defined($touch), 'the seat from the config file exists');
is($touch->{focus_enabled}, JSON::XS::false,
   '"seat touch focus disabled" was applied');

################################################################################
# Runtime seat commands.
################################################################################

cmd 'seat guest output fake-1';
my $guest = seat_named('guest');
ok(defined($guest), 'seat command created a new seat');
is_deeply($guest->{outputs}, ['fake-1'], 'seat is restricted to its output');
is($guest->{clicks_confined}, JSON::XS::true,
   'clicks are confined by default once a seat is output-restricted');

cmd 'seat guest clicks unconfined';
is(seat_named('guest')->{clicks_confined}, JSON::XS::false,
   '"clicks unconfined" was applied');

cmd 'seat guest clicks toggle';
is(seat_named('guest')->{clicks_confined}, JSON::XS::true,
   '"clicks toggle" toggled back');

cmd 'seat guest focus disabled';
is(seat_named('guest')->{focus_enabled}, JSON::XS::false,
   '"focus disabled" was applied');

cmd 'seat guest focus toggle';
is(seat_named('guest')->{focus_enabled}, JSON::XS::true,
   '"focus toggle" toggled back');

cmd 'seat guest output all';
is(seat_named('guest')->{outputs}, 'all',
   '"output all" lifts the output restriction');

cmd 'seat guest remove';
ok(!defined(seat_named('guest')), 'seat remove removed the seat');

################################################################################
# The default seat's focus cannot be disabled.
################################################################################

cmd 'seat default focus disabled';
is(seat_named('default')->{focus_enabled}, JSON::XS::true,
   'the default seat keeps its focus enabled');

################################################################################
# `seat <name> <command>` runs the command as that seat rather than being
# parsed as a seat switch. `focus` is the interesting case, since `seat <name>
# focus enabled|disabled|toggle` is a switch but `seat <name> focus left` is
# the focus command.
################################################################################

my $ws = fresh_workspace;
my $left = open_window;
my $right = open_window;
is($x->input_focus, $right->id, 'right window focused');

cmd 'seat default focus left';
is($x->input_focus, $left->id,
   '"seat default focus left" ran the focus command');

cmd 'seat default focus right';
is($x->input_focus, $right->id,
   '"seat default focus right" ran the focus command');

################################################################################
# Errors are reported rather than crashing i3.
################################################################################

my $reply = cmd 'seat nonexistent focus left';
is($reply->[0]->{success}, JSON::XS::false,
   'running a command as an unknown seat fails');

$reply = cmd 'seat nonexistent remove';
is($reply->[0]->{success}, JSON::XS::false,
   'removing an unknown seat fails');

does_i3_live;

################################################################################
# Tree nodes and workspaces name the seats focusing them.
################################################################################

my $tree = $i3->get_tree->recv;
is(ref($tree->{seats}), 'ARRAY', 'tree nodes carry a seats array');

my $focused = $x->input_focus;
sub find_con {
    my ($con) = @_;
    return $con if defined($con->{window}) && $con->{window} == $focused;
    for my $child (@{$con->{nodes}}, @{$con->{floating_nodes}}) {
        my $found = find_con($child);
        return $found if defined($found);
    }
    return undef;
}
my $con = find_con($tree);
ok(defined($con), 'found the focused container in the tree');
is_deeply($con->{seats}, ['default'],
   'the focused container names the default seat');

my @ws = grep { $_->{name} eq $ws } @{$i3->get_workspaces->recv};
is_deeply($ws[0]->{seats}, ['default'],
   'the focused workspace names the default seat');

done_testing;
