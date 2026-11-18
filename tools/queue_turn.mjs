// Holds one job in a GPU queue while a tools/remote_build.sh run goes on beside it (that script's header has why): files
// it through the queue's client (/q/gpuq.mjs, its gpuRun(spec, run)) as a low-priority job of type build asking no GPU
// memory and <ram MiB> of the host's (what remote_build.sh read off its last runs), writes <dir>/waiting with its
// place in line while it waits, <dir>/running once it is admitted, and holds it until <dir>/release appears (3 hours
// at most). A stop the queue asks for ends the job and is written to <dir>/stopped, the run going on outside the
// queue; a job the queue refuses or ends unrun writes <dir>/ended.
//
//   node queue_turn.mjs <dir> <label> <ram MiB> [<url>]
import fs from 'node:fs';
const [dir, label, ram, url] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
try {
  const { gpuRun } = await import('/q/gpuq.mjs');
  await gpuRun({ type: 'build', label: 'Build', vramMiB: 0, ramMiB: Number(ram), priority: 'low', ref: label,
    url: url || undefined,
    onUpdate: (j) => j.state === 'waiting' && fs.writeFileSync(`${dir}/waiting`, `${j.ahead} ahead: ${j.why?.text ?? 'going next'}\n`) },
  async (signal) => {
    fs.writeFileSync(`${dir}/running`, '');
    for (const end = Date.now() + 3 * 3600_000; Date.now() < end && !signal.aborted && !fs.existsSync(`${dir}/release`);) await sleep(1000);
    if (signal.aborted) fs.writeFileSync(`${dir}/stopped`, String(signal.reason?.message ?? signal.reason));
  });
} catch (e) {
  fs.writeFileSync(`${dir}/ended`, String(e?.message ?? e));
}
