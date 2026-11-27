---
name: weekly-review
description: Build the Goldfish and Dove weekly review as a page in the Goldfish website's style - what shipped, the work by area, what's in flight, the repo's numbers, findings, and what to do first. Use for the Friday review, a scheduled weekly run, or any "how did the week go" ask about this repo. Reads only GitHub (gh) and git.
---

# weekly-review: the Friday review (internal)

One page that says what the week added up to, what is waiting, and what to do
first. The reader is the team: reneaaron, rolandgx, rolznz.

Goldfish has no product analytics: the site loads nothing third party and GitHub
Pages keeps no logs. Every number comes from `gh` and git. Say so on the page
rather than letting repo traffic pass for users.

## Window

Trailing 7 days ending at run time, against the 7 days before. State it on the
page in Europe/Berlin. GitHub search dates are UTC days; say that in the footer.

## Step 1: gather

```bash
./build/gather.sh <window start, YYYY-MM-DD> > week.json
```

Run it from this skill's folder. It returns merged and open PRs, the release
state (last `v*` tag, commits and `firmware/src` commits since), issues opened and
closed, Actions runs per workflow, stars, traffic, referrers and contributors with
avatar URLs, for both windows where GitHub allows.

The spine, carried every week so weeks compare:

| Group | Carries |
|-------|---------|
| Delivery | PRs merged, lines added/removed (say when case STLs dominate), open PRs ready and draft |
| Release | releases tagged, commits and firmware commits since the last tag, `firmware.uf2` downloads |
| Quality | Actions runs, successes and failures per workflow; cancelled runs are superseded pushes, not failures |
| Reach | repo views and peak uniques, top referrer, stars/forks/watchers, site visits (always "not measured") |
| Backlog | issues opened and closed, by author; open security issues |

Every metric gets a current and a prior value, or a note saying why it has none.

## Step 2: the work

**Shipped.** Describe each merged PR by what it changes for someone holding a
board or using Dove, not by the diff. Group by area; the usual groups are
signing and scanning, seeds and handwriting, device feel, security/tests/release,
Dove, website and guides, case and brand. Several PRs may share one row. Leave
out copy tweaks, renders refreshes, dependency bumps, and say how many you left
out. Check every merged PR is either described or deliberately omitted.

**The work, by area.** Above Shipped: one card per area with the PR count, two or
three sentences on what got better, and one line on what's open there (linked).
Mark an area "Needs review" only when a ready PR in it fixes a security or signing
issue. The counts add up to the described PRs.

**In flight.** Open PRs, ready first, then drafts that bear on a finding; cap at
about 16 rows and name the ones left out. Never present them as shipped.

**Unreleased firmware is the item that bites.** Only a `v*` tag publishes
`firmware.uf2`; the landing hero and `/sim/` run `main`; the flasher and Dove's
updater install the last release. If firmware commits sit past the last tag, the
site shows behaviour no board has. Say which user-visible changes those are.

## Step 3: findings, do first, what if

Findings: at most six, ranked by what they change for someone holding a board.
Each has a claim-led heading, a one-line measurement, a verified cause and a "so
what". Mark one "Attention" only when it needs a decision this week. Verify
before writing:

- Read the PR or issue body, not the title. A security issue's own PR often
  says which candidates were reproduced and which were not.
- Traffic clones are CI and the Pages workflow; downloads include the team's own
  flashes and Dove's tests. Never call either users.
- Before recommending work, search issues and PRs for it
  (`gh search issues --repo reneaaron/goldfish --include-prs "<term>"`), link what
  you find, and say "not tracked yet" when nothing does.

Do first: two items, each saying why it's ranked there. Find last week's page
(Artifact `action: "list"`, title "Goldfish Weekly Review ...", read the newest)
and say in one line what its ranking got right or wrong; on a first run say so.

What if: three provocations from this week's numbers, each with the cheapest
thing that would show it wrong. Cut any without one.

## Step 4: build and publish

Write `data.json` in the shape of `build/example.json` (every field it has), then:

```bash
python3 build/build.py data.json goldfish-weekly-review-<end date>.html
```

Put both files in the session's scratchpad, not the repo. `build/template.html`
carries the site's tokens, nav and the goldfish and dove lockups; don't restyle
per week. The builder embeds avatars and the header, links every bare `#123`, and
refuses em dashes, external images, a tile with no prior, more than six findings,
and a lede paragraph over 70 words.

Header: one of the project's own renders in `marketing/renders/out/`, a different
one from last week, given as an absolute path (it resolves against `data.json`'s folder otherwise), with `header_crop` (an ffmpeg filter) to the devices and a short Caveat
`header_note`. Never a generated image.

Publish with the Artifact tool as a new artifact titled
`Goldfish Weekly Review <end date>` (for example `Goldfish Weekly Review 4 Dec`),
then `xdg-open` the URL. Report the URL, the areas and the findings in the terminal
too.

## Copy

- Copy rules from the root CLAUDE.md apply: no "air-gapped" unqualified, no
  claimed audits or compatibility that wasn't tested.
- No em dashes. Plain, friendly, short. "goldfish" and "dove" are lowercase only in
  the lockups; prose capitalises them.
- Colour never carries state alone: every coloured delta also says up/down in
  words, and tone (good/bad/flat) is whether it's good, not which way it moved.
