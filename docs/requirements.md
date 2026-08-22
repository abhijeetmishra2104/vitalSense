# Requirements and traceability

<!-- GENERATED FILE - produced by dsp/generate_requirements.py. Do not edit by hand. -->

Every requirement below is written to be **measurable** — a requirement that cannot fail a
test is not a requirement, it is a wish. Each names the design element that implements it
and the evidence that decides its status, and that status is *computed* from the evidence
rather than asserted here: a requirement whose evidence has never been produced says so,
and one whose evidence says it failed cannot be rendered as passing.

Evidence: MIT-BIH validation 2026-08-19 12:19 UTC · scenario matrix 2026-08-19 12:24 UTC.

Regenerate with:

```bash
python dsp/validate.py          # MIT-BIH results
python sim/run_scenarios.py     # fault-injection matrix (runs at real time)
python dsp/generate_requirements.py
```

## User needs

The needs the system exists to serve, before any engineering:

| ID | Need |
|---|---|
| UN-1 | A clinician can see a patient's heart rhythm and rate live, without touching the device |
| UN-2 | The system draws attention when a vital sign leaves its safe range |
| UN-3 | A displayed number is either trustworthy or visibly marked as untrustworthy — never confidently wrong |
| UN-4 | A brief network or electrode problem does not silently lose patient data |

## System requirements

| ID | Requirement | Serves | Design element | Verification | Status |
|---|---|---|---|---|---|
| SR-01 | Heart rate accurate to within ±5 bpm of reference | UN-1, UN-3 | Pan-Tompkins detection + median-of-instantaneous-rate (`dsp/vitals.py`, D-09) | Automated: `dsp/validate.py` against MIT-BIH cardiologist annotations | **verified** — worst 2.18 bpm, mean 0.34 bpm over 22,459 beats |
| SR-02 | End-to-end latency, electrode to dashboard, under 500 ms | UN-1 | WebSocket streaming with no batching; trailing-window streaming DSP | Fault-injection scenario: sample emitted by the device to vitals frame at a dashboard | **verified** — median 1.4 ms, p95 2.5 ms, worst 13.7 ms |
| SR-03 | Leads-off condition alarms within 2 s | UN-2, UN-3 | AD8232 LO± pins plus a rail cross-check → frame flag → alarm engine (D-28, D-18) | Fault-injection scenario: detach the electrode, time the alarm | **verified** — 1.51 s |
| SR-04 | QRS detection sensitivity > 99 % on clean records | UN-1, UN-3 | 5–15 Hz detection band, adaptive dual threshold, search-back, T-wave discrimination (D-07) | Automated: `dsp/validate.py`, ±150 ms match window per ANSI/AAMI EC57 | **verified** — 99.98 % over records 100, 101, 103, 115 |
| SR-05 | No data loss across a network outage of up to 30 s | UN-4 | Device-side ring buffer with backfill on reconnect (`firmware/src/vs_ringbuf.c`, D-27) | Fault-injection scenario: disconnect for 30 s, verify frame count and `seq` continuity | **verified** — 787/787 frames delivered, 0 gaps in seq |
| SR-06 | SpO₂ within ±3 % of reference | UN-1 | Ratio-of-ratios on raw red/IR, server-side (`dsp/ppg.py`, protocol v2, D-32) | Fault-injection scenario with a known synthetic saturation; the R→% calibration curve remains an assumption (D-34) | **verified** — 94.0 % against a true 94 % (error 0.0) |
| SR-07 | A signal too poor to trust is flagged, not reported as a number | UN-3 | QRS energy concentration gate with its threshold derived from SR-01 (`dsp/quality.py`, D-35) | Fault-injection scenario: swamp the signal, verify the rate is withheld rather than wrong | **verified** — 102/102 frames withheld a rate; 0 wrong rates reported |
| SR-08 | Alarm thresholds are defined once and applied identically everywhere | UN-2 | `RateLimits`/`DEFAULT_LIMITS` in `dsp/vitals.py`, generated into `coefficients.js` (D-12) | Boundary self-tests at each threshold, plus the generated-file drift check in CI | **verified** — `python dsp/vitals.py` (gated in CI) |

## Traceability

Which requirements serve each need — computed from the model, so a need with nothing
behind it appears as an empty row rather than being quietly absent.

| Need | Served by |
|---|---|
| UN-1 | SR-01, SR-02, SR-04, SR-06 |
| UN-2 | SR-03, SR-08 |
| UN-3 | SR-01, SR-03, SR-04, SR-07 |
| UN-4 | SR-05 |

## Standards awareness

This is a student project and makes **no claim of compliance**, but it is designed with
awareness of the standards a real device in this class must meet:

- **IEC 60601-1** — general safety for medical electrical equipment. Relevant here:
  battery operation, so the patient is never connected to mains-referenced circuitry.
- **IEC 60601-2-27** — particular requirements for ECG monitoring equipment. The source of
  the leads-off alarm requirement (SR-03), of the expectation that a monitor must not
  display a heart rate it cannot substantiate (SR-07), and of the ±5 mV input range the
  front-end gain was sized from (D-26).
- **ANSI/AAMI EC57** — the method used to evaluate beat detectors against annotated
  databases; the ±150 ms match window and the sensitivity/PPV reporting in
  `dsp/validate.py` follow it.
