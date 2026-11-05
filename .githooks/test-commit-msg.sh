#!/bin/sh
# Exercise the real hook without creating commits or changing Git settings.
set -eu
hook=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/commit-msg
work=$(mktemp -d)
trap 'rm -f "$work/message" "$work/output"; rmdir "$work"' EXIT HUP INT TERM
passed=0

expect() {
    expected=$1
    label=$2
    status=0
    sh "$hook" "$work/message" >"$work/output" 2>&1 || status=$?
    if { [ "$expected" = pass ] && [ "$status" -ne 0 ]; } ||
       { [ "$expected" = fail ] && [ "$status" -eq 0 ]; }; then
        printf 'FAIL: %s (exit %s)\n' "$label" "$status"
        cat "$work/output"
        exit 1
    fi
    passed=$((passed + 1))
}

message() { printf '%b\n' "$1" >"$work/message"; }

message 'Correct terminal device status responses\n\nReturn the standard response so clients can parse it.'
expect pass 'title and explanation'
message 'Correct terminal device status responses'
expect fail 'title alone'
message 'Correct terminal device status responses\n\n# Explanation is still missing'
expect fail 'comment-only body'
message 'Correct terminal device status responses\nReturn the standard response.'
expect fail 'missing blank separator'
message ''
expect fail 'empty message'
message 'correct terminal status responses\n\nReturn the standard response.'
expect fail 'lowercase unscoped title'
message 'fix(term): correct status responses\n\nReturn the standard response.'
expect fail 'Conventional Commits title'
message 'Correct terminal status responses.\n\nReturn the standard response.'
expect fail 'title punctuation'
message 'parvion: contain mouse gestures within the app body\n\nKeep gestures within the component.'
expect pass 'legacy component prefix'
message 'Re-arm terminal input processing\n\nRestore input processing after restart.'
expect pass 'hyphenated action'
message 'Correct terminal status responses\n\n- Return the standard response.'
expect fail 'list-only body'
message 'Correct terminal status responses\n\nSigned-off-by: Test <test@example.com>'
expect fail 'trailer-only body'
message 'Correct terminal status responses\n\nReturn the standard response.\n\n- Cover repeated queries.\n\nSigned-off-by: Test <test@example.com>'
expect pass 'prose followed by list and trailer'
message 'Correct terminal status responses\n\nNote: clients need the standard response.'
expect pass 'prose with a colon'
message 'Correct terminal status responses\n\n```text\nCSI 0 n\n```'
expect fail 'code-only body'
message 'Correct terminal status responses\r\n\r\nReturn the standard response.\r'
expect pass 'CRLF input'
message 'Correct terminal status responses\n\n返回标准状态序列。'
expect pass 'UTF-8 explanation'
message 'Merge branch "topic"'
expect pass 'generated merge'
message 'fixup! Correct terminal status responses'
expect pass 'generated fixup'
message 'squash! Correct terminal status responses'
expect pass 'generated squash'
message 'amend! Correct terminal status responses'
expect fail 'amend needs replacement body'
message 'amend! Correct terminal status responses\n\nReturn the standard response.'
expect pass 'complete amend message'
message 'Revert "Correct terminal status responses"\n\nThis reverts commit 1234567 because the behavior changed.'
expect pass 'explained revert'

title=$(printf '%080d' 0 | tr 0 a)
message "Add ${title#????}\n\nExplain the change."
expect pass '80-byte title'
message "Add ${title#???}\n\nExplain the change."
expect fail '81-byte title'
message "Correct terminal status responses\n\n${title}a"
expect fail '81-byte body line'
message "Merge branch ${title}"
expect fail 'merge line length still applies'
message 'Merge branch "topic"\nExplain the merge.'
expect fail 'merge separator still applies'
message 'Correct terminal status responses\n\n中文中文中文中文中文中文中文中文中文中文中文中文中文中'
expect fail 'UTF-8 byte limit'

message 'Correct terminal status responses\n\n; only a comment'
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.commentChar GIT_CONFIG_VALUE_0=';'
expect fail 'configured comment character'
unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0

printf 'PASS: %s commit message cases\n' "$passed"
