#!/bin/sh
set -eu
bin=$(realpath "$(dirname "$0")/../omapower")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export XDG_CONFIG_HOME="$tmp"
"$bin" --add make
jq -e '.commands == ["cargo", "make", "rustc"]' "$tmp/omapower/config.json" >/dev/null
"$bin" --remove cargo
jq -e '.commands == ["make", "rustc"]' "$tmp/omapower/config.json" >/dev/null
printf '%s' '{"commands":["rustc"],"other":{"keep":true}}' > "$tmp/omapower/config.json"
"$bin" --add 'a;echo hacked'
jq -e '.commands == ["a;echo hacked", "rustc"] and .other.keep' "$tmp/omapower/config.json" >/dev/null
"$bin" --add gcc & p=$!
"$bin" --add clang & q=$!
wait "$p"; wait "$q"
jq -e '.commands | index("gcc") != null and index("clang") != null' "$tmp/omapower/config.json" >/dev/null
if "$bin" --add /bin/rustc 2>/dev/null; then exit 1; fi
printf 'invalid' > "$tmp/omapower/config.json"
if "$bin" --add cargo 2>/dev/null; then exit 1; fi
[ "$(head -c 20 "$tmp/omapower/config.json")" = invalid ]
printf 'whitelist edit tests passed\n'
