#!/usr/bin/env perl

# suck - monitor data streams and reap stagnant processes
#
# Copyright 2026 Billy Lyrical
# License: GPL
#

use strict;
use warnings;
use IO::Select;

my $silence_timeout = 0;
my $hard_timeout    = 0;
my $quiet           = 0;
my $ring_size       = 100;
my $verbose         = 0;
my $discard_stderr  = 0;
my $stderr_log      = '';
my $panic_on_stderr = 0;
my $non_interactive = 0;
my $ignore_pattern  = '';

parse_args();

if (@ARGV) {
    exec_mode(@ARGV);
} else {
    filter_mode();
}

# --- modes ---

sub usage {
    my $err = $_[0] ? "Error: $_[0]\n\n" : "";
    die "${err}Usage: suck [options] [command [args...]]

Monitor data streams and reap stagnant processes.

Options:
  -t SEC     Silence timeout. Kill if no output for SEC seconds. [0=off]
  -T SEC     Hard timeout. Kill after SEC seconds regardless.   [0=off]
  -q         Quiet mode. Buffer output, only show on timeout.
  -n N       Ring buffer size for -q mode.                      [100]
  -E         Discard command's STDERR (redirect to /dev/null).
  -e FILE    Redirect command's STDERR to a dedicated log file.
  -P         Fast-fail panic. Kill immediately if any data hits STDERR.
  -I         Non-interactive mode. Redirect STDIN from /dev/null.
  -i REGEX   Ignore pattern. Lines matching REGEX do not reset silence timer.
  -v         Verbose status to stderr.

Exit codes:
  0    Normal completion
  124  Hard timeout
  125  Silence timeout
  126  Fast-fail panic
  127  Exec failed
\n";
}

sub exec_mode {
    my (@cmd) = @_;
    pipe(my $out_rd, my $out_wr) or die "suck: pipe: $!\n";
    pipe(my $err_rd, my $err_wr) or die "suck: pipe: $!\n";

    my $pid = fork();
    die "suck: fork: $!\n" unless defined $pid;

    if ($pid == 0) {
        close $out_rd;
        close $err_rd;

        if ($non_interactive) {
            open(STDIN, '<', '/dev/null') or exit 1;
        }

        open(STDOUT, '>&', $out_wr) or exit 1;
        close $out_wr;

        if ($stderr_log) {
            open(STDERR, '>>', $stderr_log) or exit 1;
            close $err_wr;
        } elsif ($discard_stderr) {
            open(STDERR, '>', '/dev/null') or exit 1;
            close $err_wr;
        } else {
            open(STDERR, '>&', $err_wr) or exit 1;
            close $err_wr;
        }

        exec @cmd;
        warn "suck: exec @cmd: $!\n";
        exit 127;
    }

    close $out_wr;
    close $err_wr;
    local $SIG{INT} = local $SIG{TERM} = sub { kill_child($pid); exit 130 };
    
    monitor($out_rd, $err_rd, $pid);
    
    close $out_rd;
    close $err_rd;
    waitpid($pid, 0);
    exit($? >> 8);
}

sub filter_mode {
    local $SIG{INT} = local $SIG{TERM} = sub { exit 130 };
    monitor(\*STDIN, undef, 0);
}

# --- core ---

sub monitor {
    my ($out_fh, $err_fh, $child_pid) = @_;
    my $sel = IO::Select->new();
    $sel->add($out_fh) if defined $out_fh;
    $sel->add($err_fh) if defined $err_fh;
    
    my $last_output = time();
    my $start       = time();
    my @ring;
    my $partial     = '';
    local $| = 1;

    vprint("suck: pid=%d (silence=%ss hard=%ss)\n",
           $child_pid || $$, $silence_timeout, $hard_timeout) if $verbose;

    while (1) {
        my $wait = 1;
        if ($hard_timeout > 0) {
            my $left = $hard_timeout - (time() - $start);
            if ($left <= 0) {
                vprint("suck: hard timeout after %ss\n", $hard_timeout);
                kill_child($child_pid);
                dump_ring(\@ring, $partial);
                exit 124;
            }
            $wait = $left if $left < $wait;
        }

        my @ready = $sel->can_read($wait);
        my $main_eof = 0;

        foreach my $fh (@ready) {
            if ($fh == $out_fh) {
                my $bytes = sysread($fh, my $buf, 8192);
                if (!defined $bytes || $bytes == 0) {
                    push @ring, $partial if $partial ne '' && $quiet;
                    $sel->remove($fh);
                    $main_eof = 1;
                    next;
                }

                if ($ignore_pattern) {
                    $buf = $partial . $buf;
                    my @lines = split /\n/, $buf, -1;
                    $partial = pop @lines;
                    
                    my $has_valid_activity = 0;
                    foreach my $line (@lines) {
                        if ($line !~ /$ignore_pattern/) {
                            $has_valid_activity = 1;
                        }
                        if ($quiet) {
                            push @ring, $line;
                            shift @ring while @ring > $ring_size;
                        } else {
                            print "$line\n";
                        }
                    }
                    if ($has_valid_activity) {
                        $last_output = time();
                    }
                } else {
                    $last_output = time();
                    if ($quiet) {
                        $buf = $partial . $buf;
                        my @lines = split /\n/, $buf, -1;
                        $partial = pop @lines;
                        push @ring, @lines;
                        shift @ring while @ring > $ring_size;
                    } else {
                        print $buf;
                    }
                }
            }
            elsif (defined $err_fh && $fh == $err_fh) {
                my $bytes = sysread($fh, my $buf, 8192);
                if (!defined $bytes || $bytes == 0) {
                    $sel->remove($fh);
                    next;
                }
                
                if ($panic_on_stderr) {
                    vprint("suck: panic triggered by data on stderr\n") if $verbose;
                    kill_child($child_pid);
                    print STDERR $buf;
                    exit 126;
                }
                
                print STDERR $buf;
                
                if ($ignore_pattern) {
                    if ($buf !~ /$ignore_pattern/) {
                        $last_output = time();
                    }
                } else {
                    $last_output = time();
                }
            }
        }

        last if $main_eof;

        if ($silence_timeout > 0 && time() - $last_output > $silence_timeout) {
            vprint("suck: silence timeout after %ss\n", $silence_timeout);
            kill_child($child_pid);
            dump_ring(\@ring, $partial);
            exit 125;
            last;
        }
    }
}

# --- helpers ---

sub kill_child {
    my ($pid) = @_;
    return unless $pid;
    kill 'TERM', $pid;
    eval {
        local $SIG{ALRM} = sub { kill 'KILL', $pid };
        alarm 5;
        waitpid($pid, 0);
        alarm 0;
    };
}

sub dump_ring {
    my ($ring, $partial) = @_;
    return unless @$ring || $partial ne '';
    my @lines = @$ring;
    push @lines, $partial if $partial ne '';
    print STDERR "--- last " . scalar(@lines) . " lines ---\n";
    print STDERR "$_\n" for @lines;
}

sub vprint {
    printf STDERR @_;
}

sub parse_args {
    while (@ARGV && $ARGV[0] =~ /^-/) {
        my $arg = shift @ARGV;
        if    ($arg eq '-t') { $silence_timeout = shift @ARGV // 0 }
        elsif ($arg eq '-T') { $hard_timeout    = shift @ARGV // 0 }
        elsif ($arg eq '-q') { $quiet           = 1 }
        elsif ($arg eq '-n') { $ring_size       = shift @ARGV // 100 }
        elsif ($arg eq '-E') { $discard_stderr  = 1 }
        elsif ($arg eq '-e') { $stderr_log      = shift @ARGV // '' }
        elsif ($arg eq '-P') { $panic_on_stderr = 1 }
        elsif ($arg eq '-I') { $non_interactive = 1 }
        elsif ($arg eq '-i') { $ignore_pattern  = shift @ARGV // '' }
        elsif ($arg eq '-v') { $verbose         = 1 }
        elsif ($arg eq '-h' || $arg eq '--help') { usage() }
        elsif ($arg eq '--') { last }
        else { usage("unknown option: $arg") }
    }
}

# thankyouverymuchgoodnight
