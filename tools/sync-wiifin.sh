#!/bin/bash
# Is WiiFin's develop in this branch?  If not: a trial merge of it in a
# separate work tree, as one is done by hand here, and a report of it.
#   - WiiFin's files WiiFin U removed (the Wii's own) are left out, as are
#     WiiFin's changes to the files WiiFin U has its own version of ("ours"
#     in tools/sync-wiifin.paths);
#   - the commits touching screens WiiFin U draws its own way ("hand") are
#     listed apart: to bring over by hand.
# Writes the report (Markdown) to $REPORT and adds key=value lines to $OUT
# (behind, conflicts, tree; the trial tree is kept with KEEP_TREE=1).  Nothing is committed: the merge itself is made
# by hand (signed).  Used by .github/workflows/sync-wiifin.yml; by hand:
#   tools/sync-wiifin.sh            (WiiFin's develop from GitHub)
#   UPSTREAM=<repo or url> BRANCH=<branch> tools/sync-wiifin.sh
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
UPSTREAM=${UPSTREAM:-https://github.com/fabienmillet/WiiFin.git}
BRANCH=${BRANCH:-develop}
WEB=${WEB:-fabienmillet/WiiFin}
TMP=${TMP_DIR:-$(mktemp -d)}
TREE=$TMP/merge
REPORT=${REPORT:-$TMP/report.md}
OUT=${OUT:-$TMP/out.txt}

git fetch -q --no-tags "$UPSTREAM" "$BRANCH"
UP=$(git rev-parse FETCH_HEAD)
HEAD_=$(git rev-parse HEAD)
if git merge-base --is-ancestor "$UP" "$HEAD_"; then
    echo "behind=0" >> "$OUT"
    echo "WiiFin's \`$BRANCH\` ($(git rev-parse --short "$UP")) is in \`$(git rev-parse --abbrev-ref HEAD)\`: nothing to merge." > "$REPORT"
    cat "$REPORT"
    exit 0
fi
BASE=$(git merge-base "$UP" "$HEAD_")

hand=() ours=()
while read -r kind path; do
    case "$kind" in hand) hand+=("$path");; ours) ours+=("$path");; esac
done < <(grep -v '^\s*#' tools/sync-wiifin.paths)

# a path of WiiFin's left out here: one of "ours", or removed by WiiFin U
# (the file, or its whole folder, was there at the merge base)
left_out() {
    local f=$1 p d
    for p in "${ours[@]}"; do [[ $f == "$p" || $f == "$p"* && $p == */ ]] && return 0; done
    git cat-file -e "$HEAD_:$f" 2>/dev/null && return 1
    git cat-file -e "$BASE:$f" 2>/dev/null && return 0
    d=$(dirname "$f")
    while [ "$d" != . ]; do
        if git cat-file -e "$BASE:$d" 2>/dev/null; then
            git cat-file -e "$HEAD_:$d" 2>/dev/null && return 1 || return 0
        fi
        d=$(dirname "$d")
    done
    return 1
}
by_hand() {
    local f=$1 p
    for p in "${hand[@]}"; do [[ $f == "$p"* ]] && return 0; done
    return 1
}

# the commits, sorted
merged=() handl=() wii=()
behind=0
while read -r sha; do
    behind=$((behind + 1))
    line="- $WEB@${sha:0:7} $(git log -1 --format=%s "$sha")"
    files=$(git diff-tree --no-commit-id --name-only -r "$sha")
    h=() kept=0
    while read -r f; do
        [ -z "$f" ] && continue
        by_hand "$f" && h+=("\`$f\`")
        left_out "$f" || kept=1
    done <<< "$files"
    if [ ${#h[@]} -gt 0 ]; then handl+=("$line (${h[*]})")
    elif [ $kept = 0 ]; then wii+=("$line")
    else merged+=("$line"); fi
done < <(git rev-list --reverse --no-merges "$HEAD_..$UP")

# the trial merge
git worktree remove --force "$TREE" 2>/dev/null || true
git worktree add -q --detach "$TREE" "$HEAD_"
(
    cd "$TREE"
    git -c user.name=sync -c user.email=sync@localhost merge -q --no-commit --no-ff "$UP" >/dev/null 2>&1 || true
    # what is left out goes back to this branch's state
    git diff --name-only "$HEAD_" --cached 2>/dev/null; git diff --name-only --diff-filter=U
) | sort -u | while read -r f; do
    [ -z "$f" ] && continue
    if left_out "$f"; then
        if git cat-file -e "$HEAD_:$f" 2>/dev/null; then git -C "$TREE" checkout -q "$HEAD_" -- "$f"
        else git -C "$TREE" rm -q -f --cached -- "$f" >/dev/null 2>&1 || true; rm -f "$TREE/$f"; fi
    fi
done
conflicts=$(git -C "$TREE" diff --name-only --diff-filter=U | sort -u)
nconf=$(printf '%s' "$conflicts" | grep -c . || true)
{
    echo "behind=$behind"
    echo "conflicts=$nconf"
    echo "tree=$TREE"
} >> "$OUT"

{
    echo "WiiFin's \`$BRANCH\` has **$behind commit(s)** not in \`$(git rev-parse --abbrev-ref HEAD)\` yet (WiiFin at $WEB@${UP:0:7})."
    echo
    if [ "$nconf" -gt 0 ]; then
        echo "**Trial merge: conflicts** in:"
        printf '%s\n' "$conflicts" | sed 's/.*/- `&`/'
    else
        echo "**Trial merge: goes in** (the Wii's own files and WiiFin U's own versions left out)."
    fi
    if [ ${#handl[@]} -gt 0 ]; then
        echo; echo "### To bring over by hand"
        echo "Screens WiiFin U draws its own way (\`source/platform/wiiu/ui/\`): these changes merge but do not show on the Wii U until ported."
        printf '%s\n' "${handl[@]}"
    fi
    if [ ${#merged[@]} -gt 0 ]; then
        echo; echo "### Merged as they are"
        printf '%s\n' "${merged[@]}"
    fi
    if [ ${#wii[@]} -gt 0 ]; then
        echo; echo "### Nothing for WiiFin U (the Wii's own files, or WiiFin U's own version of them)"
        printf '%s\n' "${wii[@]}"
    fi
    echo
    echo "To merge (then \`git rm\` / \`git checkout HEAD --\` the files left out, as above):"
    echo '```sh'
    echo "git fetch upstream $BRANCH && git merge upstream/$BRANCH"
    echo '```'
} > "$REPORT"
cat "$REPORT"

# the trial tree stays for a build only when asked
[ -n "${KEEP_TREE:-}" ] || git worktree remove --force "$TREE"
