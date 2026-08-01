#!/usr/bin/env bash
# Report C++ code statistics: lines per file (code / comment / blank), per-directory
# subtotals, and a grand total. Counts only the project's own C++ sources
# (apps/, libs/, hil/ — .cc/.h); extern/, build/ and vcpkg are excluded.
#
# Usage:
#   ./tools/code-stats.sh            # sort files by total lines (default)
#   ./tools/code-stats.sh --by-code  # sort files by code lines
#   ./tools/code-stats.sh --by-path  # sort files alphabetically
set -euo pipefail

cd "$(dirname "$0")/.."

sort_key="total"
case "${1:-}" in
  --by-code) sort_key="code" ;;
  --by-path) sort_key="path" ;;
  --by-total | "") sort_key="total" ;;
  -h | --help)
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *)
    echo "unknown option: $1 (try --help)" >&2
    exit 2
    ;;
esac

# Collect the C++ source files, NUL-delimited so paths with spaces survive.
mapfile -d '' files < <(
  find apps libs hil -type f \( -name '*.cc' -o -name '*.h' \) -print0 2>/dev/null
)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "no C++ source files found" >&2
  exit 1
fi

# One awk pass classifies every line of every file as code / comment / blank and
# emits a TSV record per file: code<TAB>comment<TAB>blank<TAB>total<TAB>path
stats="$(
  awk '
    function flush() {
      if (prev != "")
        printf "%d\t%d\t%d\t%d\t%s\n", code, comment, blank, code + comment + blank, prev
    }
    # New file: emit the tally for the previous file, then reset counters.
    # (No apostrophe here on purpose — this comment is inside a single-quoted
    #  awk program, where one would terminate the string.)
    FNR == 1 { flush(); code = comment = blank = inblock = 0; prev = FILENAME }
    {
      line = $0
      if (inblock) {
        comment++
        if (index(line, "*/")) inblock = 0
        next
      }
      t = line
      sub(/^[ \t]+/, "", t)            # strip leading whitespace
      if (t == "") { blank++; next }
      if (substr(t, 1, 2) == "//") { comment++; next }
      if (substr(t, 1, 2) == "/*") {
        comment++
        rest = substr(t, 3)
        if (index(rest, "*/") == 0) inblock = 1
        next
      }
      # Otherwise this is a code line; detect a block comment opened mid-line.
      code++
      p = index(line, "/*")
      if (p > 0) {
        rest = substr(line, p + 2)
        if (index(rest, "*/") == 0) inblock = 1
      }
    }
    END { flush() }
  ' "${files[@]}"
)"

# Sort the per-file table.
case "$sort_key" in
  total) sorted="$(printf '%s\n' "$stats" | sort -t$'\t' -k4,4nr)" ;;
  code) sorted="$(printf '%s\n' "$stats" | sort -t$'\t' -k1,1nr)" ;;
  path) sorted="$(printf '%s\n' "$stats" | sort -t$'\t' -k5,5)" ;;
esac

# Pretty-print: per-file table, per-directory subtotals, grand total.
printf '%s\n' "$sorted" | awk -F'\t' -v sortkey="$sort_key" '
  function bar() { printf "%s\n", \
    "--------------------------------------------------------------------------" }
  BEGIN {
    printf "%7s %9s %7s %7s  %s\n", "CODE", "COMMENT", "BLANK", "TOTAL", "FILE"
    bar()
  }
  {
    code = $1; comment = $2; blank = $3; total = $4; path = $5
    printf "%7d %9d %7d %7d  %s\n", code, comment, blank, total, path
    # Group by directory.
    dir = path
    sub(/\/[^\/]*$/, "", dir)
    gcode[dir] += code; gcom[dir] += comment; gblank[dir] += blank; gtot[dir] += total
    gfiles[dir]++
    tcode += code; tcom += comment; tblank += blank; ttot += total; tfiles++
  }
  END {
    bar()
    printf "\nBy directory:\n"
    printf "%7s %9s %7s %7s %7s  %s\n", "CODE", "COMMENT", "BLANK", "TOTAL", "FILES", "DIRECTORY"
    bar()
    n = 0
    for (d in gtot) { dirs[n++] = d }
    # Simple insertion sort by total lines, descending.
    for (i = 1; i < n; i++) {
      key = dirs[i]
      j = i - 1
      while (j >= 0 && gtot[dirs[j]] < gtot[key]) { dirs[j + 1] = dirs[j]; j-- }
      dirs[j + 1] = key
    }
    for (i = 0; i < n; i++) {
      d = dirs[i]
      printf "%7d %9d %7d %7d %7d  %s\n", gcode[d], gcom[d], gblank[d], gtot[d], gfiles[d], d
    }
    bar()
    printf "%7d %9d %7d %7d %7d  %s\n", tcode, tcom, tblank, ttot, tfiles, "TOTAL"
  }
'
