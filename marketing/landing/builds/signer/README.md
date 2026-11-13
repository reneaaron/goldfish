# Landing page

A scroll-driven page for the Stateless Signer: one continuous camera flight the
scroll scrubs, board to case to scan to sign to unplug.

```bash
python3 -m http.server 4512      # from this folder, then open localhost:4512
```

| File | What it is |
|---|---|
| `index.html` | the page: markup, tokens, the waypoint map and the ink |
| `scrollcraft.js`, `scrollcraft.css` | the scroll engine, unmodified |
| `assets/` | five legs (`legN.mp4`, `legN-m.mp4`) and their posters |
| `encode-legs.sh` | cuts rendered frames into those legs |
| `BRIEF.md` | the brief, the feeling curve, and what was verified |
| `lab/` | scroll screenshots and the probes used to check them |

Regenerating the film: see the Landing page section of `marketing/README.md`.
The frames come from `marketing/renders/flight.py`, which uses the real case STL
and the firmware's real simulator screens, so nothing on the page is invented.
