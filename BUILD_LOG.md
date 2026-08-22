# Build log

The engineering journal: one entry per work session — what was done, why that way,
what broke, what it taught, and the interview question it prepares for. The other two
tracking documents answer different questions: [PLAN.md](PLAN.md) says what is going to
be built, [docs/progress.md](docs/progress.md) says what is finished and what it measures.

> **Provenance.** This file was missing from the repository even though it is cited
> throughout the docs (`BUILD_LOG 002`, `005`, `006`, `016`, …). It was reconstructed on
> 2026-08-21 from `docs/progress.md`, `docs/decisions.md` and `docs/team-plan.md`, which
> record the same sessions phase by phase. Entry numbers follow the citations in those
> documents. Nothing here is claimed that those documents do not already record; where a
> session's detail is not recoverable it says so rather than inventing it.

Template for a new entry:

```
## NNN — <title>                                   Phase N
**Did:** …   **Why this way:** …   **Broke:** …   **Learned:** …   **Interview:** …
```

---

## 001 — Scaffold and dataset                                              Phase 0

**Did:** Repository layout (`dsp/`, `sim/`, `firmware/`, `server/`, `docs/`), Python venv
with pinned requirements, `dsp/fetch_data.py` pulling ten MIT-BIH records from PhysioNet.

**Why this way:** Data is fetched, never committed — the records are large and licensed
elsewhere. The ten were chosen *before* any results were seen: clean (100, 101, 103, 115),
arrhythmic (106, 119, 208) and famously noisy (105, 108, 203). Picking them afterwards
would let the verification table be cherry-picked (D-10 was already coming).

**Decisions:** D-01 simulation-first build order, because hardware was weeks away and
the DSP could be validated without it. D-02 360 Hz, to match MIT-BIH exactly and avoid
resampling the ground truth.

**Interview:** "Why simulate first?" — because the thing that is hard to get right is the
signal processing, and annotated clinical data is a better test bed than one volunteer.

---

## 002 — Filters                                                          Phase 1

**Did:** `dsp/filters.py`: 0.5–40 Hz Butterworth bandpass in second-order sections plus
a 50 Hz IIR notch, each in a zero-phase offline mode and a stateful streaming mode, with
a C-coefficient export and 15 self-checks.

**Why this way:** Two filter paths (D-07): the detection path may distort morphology to
sharpen the QRS; the display path must not, because a clinician reads the shape. IIR over
FIR (the question this entry is cited for): the 0.5 Hz corner at 360 Hz needs an FIR of
several hundred taps for the same rolloff, and on an MCU that is the wrong trade. The
cost of IIR is numerical — see 004 and 008 for where that bit back.

**Broke:** A `butter()` call in transfer-function form went unstable at order 4 with a
0.5 Hz corner. Switched to SOS form, which is the only sane way to run a narrow-band IIR
in floating point, and certainly in fixed point later.

**Interview:** "Why 360 Hz? IIR vs FIR on an MCU?" — D-02, and the rolloff-versus-taps
argument above.

---

## 003 — Pan-Tompkins, vitals, validation                                 Phase 1

**Did:** `dsp/pan_tompkins.py` (the 1985 algorithm end to end), `dsp/synth.py`
(synthetic ECG with exact R-peak locations), `dsp/vitals.py` (RR, heart rate, SDNN,
RMSSD, rhythm class), `dsp/validate.py` scoring 22,459 annotated beats and generating
`docs/verification.md`.

**Result:** Se 98.73 % / PPV 98.64 % pooled; 99.98 % sensitivity on the clean records;
worst-case heart-rate error 2.18 bpm against ±5. SR-01 and SR-04 pass.

**Why this way:** Heart rate as the *median* of instantaneous rates (D-09): one missed
or spurious beat moves a mean by several bpm and a median by nothing. The verification
report is generated, never hand-edited (D-11) — a hand-maintained results table drifts
and still looks authoritative.

**Broke:** Records 108 and 203 dragged PPV down through baseline wander and noise. A
flat-line amplitude gate (D-08) before the adaptive threshold stopped the detector
locking onto noise during quiet stretches. The honest fix — a signal-quality metric — was
deferred to Phase 3.5 and the bad numbers were published as they were (D-10).

**Learned:** The adaptive threshold in Pan-Tompkins has no notion of "this is not ECG".
Every later quality decision in the project descends from this observation.

---

## 004 — Wire protocol and the streaming port                              Phase 4a

**Did:** `docs/protocol.md`, `dsp/export_coefficients.py` generating the firmware header
*and* the server's `coefficients.js` from one design (D-12), `server/src/dsp/` — biquad
cascade, ring buffer, O(1) moving average, a causal streaming Pan-Tompkins — and
`server/tools/validateStreaming.js` scoring the port on the same records and window.

**Result:** Streaming Se 98.69 % / PPV 98.63 %. Not worse than the offline pipeline;
slightly more false positives traded for better sensitivity on the hard record. 23 tests.

**Why this way:** Score the port, do not assume equivalence (D-13). The offline pipeline
is zero-phase; the causal one cannot be, so the ±150 ms match window matters more and
timing error grows from ≈3 ms to ≈10 ms. Frames carry a sequence number (D-15) and absent
readings are `null`, never a stale number (D-16).

**Broke — the NaN.** A single `NaN` sample entered a biquad and the output was `NaN`
forever: in a recursive filter, the state carries it. An FIR would have flushed it out of
the window. That asymmetry is a real argument for FIR in safety-critical acquisition,
and it is why the validator now rejects any non-finite sample before the DSP sees it.

**Interview:** "IIR vs FIR?" — the rolloff argument from 002 *and* this failure mode.

---

## 005 — Ingest server, alarm engine, replay simulator                    Phase 4b

**Did:** `protocol.js` (total frame validation), `alarms.js` (declarative rules with
sustain times), `session.js` (per-device DSP, rate window, alarm latching, frame
accounting), `index.js` (`/ingest`, `/stream`, `/health`, silent-device watchdog),
`sim/replay.py` streaming MIT-BIH at true 360 Hz with fault-injection scenarios.

**Result, record 100 end to end:** 71.8–75.3 bpm against 75.3 reference; leads-off alarm
at 1.5 s against a 2 s requirement; SpO₂ alarm at exactly its 10 s sustain. 19 tests.

**Broke, #1 — the first asystole bug.** ASYSTOLE fired the moment a session started,
because "seconds since last beat" counted from zero and nothing had been measured yet.
An alarm about an absence has to know whether anything was ever present. Fixed by
requiring a usable signal before the rule is evaluated; revisited from the other
direction in D-47 (see 015).

**Broke, #2 — the debounce and the budget.** The leads-off debounce was 2000 ms to avoid
false alarms. SR-03 says the alarm must sound within 2 s of the electrode detaching. With
a 2 s debounce it *cannot* fire before 2 s; framing and processing pushed it to 2.1 s and
the requirement failed by construction. The debounce is part of the budget, not on top of
it (D-18): 1500 ms, leaving 500 ms for the rest of the path.

**Decisions:** D-17 beats as absolute sample indices · D-19 a more specific alarm
suppresses the general one · D-20 sessions survive a 30 s disconnect, then are reaped.

**Interview:** "Your requirement said 2 s. How did you verify it?" — this entry, #2.

---

## 006 — The dashboard, and the 190 bpm                                   Phase 4c

**Did:** `server/public/index.html` — scrolling ECG on canvas with a 25 mm/s ECG-paper
grid, per-beat carets, HR / SpO₂ / temperature tiles, severity-ranked alarm banner,
device selector. Served by the same process (D-21) so the demo is one command.

**Broke — the bug the project is about.** With record 100 playing, I ran the leads-off
scenario and reattached the electrodes. The monitor showed **190 bpm, "tachycardia",
SIGNAL GOOD**. The patient's rate was 75.

What happened: on reconnection the bandpass filter rang as it charged through the step,
the notch did too, and the adaptive threshold — trained on flat-line noise for ten
seconds — was at a level where the ringing looked like a string of perfect QRS complexes
at a steady interval. Every guard in the pipeline was a *plausibility* check, and the
artefact was perfectly plausible: regular, right amplitude, right width. Self-consistent
garbage passes every self-consistency check.

**Fix (D-22):** on electrode reconnection, discard filter and threshold state entirely
and withhold the heart rate for 3 s. Not "reset gently" — discard. And D-23: when frames
stop arriving, withdraw the numbers from the display rather than leaving the last value
up. A number on a monitor is a claim about *now*.

**Learned:** A monitor that declines to answer is safe; one that answers wrongly is not.
This became the recurring decision of the project (D-08, D-22, D-23, D-32, D-34, D-37,
D-47, D-51 are all this sentence in different places).

**Interview:** "What happens when an electrode falls off — and when it comes back?" The
strongest answer in the repository. "Hardest bug?" — this one.

---

## 007 — The analog front end, modelled                                    Phase 2′

**Did:** `sim/afe_model.py` — electrode half-cell offset and drift, motion artefact,
mains coupling surviving CMRR and the right-leg drive, an instrumentation amplifier with
E12-snapped component values, causal analog filtering, 3.3 V rails with saturation flags,
sampling jitter from an 8× oversampled signal, a 12-bit ADC with offset, gain error,
INL and noise. `sim/plot_afe.py` for the Bode and chain figures. 47 self-checks. A
second scoring pass in `validate.py` pushing the records through the chain first.

**Result:** Se 98.78 % / PPV 98.59 % through the chain — 0.05 points of PPV lost, 0.04 of
sensitivity gained. Detections arrive 7.1 ms later on average (the analog group delay),
a term SR-02's budget now carries explicitly.

**Broke — the gain.** Started at the AD8232 datasheet's ~1100. Records 203 and 208
clipped: they peak above 4 mV. Clipping a QRS is losing the one feature the device exists
to find. Gain re-sized to 330 from the ±5 mV input range IEC 60601-2-27 expects (D-26).
No bench session would have caught this, because the volunteer holding the electrodes
would not have produced a 4 mV QRS.

**Decisions:** D-24 acquisition phases re-cut as simulation · D-25 firmware must build
for the host as well as the target, so it can be scored against the same records.

---

## 008 — Firmware core: fixed-point notch and the ring buffer              Phase 3′

**Did:** `vs_notch.{h,c}` — Q14 fixed-point 50 Hz notch with steady-state priming;
`vs_ringbuf.{h,c}` — 30 s store-and-forward at 360 Hz, overwrite-oldest with the loss
counted; `firmware/test/` Makefile, host build, unit tests, `host_filter` harness and
`score_notch.py`. `export_coefficients.py` now emits the fixed-point coefficients and
refuses to write a header whose quantised poles leave the unit circle.

**Result:** Fixed-point notch vs the Python design over 6.5 M samples: 0.52 counts worst
case (1.28 µV at the electrodes); 99.46 % agreement with the correctly rounded float
result, never off by more than one count. DC drift over 200,000 samples: zero counts.

**Broke:** A notch started from zero state rings as it charges through the ~2048-count
electrode offset — hundreds of milliseconds of artefact at every start. `vs_notch_prime()`
seeds the state with the standing level instead. And a Q14 coefficient set that looked
fine in float had a pole pair that quantised to just outside the unit circle; the
generator now checks that and fails loudly.

**Decisions:** D-27 overwrite oldest and count the loss — if the link has been down longer
than the buffer holds, the recent samples are the ones worth keeping.

---

## 009 — Leads-off, the frame builder, and the contract check              Phase 3′

**Did:** `vs_leadsoff.{h,c}` (LO pins *or* a railed ADC reading, asymmetric 50 / 500 ms
debounce), `vs_frame.{h,c}` (protocol JSON with no malloc and no floating-point printf),
`vs_calibration.h` generated from the AFE model so the device's millivolt and the
pipeline's millivolt are the same millivolt, `host_device.c` and `validate_frames.mjs` —
real firmware frames piped into the server's own `validateSampleFrame()`.

**Result:** every emitted frame accepted, zero warnings. SR-05: a 20 s simulated outage
loses zero samples, output identical to the no-outage run, `seq` contiguous. An outage
beyond the buffer reports its loss rather than hiding it.

**Why this way:** The leads-off detector starts *disconnected* (D-28): "I do not know
yet" and "the patient is connected" are different claims and only one is safe by default.
Asymmetric debounce: 50 ms to assert keeps the whole path inside SR-03 (50 + 1500 server
= 1550 of 2000 ms); 500 ms to clear stops a flapping electrode producing a flapping
alarm.

**Learned:** "The firmware speaks the protocol" is a testable statement, and it was
worth testing — the first run caught an `fs` rendered as `360.0` that the server's strict
equality did not match.

---

## 010 — The ESP32 shell and Wokwi                                        Phase 3′

**Did:** `main.cpp` — timer ISR that only gives a semaphore (D-29), five FreeRTOS tasks,
WiFi, WebSocket client, and a serial transport behind the same frame builder (D-31) so
the simulator can drive it. `platformio.ini` with a pinned platform (D-30), `wokwi.toml`
and `diagram.json`.

**Result:** The image builds — RAM 21.4 %, flash 69.6 %. The Wokwi diagram loads with
every part and connection resolving.

**Left open, honestly:** The Wokwi simulation was never *run* — it needs a licence the
session did not have. The wiring is confirmed; the behaviour is not. Nothing has touched a
board. (The PlatformIO and Wokwi files are not in the current tree; see progress.md.)

---

## 011 — SpO₂: ratio of ratios                                             Phase 3.5

**Did:** `dsp/ppg.py` — red/IR synthesiser with known saturation, ratio-of-ratios,
perfusion index, and four refusal gates. Protocol v2: the device sends raw red and IR
light and the server derives saturation (D-39 keeps v1 working).

**Result:** 432 synthetic recordings — 216 accurate, 216 refused, **0 wrong readings
reported**. Recovery on clean 80–100 % input: worst error 0.00 %.

**Why this way:** Refuse when red and IR disagree on the pulse (D-32) — they are looking
at the same artery, so disagreement means one of them is not looking at an artery. The
calibration curve that turns R into a percentage is an *assumption* (D-34), so R is
reported beside the percentage; validating the curve needs co-oximetry in a controlled
desaturation study, which no public dataset provides.

---

## 012 — Signal quality: when not to answer                               Phase 3.5

**Did:** `dsp/quality.py` — pSQI, basSQI, kurtosis, and QRS energy concentration →
`good` / `poor` / `unusable`. Gated into the validation pass.

**Result:** Record 108 PPV 90.95 → 97.37 %. Arrhythmic records 106 and 119: 0.0 %
flagged unusable — the gate does not silence ventricular beats, which was the fear.

**Why this way:** The energy-concentration threshold is derived from SR-01 (D-35): the
question asked is "how much of the integrator energy sits inside the QRS windows", and
the cut-off is the level below which the rate error exceeds ±5 bpm. The gate's residual
— what still gets through — is published rather than hidden (D-37).

---

## 013 — Pulse rate on real ICU data, and the JS port                     Phase 3.5

**Did:** `dsp/fetch_ppg.py` and `dsp/validate_ppg.py` on ten BIDMC recordings; pulse rate
from the autocorrelation period rather than a peak count (D-36); `server/src/dsp/ppg.js`
with coefficients and thresholds generated from Python; `server/tools/validatePpg.js`.

**Result:** Pulse rate mean error 1.04 bpm, 98.6 % within ±5. The JS port against the
Python design on 25 cases: 0.0000 difference on SpO₂, pulse, R and channel correlation,
and every accept/refuse decision identical.

**Broke:** Peak picking is where two implementations of "the same" algorithm diverge —
tie-breaking, edge handling, refractory rounding. It is written out explicitly in both
languages rather than leaning on `scipy.signal.find_peaks` in one and something
approximately similar in the other (D-38). The PPG refractory period is *derived* from
the current rate estimate rather than fixed (D-33), because a fixed 300 ms refractory
halves a 200 bpm pulse.

---

## 014 — Persistence and history                                          Phase 4′

**Did:** `server/src/storage/` — one contract, three implementations (`NullStore`,
`MemoryStore`, `PostgresStore`), `schema.sql`, `docker-compose.yml`; `history.js` —
session listing, detail, 1 Hz vitals, CSV export.

**Why this way:** Store 1 Hz numerics and alarm *episodes*, not the waveform (D-42) — at
360 Hz the waveform is 30 MB an hour per bed and nobody has asked a question of it yet.
The database is optional and its failure degrades to "no history" (D-43): acquisition must
never wait on a slow disk, so session creation is fire-and-forget and the first second of
history may be missing. A missing measurement exports as a blank CSV field, never 0 and
never `null` — a spreadsheet reading 0 bpm where the monitor meant "I don't know" is the
display bug of 006 surviving into whatever anyone plots next.

---

## 015 — Acknowledge, verify against Postgres, the central station        Phase 4′

**Did:** Alarm acknowledgement with a severity-dependent re-alarm timer (D-44: a silence,
not a dismissal — 60 s high, 120 s medium, 300 s low). `server/public/central.html` —
multi-bed view sorted by worst active alarm, HR and SpO₂ trends that break across gaps,
inline ACK with a countdown, offline beds persisting for ten minutes (D-48). The whole
suite run against a real PostgreSQL.

**Result:** Storage contract 28/28 on both implementations; whole server suite green with
and without a database; a replayed patient's `LEADS_OFF` episode raised and cleared after
exactly 10.0 s; `acknowledged_by = central` persisted; three simultaneous beds with the
alarming one sorting to the top.

**Broke, #1 — total ordering.** `ORDER BY started_at DESC` returned sessions in different
orders on different runs when two started in the same millisecond. Tests that assumed an
empty store passed on a fresh database and failed on an accumulating one (D-45: tests
must not assume an empty store). Fixed with `started_at DESC, id DESC`.

**Broke, #2 — the watchdog crash.** `session.tick()` referenced `derived` from the ingest
path, which does not exist when the watchdog fires for a silent device. The one code
path that runs exactly when a patient has gone silent was the one that threw.

**Broke, #3 — the second asystole bug (D-47).** After ten seconds of leads-off plus three
of settling, "seconds since last beat" read thirteen, so ASYSTOLE fired the instant
signal quality returned — before the patient had any chance to produce a beat the system
was willing to look at. The mirror of 005 #1: not alarming because nothing was ever
measured, but because measuring was deliberately stopped. `RollingHeartRate.restart()`
moves the clock forward with the detector restart.

**Broke, #4 — speed versus wall clock (D-46).** At `--speed 15` a ten-second detached
electrode lasts 0.67 s of real time and a 1.5 s debounce correctly refuses to fire. The
demo showed no alarm and looked like a broken alarm engine. The simulator now warns.

**Interview:** "Why does a bed stay on screen after its device dies?" — because a tile
vanishing looks exactly like a patient being fine.

---

## 016 — Closing Phase 3.5's open items                                    Phase 3.5

**Did:** `firmware/src/vs_max30102.{h,c}` — portable C with the I²C transport injected,
23 host checks against a simulated part (register sequencing, part-ID rejection, 18-bit
decoding, FIFO wrap-around, overflow accounting), wired into `ppgTask`. The ECG/PPG
cross-check (D-41). The BIDMC residual re-examined (D-40).

**Result:** The "~1 % wrong by up to 25 bpm" residual was a *scoring artefact*: those
windows were scored against a bedside reference that itself moved 20 bpm inside the
comparison window while our ECG and PPG agreed with each other to 1.4 bpm. Excluding
windows where the reference is unstable — 28 of 160, reported — mean error 0.73 bpm and
100 % of accepted windows within ±5.

**Why this way:** The cross-check is asymmetric because the physiology is. Fewer pulses
than beats is *pulse deficit* — a real finding in AF and frequent ectopy, reported and
named. More pulses than beats is impossible and the pulse rate is withheld. A pulse near
exactly half the heart rate is sub-harmonic lock, withheld too.

**Still open:** The MAX30102 driver has never met a MAX30102. The calibration curve is
still an assumption. SR-06 stays partial.

---

## 017 — Fault injection against the running system                        Phase 5

**Did:** `sim/harness.py` (the real server as a subprocess, a v2 device with the AFE model
in the signal path, a dashboard recording arrival times), `sim/scenarios.py` (eight
scenarios), `sim/run_scenarios.py` writing `docs/scenario_results.json`.

**Result:** SR-02 latency median 0.5 ms, p95 2.1 ms (loopback); SR-03 leads-off 1.51 s;
SR-05 787/787 frames across a 30 s outage; SR-07 102/102 frames withheld on a swamped
signal.

**Broke — 102 wrong heart rates.** On its first run the matrix showed the *shipped* server
reporting 102 wrong heart rates on a swamped signal and withholding none: an outright
SR-07 violation. The signal-quality metric from 012 existed only in Python. The server's
own check was a coarse heuristic that never consulted it. Every component test had passed
throughout. A requirement is a property of the *system*; testing its parts is not the
same claim (D-49). Fixed by exposing QRS energy concentration from the streaming detector,
where the integrator already runs, and gating on it in `session.js`.

**Interview:** "Unit tests all passed. What did the integration test find?" — this.

---

## 018 — Generated requirements and CI                                    Phase 5

**Did:** `dsp/requirements.py` — the requirements as data, each naming the evidence that
decides its status — and `dsp/generate_requirements.py` rendering `docs/requirements.md`
from the model plus `validation_results.json` and `scenario_results.json`.
`.github/workflows/ci.yml`, six jobs.

**Why this way:** D-11 applied one level up (D-50). A hand-maintained status table drifts
silently and still looks authoritative. Status is *computed*: a requirement whose evidence
has never been produced renders as "not yet measured", and one whose evidence says it
failed cannot be rendered as passing because nothing in the generator can express that.

**Left open:** The `UNUSABLE_CONCENTRATION` threshold is defined twice (Python and JS) —
the one surviving hand-copied constant. CI was written and every job runs locally, but no
push had exercised GitHub Actions at the time of writing.

---

## 019 — Design diagrams and the hazard analysis                           Phase 6

**Did:** `docs/design.md` — seven Mermaid diagrams (context, work-split block diagram,
three sequences, the alarm state machine, the verification structure), each rendered
through `mermaid-cli` to check. `docs/safety.md` — IEC 60601-1 / -2-27 awareness, the
battery-and-isolation argument, an 11-row hazard analysis in the shape ISO 14971 asks for.

**Learned:** Of eleven hazards, the one this project is actually about is **H-2, a wrong
vital sign is believed**. Seven of the fifty decisions exist because of it, and every one
was forced by an observed failure (006, 015 #3, 017) rather than anticipated. That says
something about how this class of hazard gets found: by running the system and watching
it lie.

---

## 020 — Decisions, demo script, README                                    Phase 6

**Did:** `docs/decisions.md` — all fifty decisions moved out of PLAN §10 and grouped by
theme, each with the alternative rejected. `docs/demo.md` — a three-minute walkthrough.
`docs/interview.md` — questions mapped to evidence. Screenshots in `docs/img/`. A README
that shows the system rather than describing it.

**Broke:** The browser GIF recorder captures per action and is built for click-flows, not
a continuously scrolling waveform — two usable frames. No demo video; the script and
stills exist.

**Left open, across the whole project:** no hardware has been touched; the SpO₂
calibration curve is unvalidated; there is no authentication on the central station;
the WiFi term in the latency budget is named but unmeasured.

---

## 021 — Restoring the missing files                                      Maintenance

**Did (2026-08-21):** `server/public/central.html`, `firmware/test/host_device.c`,
`firmware/test/validate_frames.mjs` and this journal were absent from the tree although
the docs, the `validate:frames` npm script, the firmware Makefile and the bedside page's
"central station →" link all referred to them. `vs_calibration.h` was also missing — it is
generated, so it was regenerated with `dsp/export_coefficients.py` rather than written.

**Result:** firmware host tests 74/74; frame-contract check 26/26 (clean recording,
LO-pin leads-off, railed amplifier, 20 s outage lossless and sample-identical, 43 s
outage with 4,716 samples *reported* lost); server suite 102/102; `/central.html` serving
beds sorted by alarm, trends breaking across gaps, ACK with countdown, offline tiles
persisting.

**Broke:** Two of the new frame checks failed on first run, and both were the checks
rather than the firmware: the detector starts *asserted* by design (D-28, entry 009), and
the wire format puts `spo2`/`temp` at the top level rather than under `sensors`. The
contract check did its job in the other direction — it caught a wrong assumption about
the firmware before it became a wrong page.
