#!/usr/bin/env bash
set -euo pipefail

# Guard exact-mask consumption.  Multi-site B&B keep_production is only safe to
# apply or summarize as an exact optimal-production union when
# keep_production_exact has been checked/reported.  This scans include/src/tools
# (except the chart_trim.hpp producer internals) and allowlists only the
# production-mask apply path after validate_trim_mask_for_apply().

perl - <<'PERL'
use strict;
use warnings;

my @bad;
my $producer_path = 'include/larch/chart_trim.hpp';
my $apply_path = 'src/chart_bnb_trim_apply.cpp';

sub slurp {
  my ($path) = @_;
  open my $fh, '<', $path or die "open $path: $!";
  local $/;
  return <$fh>;
}

sub line_no {
  my ($text, $offset) = @_;
  my $newlines = substr($text, 0, $offset) =~ tr/\n//;
  return 1 + $newlines;
}

sub function_body {
  my ($text, $name) = @_;
  my $start = index($text, $name);
  return unless $start >= 0;
  my $brace = index($text, '{', $start);
  return ($start, '') unless $brace >= 0;
  my $depth = 0;
  for (my $pos = $brace; $pos < length($text); ++$pos) {
    my $ch = substr($text, $pos, 1);
    if ($ch eq '{') { ++$depth; }
    elsif ($ch eq '}') {
      --$depth;
      if ($depth == 0) {
        return ($start, substr($text, $start, $pos - $start + 1));
      }
    }
  }
  return ($start, substr($text, $start));
}

sub known_trim_result_vars {
  my ($text) = @_;
  my %vars;

  # Function parameters / locals, in both common const placements:
  #   multisite_trim_result const& trim
  #   const multisite_trim_result& trim
  while ($text =~ /\bmultisite_trim_result\b\s*(?:const\b\s*)?(?:[&*]\s*)?([A-Za-z_]\w*)/g) {
    $vars{$1} = 1;
  }
  while ($text =~ /\bconst\s+multisite_trim_result\b\s*(?:[&*]\s*)?([A-Za-z_]\w*)/g) {
    $vars{$1} = 1;
  }

  # Common local form used by CLIs.
  while ($text =~ /\bauto\s+([A-Za-z_]\w*)\s*=\s*build_multisite_trim\b/g) {
    $vars{$1} = 1;
  }

  # If a file directly uses trim.keep_production but type inference was too
  # clever for the regexes above, still treat `trim` as a trim result variable.
  $vars{trim} = 1 if $text =~ /\btrim(?:\.|->)keep_production(?!_exact)\b/;
  return sort keys %vars;
}

sub exact_guard_before {
  my ($text, $pos, $var) = @_;
  my $prefix_start = $pos > 1600 ? $pos - 1600 : 0;
  my $prefix = substr($text, $prefix_start, $pos - $prefix_start);
  my $near_start = $pos > 300 ? $pos - 300 : 0;
  my $near = substr($text, $near_start, 700);
  my $q = quotemeta($var);

  return 1 if $near =~ /\b$q(?:\.|->)keep_production_exact\s*\?/s;
  return 1 if $prefix =~ /assert\s*\(\s*$q(?:\.|->)keep_production_exact\s*\)/s;
  return 1 if $prefix =~ /(?:CHECK|REQUIRE)\s*\(\s*$q(?:\.|->)keep_production_exact\s*\)/s;
  return 1 if $prefix =~ /if\s*\(\s*!\s*$q(?:\.|->)keep_production_exact\s*\)\s*\{?\s*(?:throw|[^\n;{}]*\n\s*throw)/s;
  return 1 if $prefix =~ /if\s*\(\s*$q(?:\.|->)keep_production_exact\s*\)/s;
  return 0;
}

sub in_any_span {
  my ($pos, $spans) = @_;
  for my $span (@$spans) {
    return 1 if $pos >= $span->[0] && $pos < $span->[1];
  }
  return 0;
}

# Explicit allowlist for production-mask application: validate_trim_mask_for_apply
# must check exactness, and apply_production_mask_superset may consume the mask
# only after calling that validator.
my %allowed_spans_by_path;
{
  my $apply_text = slurp($apply_path);
  my @allowed_spans;
  for my $name ('validate_trim_mask_for_apply', 'apply_production_mask_superset') {
    my ($start, $body) = function_body($apply_text, $name);
    if (!defined $body) {
      push @bad, "$apply_path: $name() not found";
      next;
    }
    if ($name eq 'validate_trim_mask_for_apply') {
      my $exact_pos = index($body, 'trim.keep_production_exact');
      if ($exact_pos < 0) {
        push @bad, "$apply_path:" . line_no($apply_text, $start) . ": $name() does not check keep_production_exact";
      }
      while ($body =~ /trim\.keep_production(?!_exact)/g) {
        my $pos = $-[0];
        if ($exact_pos < 0 || $pos < $exact_pos) {
          push @bad, "$apply_path:" . line_no($apply_text, $start + $pos) . ": $name() uses trim.keep_production before checking keep_production_exact";
        }
      }
      push @allowed_spans, [$start, $start + length($body)];
      next;
    }

    my $validate_pos = index($body, 'validate_trim_mask_for_apply(grammar, trim)');
    if ($validate_pos < 0) {
      push @bad, "$apply_path:" . line_no($apply_text, $start) . ": production-mask apply does not call validate_trim_mask_for_apply()";
    }
    while ($body =~ /trim\.keep_production(?!_exact)/g) {
      my $pos = $-[0];
      if ($validate_pos < 0 || $pos < $validate_pos) {
        push @bad, "$apply_path:" . line_no($apply_text, $start + $pos) . ": trim.keep_production used before exact-mask validation";
      }
    }
    push @allowed_spans, [$start, $start + length($body)];
  }
  $allowed_spans_by_path{$apply_path} = \@allowed_spans;
}

my @paths = split /\n/, `find include src tools -type f | grep -E '\\.(hpp|cpp)\$' | sort`;
for my $path (@paths) {
  next if $path eq '';
  next if $path eq $producer_path;
  my $text = slurp($path);
  my @vars = known_trim_result_vars($text);
  next unless @vars;
  my $allowed = $allowed_spans_by_path{$path} // [];

  for my $var (@vars) {
    my $q = quotemeta($var);
    while ($text =~ /\b$q(?:\.|->)keep_production(?!_exact)\b/g) {
      my $pos = $-[0];
      next if in_any_span($pos, $allowed);
      next if exact_guard_before($text, $pos, $var);
      push @bad, "$path:" . line_no($text, $pos) . ": $var.keep_production consumed without a preceding keep_production_exact guard/assert";
    }
  }
}

if (@bad) {
  print STDERR "$_\n" for @bad;
  exit 1;
}
PERL
