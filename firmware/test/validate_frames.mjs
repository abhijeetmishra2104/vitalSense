#!/usr/bin/env node
/**
 * Frame-contract check: real firmware frames through the real server validator.
 *
 * Builds `host_device` (the portable firmware core compiled for the host, D-25), feeds
 * it synthetic ADC counts covering the situations the protocol has to express - a clean
 * signal, a leads-off interval, a railed amplifier, a link outage with store-and-forward
 * - and pipes every frame it emits into the server's own `validateSampleFrame()`.
 *
 * The firmware and the server were written against the same document
 * (docs/protocol.md). This script is what turns "they agree" from an intention into a
 * measurement: any frame the server would reject, or accept with a warning, fails here.
 *
 *   cd server && npm run validate:frames
 *
 * Exits non-zero on any failed check, so CI can gate on it.
 */

import { spawn, spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..', '..');
const SERVER_SRC = join(ROOT, 'server', 'src');

const { validateSampleFrame, SUPPORTED_VERSIONS } = await import(join(SERVER_SRC, 'protocol.js'));
const { SAMPLE_RATE_HZ } = await import(join(SERVER_SRC, 'dsp', 'coefficients.js'));

const FS = SAMPLE_RATE_HZ;
const FRAME_SAMPLES = 36; // 100 ms frames, as the device sends
const ZERO_CODE = 2048; // mid-supply; VS_ADC_ZERO_CODE in the generated header
const FULL_SCALE = 4095;

// --- checks -----------------------------------------------------------------------
let passed = 0;
let failed = 0;
function check(name, ok, detail = '') {
  if (ok) passed += 1;
  else failed += 1;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name.padEnd(70)} ${detail}`);
}

// --- build the emulator -----------------------------------------------------------
const binary = join(HERE, 'build', 'host_device');
const make = spawnSync('make', ['device'], { cwd: HERE, encoding: 'utf8' });
if (make.status !== 0 || !existsSync(binary)) {
  console.error(make.stdout, make.stderr);
  console.error('could not build firmware/test/build/host_device - is a C compiler installed?');
  process.exit(2);
}

// --- synthetic acquisition --------------------------------------------------------
/**
 * ADC counts for `seconds` of a plausible ECG at `hr` bpm: a narrow QRS on a 1 mV
 * scale, a little mains pickup for the notch to remove, and the electrode offset the
 * amplifier sits at. Roughly 2441 nV per count (generated calibration), so 1 mV ≈ 410
 * counts. Segments may override the signal: 'lo' holds the LO pins (handled by the
 * emulator's flag), 'rail' pins the converter to its top code as a detached electrode
 * does through the amplifier.
 */
function synthesize({ seconds, hr = 72, rail = null }) {
  const n = Math.round(seconds * FS);
  const counts = new Int32Array(n);
  const beatPeriod = 60 / hr;
  for (let i = 0; i < n; i += 1) {
    const t = i / FS;
    const phase = (t % beatPeriod) / beatPeriod;
    let mv = 0;
    // QRS: a 40 ms triangular spike, 1.2 mV.
    const qrs = Math.abs(phase - 0.1) / 0.02;
    if (qrs < 1) mv += 1.2 * (1 - qrs);
    // T wave: broad, 0.25 mV.
    const tw = (phase - 0.35) / 0.08;
    if (Math.abs(tw) < 1) mv += 0.25 * Math.cos((tw * Math.PI) / 2);
    // Mains pickup surviving CMRR, 0.05 mV at 50 Hz.
    mv += 0.05 * Math.sin(2 * Math.PI * 50 * t);
    let code = ZERO_CODE + Math.round((mv * 1e6) / 2441);
    if (rail && i >= rail.from && i < rail.to) code = FULL_SCALE;
    counts[i] = Math.min(FULL_SCALE, Math.max(0, code));
  }
  return counts;
}

function runDevice(counts, args) {
  return new Promise((resolveRun, reject) => {
    const child = spawn(binary, args, { cwd: HERE });
    let out = '';
    let err = '';
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { err += d; });
    child.on('error', reject);
    child.on('close', (code) => resolveRun({ code, lines: out.split('\n').filter(Boolean), stderr: err.trim() }));
    child.stdin.end(Array.from(counts).join('\n') + '\n');
  });
}

function validateAll(lines) {
  const results = [];
  for (const line of lines) {
    let parsed;
    try {
      parsed = JSON.parse(line);
    } catch (e) {
      results.push({ ok: false, errors: [`not JSON: ${e.message}`], warnings: [], frame: null, raw: line });
      continue;
    }
    results.push({ ...validateSampleFrame(parsed, { expectedFs: FS }), raw: parsed });
  }
  return results;
}

// --- scenario 1: a clean recording -------------------------------------------------
{
  const seconds = 20;
  const counts = synthesize({ seconds });
  const run = await runDevice(counts, ['--device-id', 'esp32-host', '--frame-samples', String(FRAME_SAMPLES)]);
  const results = validateAll(run.lines);
  const rejected = results.filter((r) => !r.ok);
  const warned = results.filter((r) => r.ok && r.warnings.length);
  const expectedFrames = Math.ceil(counts.length / FRAME_SAMPLES);

  check('the emulator exits cleanly', run.code === 0, run.stderr);
  check('every frame is accepted by the server validator', rejected.length === 0,
    `${results.length - rejected.length} of ${results.length} accepted` +
      (rejected[0] ? ` - first rejection: ${rejected[0].errors.join('; ')}` : ''));
  check('no frame carries a warning', warned.length === 0,
    warned[0] ? warned[0].warnings.join('; ') : `${results.length} frames clean`);
  check('every sample arrives in exactly one frame', results.length === expectedFrames &&
    results.reduce((n, r) => n + (r.frame?.ecg.length ?? 0), 0) === counts.length,
    `${results.length} frames, ${results.reduce((n, r) => n + (r.frame?.ecg.length ?? 0), 0)} samples`);
  check('frames declare a supported protocol version', results.every((r) => SUPPORTED_VERSIONS.includes(r.raw?.v)),
    `v${results[0]?.raw?.v}`);
  check('seq is contiguous from 0', results.every((r, i) => r.frame?.seq === i), `${results.length} frames`);
  check('tDevice advances by one frame period', results.slice(1).every((r, i) =>
    Math.abs((r.frame.tDevice - results[i].frame.tDevice) - (FRAME_SAMPLES * 1000) / FS) <= 1),
    `${(FRAME_SAMPLES * 1000) / FS} ms per frame`);
  check('fs matches the DSP sampling rate exactly', results.every((r) => r.frame?.fs === FS), `${FS} Hz`);
  // The detector starts "disconnected" on purpose - "I do not know yet" is the only safe
  // default - and needs 500 ms of good signal to clear (D-28). After that, never again.
  const settle = Math.ceil((0.5 * FS) / FRAME_SAMPLES) + 1;
  check('leadsOff starts asserted and clears within the 500 ms de-assert debounce',
    results[0].frame?.leadsOff === true && results.slice(settle).every((r) => r.frame?.leadsOff === false),
    `clear from frame ${results.findIndex((r) => r.frame?.leadsOff === false)}`);
  const peak = Math.max(...results.flatMap((r) => r.frame?.ecg ?? []));
  check('QRS amplitude survives counts -> mV in the expected range', peak > 0.9 && peak < 1.6, `${peak.toFixed(3)} mV peak`);
  check('absent sensors are null, never a plausible constant', results.every((r) =>
    r.frame?.spo2 === null && r.frame?.temp === null && r.raw?.spo2 === null && r.raw?.temp === null),
    'spo2 and temp null');
  check('no PPG on the host, stated explicitly', results.every((r) => r.raw?.ppg === null), '"ppg":null');
}

// --- scenario 2: electrodes detached, via the LO pins ----------------------------
{
  const seconds = 10;
  const counts = synthesize({ seconds });
  const loFrom = 3 * FS;
  const loTo = 6 * FS;
  const run = await runDevice(counts, ['--frame-samples', String(FRAME_SAMPLES), '--lo-from', String(loFrom), '--lo-to', String(loTo)]);
  const results = validateAll(run.lines);
  const off = results.filter((r) => r.frame?.leadsOff === true);
  // Skip the start-up assertion: look for the first flagged frame after the detector has cleared.
  const firstClear = results.findIndex((r) => r.frame?.leadsOff === false);
  const firstOff = results.findIndex((r, i) => i > firstClear && r.frame?.leadsOff === true);
  const lastOff = results.map((r) => r.frame?.leadsOff).lastIndexOf(true);

  check('leads-off frames are still valid frames', results.every((r) => r.ok), `${results.length} accepted`);
  check('leadsOff asserts while the LO pins are held', off.length > 0, `${off.length} frames flagged`);
  // 50 ms assert debounce: the first flagged frame is the one containing sample loFrom + 18.
  check('leads-off asserts within its 50 ms debounce',
    firstOff >= 0 && Math.abs(firstOff - Math.floor((loFrom + 0.05 * FS) / FRAME_SAMPLES)) <= 1,
    `first flagged frame ${firstOff}`);
  // 500 ms de-assert debounce: stays flagged for ~0.5 s after the pins release.
  check('leads-off clears only after its 500 ms de-assert debounce',
    lastOff >= 0 && Math.abs(lastOff - Math.floor((loTo + 0.5 * FS) / FRAME_SAMPLES)) <= 1,
    `last flagged frame ${lastOff}`);
}

// --- scenario 3: a railed amplifier, no LO signal ----------------------------------
{
  const seconds = 8;
  const rail = { from: 2 * FS, to: 5 * FS };
  const counts = synthesize({ seconds, rail });
  const run = await runDevice(counts, ['--frame-samples', String(FRAME_SAMPLES)]);
  const results = validateAll(run.lines);
  const railed = results.filter((r) => r.frame?.leadsOff === true);
  check('a railed converter is reported as leads-off even without the LO pins', railed.length > 0, `${railed.length} frames flagged`);
  check('railed samples stay inside the protocol amplitude limit', results.every((r) => r.ok),
    `${results.filter((r) => r.ok).length} of ${results.length} accepted`);
}

// --- scenario 4: a link outage, store and forward (SR-05) ---------------------------
{
  const seconds = 30;
  const counts = synthesize({ seconds });
  const outFrom = 5 * FS;
  const outTo = 25 * FS; // 20 s outage, inside the 30 s buffer
  const run = await runDevice(counts, ['--frame-samples', String(FRAME_SAMPLES), '--outage-from', String(outFrom), '--outage-to', String(outTo)]);
  const results = validateAll(run.lines);
  const delivered = results.reduce((n, r) => n + (r.frame?.ecg.length ?? 0), 0);

  check('frames emitted after an outage are all valid', results.every((r) => r.ok), `${results.length} accepted`);
  check('a 20 s outage loses zero samples', delivered === counts.length, `${delivered} of ${counts.length} delivered`);
  check('seq stays contiguous across the outage', results.every((r, i) => r.frame?.seq === i), 'no gaps');
  check('the emulator reports nothing dropped', /\b0 dropped\b/.test(run.stderr), run.stderr);

  // Compare against the no-outage run sample for sample: buffering must be lossless,
  // not merely count-preserving.
  const reference = validateAll((await runDevice(counts, ['--frame-samples', String(FRAME_SAMPLES)])).lines);
  const a = results.flatMap((r) => r.frame?.ecg ?? []);
  const b = reference.flatMap((r) => r.frame?.ecg ?? []);
  const identical = a.length === b.length && a.every((v, i) => v === b[i]);
  check('samples through the outage are identical to the no-outage run', identical, `${a.length} samples compared`);
}

// --- scenario 5: an outage longer than the buffer --------------------------------------
{
  const seconds = 50;
  const counts = synthesize({ seconds });
  const run = await runDevice(counts, ['--frame-samples', String(FRAME_SAMPLES), '--outage-from', String(2 * FS), '--outage-to', String(45 * FS)]);
  const results = validateAll(run.lines);
  const delivered = results.reduce((n, r) => n + (r.frame?.ecg.length ?? 0), 0);
  const m = run.stderr.match(/(\d+) dropped/);
  const dropped = m ? Number(m[1]) : NaN;
  check('an outage beyond the buffer reports its loss rather than hiding it', dropped > 0, `${dropped} samples dropped`);
  check('what was delivered plus what was dropped accounts for every sample', delivered + dropped === counts.length,
    `${delivered} + ${dropped} = ${counts.length}`);
  check('the most recent samples are the ones kept', results.every((r) => r.ok) && delivered >= 30 * FS - FRAME_SAMPLES,
    `${(delivered / FS).toFixed(1)} s delivered`);
}

console.log('-'.repeat(90));
console.log(`${passed}/${passed + failed} checks passed`);
process.exit(failed === 0 ? 0 : 1);
