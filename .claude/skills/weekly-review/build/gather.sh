#!/usr/bin/env bash
# Collect the week's Goldfish numbers as one JSON blob.
#
#     ./gather.sh 2026-11-20 > week.json
#
# Everything comes from `gh` and git, which are already authenticated; no
# other credential is used. Window: SINCE up to now; prior: the 7 days before.
# GitHub's A..B search range includes B, so the prior window ends the day
# before SINCE.
set -euo pipefail

SINCE="${1:?usage: gather.sh YYYY-MM-DD}"
REPO="${REPO:-reneaaron/goldfish}"
PRIOR=$(date -u -d "$SINCE - 7 days" +%F)
PRIOR_END=$(date -u -d "$SINCE - 1 day" +%F)
TODAY=$(date -u +%F)

git fetch origin --tags --quiet

merged=$(gh pr list --repo "$REPO" --state merged --limit 200 --search "merged:>=$SINCE" \
  --json number,title,mergedAt,author,additions,deletions,url)
merged_prev=$(gh pr list --repo "$REPO" --state merged --limit 200 --search "merged:$PRIOR..$PRIOR_END" \
  --json number,author,additions,deletions)
open_prs=$(gh pr list --repo "$REPO" --state open --limit 100 \
  --json number,title,isDraft,author,createdAt,updatedAt,url)

# Releases: only a v* tag publishes firmware.uf2, so commits after the last tag
# are on the site's /sim/ (built from main) but on nobody's board.
last_tag=$(git tag --list 'v*' --sort=-creatordate | head -1)
if [ -n "$last_tag" ]; then
  tag_date=$(git log -1 --format=%cI "$last_tag")
  since_tag=$(git rev-list --count "$last_tag..origin/main")
  since_tag_fw=$(git rev-list --count "$last_tag..origin/main" -- ":/firmware/src")
else
  tag_date=""; since_tag=$(git rev-list --count origin/main); since_tag_fw=$(git rev-list --count origin/main -- ":/firmware/src")
fi
releases=$(gh api "repos/$REPO/releases" --jq '[.[] | {tag: .tag_name, published: .published_at,
  downloads: ([.assets[] | {(.name): .download_count}] | add)}]')

issues=$(gh issue list --repo "$REPO" --state all --limit 300 \
  --json number,title,state,createdAt,closedAt,author,labels)

runs() {  # per-workflow totals for a created-date range
  gh run list --repo "$REPO" --limit 1000 --created "$1" --json conclusion,workflowName \
    --jq 'group_by(.workflowName) | map({workflow: .[0].workflowName, runs: length,
      success: ([.[] | select(.conclusion == "success")] | length),
      failure: ([.[] | select(.conclusion == "failure")] | length)})'
}
ci=$(runs "$SINCE..$TODAY")
ci_prev=$(runs "$PRIOR..$PRIOR_END")

repo=$(gh api "repos/$REPO" --jq '{stars: .stargazers_count, forks: .forks_count, watchers: .subscribers_count}')
stars=$(gh api -H "Accept: application/vnd.github.star+json" "repos/$REPO/stargazers" --paginate \
  --jq '[.[].starred_at[0:10]]' | jq -s 'add // []')
# Traffic covers the last 14 whole days only, daily. Clones are mostly CI and
# the Pages workflow checking out the repo; they are not people.
views=$(gh api "repos/$REPO/traffic/views" --jq '.views | map({day: .timestamp[0:10], count, uniques})')
clones=$(gh api "repos/$REPO/traffic/clones" --jq '.clones | map({day: .timestamp[0:10], count, uniques})')
referrers=$(gh api "repos/$REPO/traffic/popular/referrers")

logins=$(echo "$merged" | jq -r '[.[].author.login] | map(select(. != null and (startswith("app/") | not))) | unique | .[]')
if [ -z "$logins" ]; then avatars='[]'; else
  q="{"; i=0
  for l in $logins; do q+="u$i: user(login: \"$l\"){ login avatarUrl(size: 96) } "; i=$((i + 1)); done
  avatars=$(gh api graphql -f query="$q}" | jq '[.data[] | select(. != null) | {login, avatar_url: .avatarUrl}]')
fi
contributors=$(echo "$merged" | jq --argjson av "$avatars" '
  [group_by(.author.login)[] | {login: .[0].author.login, count: length}]
  | map(. as $c | . + {avatar_url: (($av[] | select(.login == $c.login) | .avatar_url) // "")})
  | sort_by(-.count)')

jq -n --arg since "$SINCE" --arg prior "$PRIOR" --arg head "$(git rev-parse --short origin/main)" \
  --argjson merged "$merged" --argjson merged_prev "$merged_prev" --argjson open "$open_prs" \
  --arg last_tag "$last_tag" --arg tag_date "$tag_date" --argjson since_tag "$since_tag" --argjson since_tag_fw "$since_tag_fw" \
  --argjson releases "$releases" --argjson issues "$issues" --argjson ci "$ci" --argjson ci_prev "$ci_prev" \
  --argjson repo "$repo" --argjson stars "$stars" --argjson views "$views" --argjson clones "$clones" \
  --argjson referrers "$referrers" --argjson contributors "$contributors" '
{
  since: $since, prior_window_start: $prior, main_head: $head,
  merged: $merged, merged_count: ($merged | length), merged_prev_count: ($merged_prev | length),
  lines: {added: ([$merged[].additions] | add // 0), removed: ([$merged[].deletions] | add // 0),
          prev_added: ([$merged_prev[].additions] | add // 0), prev_removed: ([$merged_prev[].deletions] | add // 0)},
  open: $open, open_ready: [$open[] | select(.isDraft | not)], open_draft: [$open[] | select(.isDraft)],
  release: {last_tag: $last_tag, tag_date: $tag_date, commits_since: $since_tag, firmware_commits_since: $since_tag_fw},
  releases: $releases,
  issues_opened: [$issues[] | select(.createdAt[0:10] >= $since)],
  issues_closed: [$issues[] | select(.closedAt != null and .closedAt[0:10] >= $since)],
  issues_opened_prev: ([$issues[] | select(.createdAt[0:10] >= $prior and .createdAt[0:10] < $since)] | length),
  issues_closed_prev: ([$issues[] | select(.closedAt != null and .closedAt[0:10] >= $prior and .closedAt[0:10] < $since)] | length),
  ci: $ci, ci_prev: $ci_prev,
  repo: $repo, stars_by_day: $stars, views: $views, clones: $clones, referrers: $referrers,
  contributors: $contributors
}'
