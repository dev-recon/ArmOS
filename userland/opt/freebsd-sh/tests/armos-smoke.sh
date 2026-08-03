#!/bin/freebsd-sh
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: userland/opt/freebsd-sh/tests/armos-smoke.sh
# Layer: Userland / FreeBSD sh validation
#
# Responsibilities:
# - Exercise the shell evaluator, redirections, pipelines and builtins.
# - Validate the expanded ArmOS execve argument and environment contracts.
#
# Notes:
# - Run with /bin/freebsd-sh; it does not depend on the host shell.
# - The large-argument cases cross the former one-page exec stack limit.
#

failures=0

check()
{
	if "$@"; then
		printf '[OK] %s\n' "$*"
	else
		printf '[KO] %s\n' "$*"
		failures=$((failures + 1))
	fi
}

value=$(printf 'alpha\nbeta\n' | grep beta)
check test "$value" = beta

value=$(printf '%s' "$(printf command-substitution)")
check test "$value" = command-substitution

temporary=/tmp/freebsd-sh-smoke.$$
printf 'redirection\n' >"$temporary"
read value <"$temporary"
rm -f "$temporary"
check test "$value" = redirection

printf 'sourced_value=source-builtin\n' >"$temporary"
source "$temporary"
rm -f "$temporary"
check test "$sourced_value" = source-builtin

glob_root=/tmp/freebsd-sh-glob.$$
mkdir -p "$glob_root/src" "$glob_root/dev"
printf 'one\n' >"$glob_root/src/001-one.c"
printf 'two\n' >"$glob_root/src/001-two.c"
printf 'other\n' >"$glob_root/src/002-other.c"
(
	cd "$glob_root/src" || exit 1
	cp 001*.c ../dev
)
check test -f "$glob_root/dev/001-one.c"
check test -f "$glob_root/dev/001-two.c"
check test ! -e "$glob_root/dev/002-other.c"
(
	cd "$glob_root/dev" || exit 1
	mv 001*.c ../src
	rm ../src/001*.c
)
check test ! -e "$glob_root/src/001-one.c"
check test ! -e "$glob_root/src/001-two.c"
rm -rf "$glob_root"

shell_function()
{
	printf '%s' "$1-$2"
}
value=$(shell_function function works)
check test "$value" = function-works

trap_value=unset
trap 'trap_value=set' USR1
kill -USR1 $$
check test "$trap_value" = set
trap - USR1

set --
index=0
while test "$index" -lt 48; do
	set -- "$@" "arg-$index"
	index=$((index + 1))
done
value=$(/bin/echo "$@")
check test "$#" -eq 48
check test "${value#*arg-47}" != "$value"

large=0123456789abcdef
while test "${#large}" -lt 8192; do
	large=$large$large
done
/bin/echo "$large" >"$temporary"
value=$?
rm -f "$temporary"
check test "$value" -eq 0

index=0
while test "$index" -lt 48; do
	eval "ARMOS_SH_ENV_$index=$index"
	eval "export ARMOS_SH_ENV_$index"
	index=$((index + 1))
done
/bin/true
check test "$?" -eq 0

case arm-os in
arm-*) value=case ;;
*) value=broken ;;
esac
check test "$value" = case

if test "$failures" -ne 0; then
	printf 'FreeBSD sh smoke: %s failure(s)\n' "$failures"
	exit 1
fi

printf 'FreeBSD sh smoke: all checks passed\n'
exit 0
