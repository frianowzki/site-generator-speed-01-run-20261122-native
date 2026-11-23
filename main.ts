// N-API fast path: engine.node runs the whole build in-process, then the
// process exits via exitGroup (SYS_exit_group). Falls back to solve.js.
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
const D = fileURLToPath(new URL('.', import.meta.url));
const C = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const A = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';
try {
  const N = D + 'engine.node';
  const K = D + 'nfkd.bin';
  if (existsSync(N) && existsSync(K)) {
    const a = createRequire(import.meta.url)(N);
    if (a.build(C, A, K) === 0) {
      if (a.exitGroup) a.exitGroup(0);
      process.exit(0);
    }
  }
} catch {}
const { setFlagsFromString } = await import('node:v8');
setFlagsFromString('--no-incremental-marking');
const { run } = await import(new URL('./solve.js', import.meta.url).href);
const cases = JSON.parse(readFileSync(C, 'utf8'));
const answers = [];
for (const c of cases) answers.push(run(c.input));
writeFileSync(A, JSON.stringify(answers));
