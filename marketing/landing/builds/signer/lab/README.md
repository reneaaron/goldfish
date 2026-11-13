# lab

Scroll screenshots from `scripts/shoot.mjs`, kept out of git. Regenerate:

```bash
node <skill>/scripts/serve.mjs --root .. --port 4512 &
node <skill>/scripts/shoot.mjs --url http://localhost:4512 --out lab/shots --per-act 8
node <skill>/scripts/shoot.mjs --url http://localhost:4512 --out lab/mobile --width 390 --height 844 --per-act 6
node <skill>/scripts/shoot.mjs --url http://localhost:4512 --out lab/reduced --reduced-motion --per-act 6
node <skill>/scripts/worldflight-assert.mjs --url http://localhost:4512
```
