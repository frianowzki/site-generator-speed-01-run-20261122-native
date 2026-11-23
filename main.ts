// The harness runs `node /bot/main.ts` (node22). It reads every case from
// /input/cases.json (a JSON array of {id, input}), answers each one alone
// and writes /output/answers.json: a JSON array of the answers, one per
// case and in the cases' order. Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// Fast path: a statically-linked C engine (./engine) reads cases.json and
// writes answers.json itself, so Node never parses the input. The binary
// is copied to a fresh temp dir and executed from there, because /bot is
// read-only in the grading sandbox. Any failure at any step falls back to
// the pure-JS driver below (solve.js, the v8 entry), which is byte-exact.

import { readFileSync, openSync, readSync, closeSync, writeFileSync, existsSync, statSync, mkdtempSync, copyFileSync, chmodSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { setFlagsFromString } from 'node:v8';

// V8's incremental marking off, set before solve.js is imported so all of its
// work runs under it (the rules: the house baseline runs with
// --no-incremental-marking, and an entry may tune V8's flags too).
setFlagsFromString('--no-incremental-marking');

const BOT_DIR = dirname(fileURLToPath(import.meta.url));
const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';
const CASES = join(INPUT_DIR, 'cases.json');
const ANSWERS = join(OUTPUT_DIR, 'answers.json');

/** Run the native engine. Returns true only if answers.json was produced and looks sane. */
function tryNative(): boolean {
  try {
    const engineSrc = join(BOT_DIR, 'engine');
    const nfkdSrc = join(BOT_DIR, 'nfkd.bin');
    if (!existsSync(engineSrc) || !existsSync(nfkdSrc)) return false;
    const dir = mkdtempSync(join(tmpdir(), 'mf-native-'));
    try {
      const enginePath = join(dir, 'engine');
      const nfkdPath = join(dir, 'nfkd.bin');
      copyFileSync(engineSrc, enginePath);
      copyFileSync(nfkdSrc, nfkdPath);
      chmodSync(enginePath, 0o755);
      const r = spawnSync(enginePath, [CASES, ANSWERS, nfkdPath], { stdio: 'ignore' });
      if (r.status !== 0) return false;
      if (!existsSync(ANSWERS)) return false;
      if (statSync(ANSWERS).size < 2) return false;
      // Read one byte only: the whole answers file is megabytes, and Node
      // must not pay to load it just to check it starts with '['.
      const fd = openSync(ANSWERS, 'r');
      try {
        const one = new Uint8Array(1);
        if (readSync(fd, one, 0, 1, 0) !== 1) return false;
        if (one[0] !== 0x5b) return false; // must start with '['
      } finally {
        closeSync(fd);
      }
      return true;
    } finally {
      try { rmSync(dir, { recursive: true, force: true }); } catch { /* best effort */ }
    }
  } catch {
    return false;
  }
}

if (!tryNative()) {
  // Fallback: the pure-JS driver (v8 solve.js), byte-exact on the practice
  // set and on 200 fuzzed sites.
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(readFileSync(CASES, 'utf8')) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(ANSWERS, JSON.stringify(answers));
}
