import { defineConfig } from 'astro/config';

// The site is served from a subpath (reneaaron.github.io/goldfish/), so pages link
// relatively and styles are inlined: nothing points at an absolute /_astro/ path.
export default defineConfig({
  build: { inlineStylesheets: 'always' },
  // Compression drops the space in "the <a>…" when the link starts a new line.
  compressHTML: false,
});
