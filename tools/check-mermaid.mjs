#!/usr/bin/env node
// Parses every Mermaid diagram in the repository's Markdown and fails on the first one that
// does not.
//
// GitHub renders these blocks itself, and a diagram that does not parse is replaced *in whole* by
// an error box. So one bad character costs the entire diagram, and nothing in the rest of the lint
// suite looks inside a fenced code block. The break that prompted this check was a class-diagram
// relationship label reading "via std::function": a label runs from its colon to the end of the
// line, so the second colon ended the parse.
//
// It runs the same parser GitHub does, rather than a pattern list of things known to break. A
// pattern list only ever covers the mistakes already made once.
//
// Usage:
//   node tools/check-mermaid.mjs [file.md ...]
//
// With no arguments it checks every tracked Markdown file. Needs `mermaid` and `jsdom`, which the
// lint workflow installs; locally, `npm install --no-save mermaid jsdom`.

import { execFileSync } from "node:child_process";
import fs from "node:fs";

// Mermaid expects a browser. jsdom is enough: nothing here lays anything out, and parse() only
// needs a document to exist.
let JSDOM;
let mermaid;
try {
  ({ JSDOM } = await import("jsdom"));
  const dom = new JSDOM("<!DOCTYPE html><body></body>", { pretendToBeVisual: true });
  globalThis.window = dom.window;
  globalThis.document = dom.window.document;
  globalThis.navigator = dom.window.navigator;
  mermaid = (await import("mermaid")).default;
} catch (e) {
  console.error("check-mermaid: cannot load the parser — install it with:");
  console.error("  npm install --no-save mermaid jsdom");
  console.error(`(${e.message})`);
  process.exit(2);
}

mermaid.initialize({ startOnLoad: false });

const files =
  process.argv.length > 2
    ? process.argv.slice(2)
    : execFileSync("git", ["ls-files", "*.md"], { encoding: "utf8" }).split("\n").filter(Boolean);

let checked = 0;
let failed = 0;

for (const file of files) {
  const text = fs.readFileSync(file, "utf8");
  // Line numbers so a failure names the line in the file, not the line in the block. Mermaid
  // reports the latter, and the two differ by wherever the fence starts.
  const fence = /^```mermaid[^\n]*\n([\s\S]*?)^```/gm;
  for (let m; (m = fence.exec(text)) !== null; ) {
    checked++;
    const firstLine = text.slice(0, m.index).split("\n").length + 1;
    try {
      await mermaid.parse(m[1]);
      console.log(`ok    ${file}:${firstLine}`);
    } catch (e) {
      failed++;
      const detail = String(e?.message ?? e).trimEnd();
      console.log(`FAIL  ${file}:${firstLine} (diagram starts here)`);
      console.log(detail.replace(/^/gm, "      "));
    }
  }
}

if (checked === 0) {
  console.log("check-mermaid: no diagrams found");
} else {
  console.log(`\n${checked - failed}/${checked} diagrams parse`);
}
process.exit(failed > 0 ? 1 : 0);
