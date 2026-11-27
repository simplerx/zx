#!/bin/sh
set -eu

SKILL_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT="$(CDPATH= cd -- "$SKILL_DIR/../../.." && pwd)"
NAME="$(basename "$SKILL_DIR")"
fail=0

note_fail() {
    printf 'FAIL %s\n' "$1"
    fail=$((fail + 1))
}

for f in SKILL.md references/project-map.md references/workflows.md references/lessons.md; do
    [ -f "$SKILL_DIR/$f" ] || note_fail "missing $f"
done
[ "$fail" -eq 0 ] || exit 1

name_count=$(awk 'NR == 1 && $0 == "---" { fm=1; next } fm && $0 == "---" { exit } fm && /^name:/ { n++ } END { print n + 0 }' "$SKILL_DIR/SKILL.md")
name_value=$(awk 'NR == 1 && $0 == "---" { fm=1; next } fm && $0 == "---" { exit } fm && /^name:/ { sub(/^name:[[:space:]]*/, ""); print; exit }' "$SKILL_DIR/SKILL.md")
desc_count=$(awk 'NR == 1 && $0 == "---" { fm=1; next } fm && $0 == "---" { exit } fm && /^description:/ { n++ } END { print n + 0 }' "$SKILL_DIR/SKILL.md")
desc_len=$(awk 'NR == 1 && $0 == "---" { fm=1; next } fm && $0 == "---" { exit } fm && /^description:/ { sub(/^description:[[:space:]]*/, ""); print length($0); exit }' "$SKILL_DIR/SKILL.md")
body_lines=$(awk 'NR == 1 && $0 == "---" { fm=1; next } fm && $0 == "---" { fm=0; body=1; next } body { n++ } END { print n + 0 }' "$SKILL_DIR/SKILL.md")

[ "$name_count" -eq 1 ] && [ "$name_value" = "$NAME" ] || note_fail "name must be unique and match folder: $NAME"
case "$name_value" in
    *[!a-z0-9-]*|'') note_fail "name must use lowercase letters, digits, and hyphens" ;;
esac
[ "$desc_count" -eq 1 ] || note_fail "description must appear exactly once"
[ "$desc_len" -gt 0 ] && [ "$desc_len" -le 1024 ] || note_fail "description must be 1..1024 characters"
[ "$body_lines" -gt 0 ] && [ "$body_lines" -lt 500 ] || note_fail "SKILL.md body must be 1..499 lines"

if grep -n 'TODO:' "$SKILL_DIR/SKILL.md" "$SKILL_DIR/references/"*.md; then
    note_fail "unresolved TODO marker"
fi

incomplete_lessons=$(awk '
    /^## / {
        if (heading != "" && !(evidence && rule && guard)) print heading
        heading = $0
        evidence = rule = guard = 0
        next
    }
    heading != "" && /^- 证据：/ { evidence = 1 }
    heading != "" && /^- 规则：/ { rule = 1 }
    heading != "" && /^- 守门：/ { guard = 1 }
    END { if (heading != "" && !(evidence && rule && guard)) print heading }
' "$SKILL_DIR/references/lessons.md")
if [ -n "$incomplete_lessons" ]; then
    note_fail "lessons require evidence, rule, and guard: $incomplete_lessons"
fi

paths=$(
    awk '
        {
            line = $0
            while (match(line, /`[^`]+`/)) {
                token = substr(line, RSTART + 1, RLENGTH - 2)
                line = substr(line, RSTART + RLENGTH)
                if (token ~ /^(src|std|bootstrap|tests|tools|editor|spec|docs|references|\.github|\.workbuddy)\// || token ~ /^(README\.md|build\.sh|test\.sh)/) {
                    gsub(/[.,;:]+$/, "", token)
                    sub(/:[0-9]+([-–][0-9]+)?$/, "", token)
                    if (token !~ /[*?<>|]/ && token !~ / /) print token
                }
            }
        }
    ' "$SKILL_DIR/SKILL.md" "$SKILL_DIR/references/"*.md | sort -u
)

while IFS= read -r p; do
    [ -n "$p" ] || continue
    case "$p" in
        references/*) full="$SKILL_DIR/$p" ;;
        *) full="$ROOT/$p" ;;
    esac
    if [ -e "$full" ]; then
        continue
    fi
    case "$p" in
        spec/*|docs/*|.workbuddy/*)
            top=${p%%/*}
            [ -d "$ROOT/$top" ] || continue
            ;;
    esac
    note_fail "stale path: $p"
done <<EOF
$paths
EOF

if [ "$fail" -ne 0 ]; then
    printf 'skill check: %s failure(s)\n' "$fail"
    exit 1
fi
printf 'skill check: PASS (%s, %s body lines)\n' "$NAME" "$body_lines"
