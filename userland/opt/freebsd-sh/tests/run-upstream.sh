#!/bin/freebsd-sh
# Run the imported FreeBSD sh tests without ATF.

shell=${ARMOS_TEST_SHELL:-/bin/freebsd-sh}
root=${ARMOS_SH_TEST_ROOT:-/opt/freebsd-sh/tests/upstream}
manifest=$root/armos-manifest
category=${1:-all}
selected=${2:-}
verbose=${ARMOS_SH_TEST_VERBOSE:-0}
work=/tmp/freebsd-sh-tests.$$
passed=0
failed=0
skipped=0

if test ! -x "$shell" || test ! -r "$manifest"; then
    printf 'FreeBSD sh tests: missing shell or manifest\n' >&2
	exit 2
fi

case $work in
/tmp/freebsd-sh-tests.[0-9]*) rm -rf "$work" ;;
*) printf 'FreeBSD sh tests: unsafe work directory\n' >&2; exit 2 ;;
esac
mkdir "$work" || exit 2
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Installed test sources live below /opt and are intentionally not writable by
# an unprivileged user.  Several upstream cases create relative files next to
# their script, so run the suite from one private copy instead of weakening the
# installed tree permissions.
cp -R "$root" "$work/upstream" || exit 2
root=$work/upstream
manifest=$root/armos-manifest

while IFS= read -r relative; do
	case $relative in
	./*) relative=${relative#./} ;;
	esac
	case $category in
	all) ;;
	*) case $relative in "$category"/*) ;; *) continue ;; esac ;;
	esac
	if test -n "$selected" && test "$relative" != "$selected"; then
		continue
	fi

	test_file=$root/$relative
	test_dir=${test_file%/*}
	test_name=${test_file##*/}
	expected=${test_name##*.}
	stdout=$work/stdout
	stderr=$work/stderr
	rm -f "$stdout" "$stderr"
	if test "$verbose" = 1; then
		printf '[RUN] %s\n' "$relative"
	fi

	(
		cd "$test_dir" || exit 125
		export SH="$shell"
		export TEST_SH="$shell"
		export TMPDIR=/tmp
		PATH=/bin:/usr/bin:/opt/freebsd-sh/bin
		export PATH
		"$shell" "./$test_name"
	) >"$stdout" 2>"$stderr"
	status=$?
	reason=

	if test "$status" -ne "$expected"; then
		reason="exit $status, expected $expected"
	elif test -f "$test_file.stdout" &&
	     ! cmp "$test_file.stdout" "$stdout" >/dev/null 2>&1; then
		reason='stdout mismatch'
	elif test -f "$test_file.stderr" &&
	     ! cmp "$test_file.stderr" "$stderr" >/dev/null 2>&1; then
		reason='stderr mismatch'
	fi

	if test -z "$reason"; then
		passed=$((passed + 1))
	else
		failed=$((failed + 1))
		printf '[KO] %s: %s\n' "$relative" "$reason"
		if test "$verbose" = 1; then
			if test -s "$stdout"; then
				printf '%s\n' '--- stdout ---'
				cat "$stdout"
			fi
			if test -s "$stderr"; then
				printf '%s\n' '--- stderr ---'
				cat "$stderr"
			fi
		fi
	fi
done <"$manifest"

total=$((passed + failed + skipped))
printf 'FreeBSD sh upstream: %s total, %s passed, %s failed, %s skipped\n' \
    "$total" "$passed" "$failed" "$skipped"
test "$failed" -eq 0
