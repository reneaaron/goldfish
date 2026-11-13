# Fingerprints

Every site you build with **scroll-craft** gets one row here, appended after it
ships. The registry exists so your next build can prove it is a different page
rather than a re-skin of one you already made.

This file is **yours**. It starts empty on purpose: the gate is about not
repeating *yourself*, so it has nothing to say until you have built something.

The rules and the gate live in the skill's
`references/uniqueness.md`. Short version:

**A new build must differ from EVERY row below on at least 4 of the 6
dimensions.** Four against each row individually, not four on average across the
table. If a planned build fails, change the plan. Never edit a row to make room
for it.

The six dimensions are: **grammar**, **nav treatment**, **hero device**,
**act-sequence shape**, **close pattern**, **signature move**.

Dimension 6 is free, because a signature move is unique by definition. So the
gate really asks for three more out of the remaining five, and a build that
changes only grammar and world will fail it.

---

## The registry

| Build | Grammar | Nav treatment | Hero device | Act-sequence shape | Close pattern | Signature move | World | Port |
|---|---|---|---|---|---|---|---|---|
| signer | Continuous world (worldflight) | Clickable waypoint map: labelled legs on a dashed rule with a marker that tracks scroll | Establishing position inside the world: the board hangs over its open case, copy on the empty paper beside it | 5 legs, weights 1.3 / 1.82 / 1.56 / 1.56 / 1.3, 7.54vh track + 1vh landing | Arrival at a top-down place in the same canvas; the CTA is an object on that paper | None. An ink layer that wiped at the unplug frame was built and removed: it painted under the cursor and read as a fault. The slot is open. | Paper and ink, photoreal Blender renders of the real case STL and the firmware's real screens | 4512 |

---

## What is taken

Add a bullet here whenever a build claims something a later build should avoid
reusing: a grammar, a nav treatment, a close pattern, a signature move, an
act-count-and-length band. The shared columns are what the next build inherits
as a constraint, so writing them down is the whole point.

- **Continuous world / worldflight** as a grammar.
- **A clickable waypoint map with a live readout** as nav.
- **Hero as an establishing position inside the world** (no separate title stage).
- **A 5-leg flight on a 7.54vh track**, peak leg weighted 1.82.
- **Close as arrival at a place in the same canvas**, CTA as an object in it.
- **Paper-and-ink world built from the project's own CAD and firmware screens**
  rather than generated imagery.

---

## Appending a row

After shipping, add one line to the table and one bullet to **What is taken** if
the build claimed something new. Fill every column. Say what the build shares
with existing rows.

Rows are append-only. A build that has been superseded stays in the table,
because the space it occupies is still occupied.

---

## Worked example

The skill's author kept a registry of twelve builds across eight page grammars.
If you want to see what a filled-in table looks like, and which shapes tend to
collide, read `EXAMPLES.md` in the scroll-craft repository. Treat it as
illustration only: those rows are somebody else's builds and they do **not**
constrain yours.
