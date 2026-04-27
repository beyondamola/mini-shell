#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make >/dev/null

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

assert_eq() {
	local expected="$1"
	local actual="$2"
	local label="$3"

	while [[ "$expected" == *$'\n' ]]; do
		expected="${expected%$'\n'}"
	done
	while [[ "$actual" == *$'\n' ]]; do
		actual="${actual%$'\n'}"
	done

	if [[ "$expected" != "$actual" ]]; then
		printf 'FAIL: %s\n' "$label" >&2
		printf 'Expected:\n%s\n' "$expected" >&2
		printf 'Actual:\n%s\n' "$actual" >&2
		exit 1
	fi
}

script="$tmpdir/script.txt"

cat >"$script" <<'EOF'
printf '%s\n' "hello world"
exit
EOF
actual="$(./mini-shell <"$script")"
assert_eq $'hello world\n' "$actual" "quoted arguments preserve spaces"

cat >"$script" <<'EOF'
printf '%s\n' a\ b
exit
EOF
actual="$(./mini-shell <"$script")"
assert_eq $'a b\n' "$actual" "escaped spaces are preserved"

cat >"$script" <<'EOF'
echo "$MINI_SHELL_TOKEN"
exit
EOF
actual="$(env MINI_SHELL_TOKEN=expanded-value ./mini-shell <"$script")"
assert_eq $'expanded-value\n' "$actual" "environment variables expand"

cat >"$script" <<'EOF'
false && echo nope || echo yes
true && echo ok; echo next
exit
EOF
actual="$(./mini-shell <"$script")"
assert_eq $'yes\nok\nnext\n' "$actual" "chain operators execute correctly"

cat >"$script" <<'EOF'
printf 'a\nb\n' | grep b
exit
EOF
actual="$(./mini-shell <"$script")"
assert_eq $'b\n' "$actual" "pipelines still work"

outfile="$tmpdir/output.txt"
cat >"$script" <<EOF
echo filetest > "$outfile"
cat < "$outfile"
exit
EOF
actual="$(./mini-shell <"$script")"
assert_eq $'filetest\n' "$actual" "redirection still works"

cat >"$script" <<EOF
cd "$tmpdir"
pwd
exit
EOF
actual="$(./mini-shell <"$script")"
expected_pwd="$(cd "$tmpdir" && pwd -P)"
assert_eq "$expected_pwd"$'\n' "$actual" "cd and pwd builtins work"

cat >"$script" <<'EOF'
false
echo $?
exit
EOF
actual="$(./mini-shell <"$script")"
assert_eq $'1\n' "$actual" "status expansion works"

printf 'All mini-shell tests passed.\n'
