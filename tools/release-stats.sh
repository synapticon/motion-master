#!/usr/bin/env bash
# Generate STATS.md: how many times each release asset has been downloaded.
#
# The numbers come from the GitHub Releases API. A count is the total number of
# downloads of that file since it was published. The API reports no per-day
# breakdown, so each run is a snapshot of the totals at that moment.
#
# Usage:
#   ./tools/release-stats.sh                     # write ./STATS.md
#   ./tools/release-stats.sh --output <path>     # write elsewhere ("-" for stdout)
#   ./tools/release-stats.sh --repo <owner/name> # another repository
set -euo pipefail

output="STATS.md"
repo=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -o | --output)
      output="${2:?--output needs a path}"
      shift 2
      ;;
    -r | --repo)
      repo="${2:?--repo needs owner/name}"
      shift 2
      ;;
    -h | --help)
      sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown option: $1 (try --help)" >&2
      exit 2
      ;;
  esac
done

for cmd in gh jq; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "$cmd is required but is not installed" >&2
    exit 1
  fi
done

if [[ -z "$repo" ]]; then
  repo="${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"
fi

# --paginate emits one JSON array per page, so slurp the pages and concatenate them.
releases=$(gh api "repos/${repo}/releases?per_page=100" --paginate | jq -s 'add // []')

if [[ "$(jq 'length' <<<"$releases")" -eq 0 ]]; then
  echo "no releases found in ${repo}" >&2
  exit 1
fi

# The jq program writes the whole document. Keep the heredoc quoted: values reach the
# program through --arg, never through shell expansion.
program=$(cat <<'JQ'
def cells($values): "| " + ($values | join(" | ")) + " |";

# The first $left columns are text and align left. The rest hold counts and align right.
def separator($headers; $left):
  "|" + ([range(0; $headers | length)
          | if . < $left then ":---|" else "---:|" end] | join(""));

def table($headers; $left; $rows):
  [cells($headers), separator($headers; $left)] + ($rows | map(cells(.)));

# An asset name repeats the version of the release that carries it, so strip that to leave
# the artifact kind: motion-master-6.0.0-alpha.84-amd64.deb becomes amd64.deb. A name
# without that prefix is kept whole.
def kind($version): ltrimstr("motion-master-" + $version + "-");

def downloads: [.assets[].download_count] | add // 0;

# A version release is tagged v<version>. Every other tag is a rolling release, whose
# assets carry no version and so share no columns with the version releases.
(map(select(.tag_name | startswith("v")))) as $versioned
| (map(select(.tag_name | startswith("v") | not))) as $rolling
| ($versioned | map(
    (.tag_name | ltrimstr("v")) as $version
    | {
        tag: .tag_name,
        publishedAt: (.published_at // .created_at),
        published: ((.published_at // .created_at)[0:10]),
        counts: (.assets
                 | map({key: (.name | kind($version)), value: .download_count})
                 | from_entries),
        total: downloads
      })) as $rows
| ($rows | map(.counts | keys[]) | unique) as $kinds
| ($rows | map(.total) | add // 0) as $versionedTotal
| (map(downloads) | add // 0) as $grandTotal
| (map(.assets | length) | add // 0) as $assetCount
| [
    "# Release Download Statistics",
    "",
    "Generated \($generated) from the GitHub Releases API for",
    "[\($repo)](https://github.com/\($repo)/releases).",
    "",
    "A count is the total number of downloads of one file since that file was published. The",
    "API reports no per-day breakdown, so this file holds the totals at the time it was",
    "generated. It is not a history. A workflow regenerates it once a day.",
    "",
    "**\($grandTotal) downloads** of \($assetCount) files across \(length) releases.",
    "",
    "## Downloads per artifact",
    "",
    "Every version release builds the same set of artifacts. This table adds each artifact up",
    "over all \($versioned | length) version releases.",
    ""
  ]
  + table(["Artifact", "Downloads"]; 1;
          ($kinds
           | map(. as $k | [$k, ($rows | map(.counts[$k] // 0) | add)])
           | sort_by(-.[1])
           | map([.[0], (.[1] | tostring)]))
          + [["**Total**", "**\($versionedTotal)**"]])
  + [
    "",
    "## Downloads per release",
    "",
    "A dash means that release carries no such artifact.",
    ""
  ]
  + table((["Release", "Published"] + $kinds + ["Total"]); 2;
          ($rows
           | sort_by(.publishedAt) | reverse
           | map(. as $r
                 | [$r.tag, $r.published]
                   + ($kinds | map($r.counts[.] as $c
                                   | if $c == null then "—" else ($c | tostring) end))
                   + [($r.total | tostring)])))
  + (if ($rolling | length) == 0 then [] else
      [
        "",
        "## Rolling releases",
        "",
        "These releases hold one asset set that is replaced in place, so they carry no version",
        "and no artifact columns.",
        ""
      ]
      + table(["Release", "Asset", "Downloads"]; 2;
              ($rolling
               | sort_by(.tag_name)
               | map(. as $r | $r.assets | map([$r.tag_name, .name, (.download_count | tostring)]))
               | add // []))
    end)
  + [""]
  | .[]
JQ
)

markdown=$(jq -r \
  --arg repo "$repo" \
  --arg generated "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  "$program" <<<"$releases")

if [[ "$output" == "-" ]]; then
  printf '%s\n' "$markdown"
else
  printf '%s\n' "$markdown" > "$output"
  echo "wrote ${output}"
fi
