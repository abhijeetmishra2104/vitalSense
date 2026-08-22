/* Host-build unit tests for the portable firmware core.
 *
 * Run: make -C firmware/test test
 *
 * These exercise the same translation units the ESP32 image links, so a failure here
 * is a failure on the device. Output format matches the Python self-tests on purpose -
 * one project, one way of reporting a check.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "vs_calibration.h"
#include "vs_frame.h"
#include "vs_leadsoff.h"
#include "vs_max30102.h"
#include "vs_notch.h"
#include "vs_ringbuf.h"
#include "notch_coeffs.h"

/* -std=c99 is strict ISO C, under which glibc does not expose M_PI from <math.h>.
 * macOS libc does, which is why this only ever broke on the Linux CI runner. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int total = 0;
static int passed = 0;

static void check(const char *name, int ok, const char *fmt, ...)
{
    char detail[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    total++;
    if (ok) passed++;
    printf("%s  %-62s  %s\n", ok ? "PASS" : "FAIL", name, detail);
}

/* ---------------------------------------------------------------- notch --- */

static void test_notch(void)
{
    vs_notch_t f;

    /* A mains notch must pass DC: an ECG's baseline is not interference. */
    vs_notch_reset(&f);
    int32_t out = 0;
    for (int i = 0; i < 2000; ++i) out = vs_notch_process(&f, 2048);
    check("DC passes through the notch unchanged", out == 2048, "%d counts in, %d out", 2048, out);

    /* The reason rounding is done properly: a half-LSB bias per sample would be fed
     * back through the recursion and integrate into a visible baseline drift. */
    vs_notch_reset(&f);
    int32_t first = vs_notch_process(&f, 1000);
    int32_t last = 0;
    for (int i = 0; i < 200000; ++i) last = vs_notch_process(&f, 1000);
    check("no DC drift over 200k samples (9 minutes)", last == first && last == 1000,
          "first %d, last %d", first, last);

    /* Negative-going input exercises the other branch of the rounding. */
    vs_notch_reset(&f);
    for (int i = 0; i < 5000; ++i) out = vs_notch_process(&f, -1500);
    check("no drift on a negative constant either", out == -1500, "%d counts", out);

    /* Priming: a primed filter emits the steady state immediately, no charge-up. */
    vs_notch_reset(&f);
    vs_notch_prime(&f, 2048);
    int32_t primed_first = vs_notch_process(&f, 2048);
    check("a primed filter has no start-up transient", primed_first == 2048,
          "first output %d", primed_first);

    /* 50 Hz must be rejected; 10 Hz (inside the QRS band) must not be. */
    const double fs = VS_SAMPLE_RATE_HZ;
    double tone_out[2] = {0, 0};
    const double freqs[2] = {50.0, 10.0};
    for (int t = 0; t < 2; ++t) {
        vs_notch_reset(&f);
        vs_notch_prime(&f, 2048);
        double peak = 0.0;
        const int n = 4000;
        for (int i = 0; i < n; ++i) {
            const double s = 400.0 * sin(2.0 * M_PI * freqs[t] * i / fs);
            const int32_t y = vs_notch_process(&f, 2048 + (int32_t)lround(s));
            if (i > n / 2) {  /* skip the settling half */
                const double dev = fabs((double)y - 2048.0);
                if (dev > peak) peak = dev;
            }
        }
        tone_out[t] = peak;
    }
    check("50 Hz interference attenuated by > 25 dB",
          20.0 * log10(tone_out[0] / 400.0) < -25.0,
          "%.1f dB (400 -> %.1f counts)", 20.0 * log10(tone_out[0] / 400.0), tone_out[0]);
    check("10 Hz QRS-band content passes within 1 dB",
          fabs(20.0 * log10(tone_out[1] / 400.0)) < 1.0,
          "%.2f dB (400 -> %.1f counts)", 20.0 * log10(tone_out[1] / 400.0), tone_out[1]);

    /* Quantised coefficients can move the poles onto or outside the unit circle. The
     * generator checks this analytically; this checks the built filter empirically. */
    vs_notch_reset(&f);
    vs_notch_prime(&f, 0);
    (void)vs_notch_process(&f, 3000);
    double energy_early = 0.0, energy_late = 0.0;
    for (int i = 0; i < 8000; ++i) {
        const int32_t y = vs_notch_process(&f, 0);
        if (i < 500) energy_early += fabs((double)y);
        if (i >= 7000) energy_late += fabs((double)y);
    }
    check("impulse response decays (filter is stable as built)", energy_late < energy_early / 100.0,
          "early %.0f, late %.0f", energy_early, energy_late);

    /* Block and per-sample paths must agree exactly, or the frame size silently
     * changes the signal - the frame-size-independence property from BUILD_LOG 004. */
    int32_t a[97], b[97];
    for (int i = 0; i < 97; ++i) a[i] = b[i] = 2048 + (int32_t)lround(300.0 * sin(i * 0.37));
    vs_notch_t g;
    vs_notch_reset(&f);
    vs_notch_reset(&g);
    vs_notch_process_block(&f, a, 97);
    for (int i = 0; i < 97; ++i) b[i] = vs_notch_process(&g, b[i]);
    check("block processing equals sample-by-sample", memcmp(a, b, sizeof a) == 0, "97 samples identical");

    /* Splitting the same signal into different chunk sizes must not change it. */
    int32_t c1[240], c2[240];
    for (int i = 0; i < 240; ++i) c1[i] = c2[i] = 2048 + (int32_t)lround(500.0 * sin(i * 0.11));
    vs_notch_reset(&f);
    vs_notch_process_block(&f, c1, 240);
    vs_notch_reset(&g);
    for (int off = 0; off < 240; off += 32) {
        const uint32_t n = (240 - off < 32) ? (uint32_t)(240 - off) : 32u;
        vs_notch_process_block(&g, c2 + off, n);
    }
    check("output is independent of frame size", memcmp(c1, c2, sizeof c1) == 0,
          "one block of 240 == blocks of 32");

    /* Full-scale input must not overflow the accumulator into garbage. */
    vs_notch_reset(&f);
    int ok_range = 1;
    for (int i = 0; i < 2000; ++i) {
        const int32_t y = vs_notch_process(&f, (i % 2) ? 4095 : 0);
        if (y < -8192 || y > 12288) ok_range = 0;
    }
    check("full-scale square wave does not overflow", ok_range, "output stayed bounded");
}

/* -------------------------------------------------------------- ringbuf --- */

static void test_ringbuf(void)
{
    enum { CAP = 8 };
    int16_t storage[CAP];
    vs_ringbuf_t rb;

    check("init rejects zero capacity", !vs_ringbuf_init(&rb, storage, 0), "returned false");
    check("init rejects null storage", !vs_ringbuf_init(&rb, NULL, CAP), "returned false");
    check("init succeeds", vs_ringbuf_init(&rb, storage, CAP), "capacity %d", CAP);
    check("starts empty", vs_ringbuf_count(&rb) == 0 && vs_ringbuf_free(&rb) == CAP, "0 of %d", CAP);

    int16_t in[5] = {10, 20, 30, 40, 50};
    vs_ringbuf_push_block(&rb, in, 5);
    check("push_block stores every sample", vs_ringbuf_count(&rb) == 5, "%u held", vs_ringbuf_count(&rb));

    int16_t out[8] = {0};
    uint32_t got = vs_ringbuf_pop_block(&rb, out, 3);
    check("pop returns the oldest first (FIFO)",
          got == 3 && out[0] == 10 && out[1] == 20 && out[2] == 30,
          "got %u: %d %d %d", got, out[0], out[1], out[2]);
    check("popped samples are gone", vs_ringbuf_count(&rb) == 2, "%u left", vs_ringbuf_count(&rb));

    /* Peek must not consume - a frame handed to the network is not gone until sent. */
    got = vs_ringbuf_peek_block(&rb, out, 8);
    check("peek returns without consuming",
          got == 2 && out[0] == 40 && vs_ringbuf_count(&rb) == 2,
          "peeked %u, still holding %u", got, vs_ringbuf_count(&rb));
    vs_ringbuf_discard(&rb, 1);
    check("discard drops exactly the acknowledged samples", vs_ringbuf_count(&rb) == 1,
          "%u left", vs_ringbuf_count(&rb));

    /* Overflow: newest wins, and the loss is counted rather than silent. */
    vs_ringbuf_reset(&rb);
    int16_t many[12];
    for (int i = 0; i < 12; ++i) many[i] = (int16_t)(100 + i);
    const uint32_t lost = vs_ringbuf_push_block(&rb, many, 12);
    check("overflow drops the oldest, not the newest", lost == 4, "%u dropped", lost);
    check("overflow is counted, not silent", rb.dropped == 4, "dropped counter = %u", rb.dropped);
    got = vs_ringbuf_pop_block(&rb, out, 8);
    check("the samples kept are the most recent ones",
          got == 8 && out[0] == 104 && out[7] == 111,
          "held %d..%d", out[0], out[7]);

    got = vs_ringbuf_pop_block(&rb, out, 8);
    check("popping an empty buffer returns nothing", got == 0, "%u samples", got);

    /* SR-05 sizing: 30 s at 360 Hz is 10,800 samples = 21.6 kB as int16. */
    check("30 s of ECG at 360 Hz fits in 22 kB as int16",
          (30 * 360 * sizeof(int16_t)) <= 22000u,
          "%zu bytes for %d samples", 30 * 360 * sizeof(int16_t), 30 * 360);
}

/* ------------------------------------------------------------- leads-off --- */

static void test_leadsoff(void)
{
    const float fs = VS_SAMPLE_RATE_HZ;
    vs_leadsoff_t d;
    vs_leadsoff_init(&d, fs);

    /* Before any evidence, a monitor must not claim the patient is connected. */
    check("starts in the leads-off state", d.state == true, "state = %d", d.state);

    /* Clearing is slow on purpose: 500 ms of good signal before declaring connection. */
    int i = 0;
    while (d.state && i < 10000) { vs_leadsoff_update(&d, false, 2048); i++; }
    const double clear_ms = 1000.0 * i / fs;
    check("clears only after the de-assert window", fabs(clear_ms - VS_LEADSOFF_DEASSERT_MS) < 6.0,
          "%.1f ms (window %.0f ms)", clear_ms, (double)VS_LEADSOFF_DEASSERT_MS);

    /* Asserting is fast: SR-03 gives the whole system 2 s and the server takes 1.5 s. */
    i = 0;
    while (!d.state && i < 10000) { vs_leadsoff_update(&d, false, VS_ADC_FULL_SCALE); i++; }
    const double assert_ms = 1000.0 * i / fs;
    check("asserts within the assert window", fabs(assert_ms - VS_LEADSOFF_ASSERT_MS) < 6.0,
          "%.1f ms (window %.0f ms)", assert_ms, (double)VS_LEADSOFF_ASSERT_MS);
    check("device debounce leaves room inside SR-03's 2 s budget",
          VS_LEADSOFF_ASSERT_MS + 1500.0f < 2000.0f,
          "%.0f ms device + 1500 ms server = %.0f ms of 2000 ms",
          (double)VS_LEADSOFF_ASSERT_MS, (double)VS_LEADSOFF_ASSERT_MS + 1500.0);

    /* A single glitched sample must not toggle the reported state. */
    vs_leadsoff_init(&d, fs);
    for (i = 0; i < 1000; ++i) vs_leadsoff_update(&d, false, 2048);
    const bool before = d.state;
    vs_leadsoff_update(&d, false, VS_ADC_FULL_SCALE);
    vs_leadsoff_update(&d, false, 2048);
    check("one railed sample does not flip the state", d.state == before, "still %d", d.state);

    /* The LO pins alone are sufficient, even with a healthy-looking signal. */
    vs_leadsoff_init(&d, fs);
    for (i = 0; i < 1000; ++i) vs_leadsoff_update(&d, false, 2048);
    for (i = 0; i < 100; ++i) vs_leadsoff_update(&d, true, 2048);
    check("the LO pins assert leads-off on their own", d.state == true, "state = %d", d.state);

    check("rail detection covers both rails",
          vs_leadsoff_railed(0) && vs_leadsoff_railed(VS_ADC_FULL_SCALE) && !vs_leadsoff_railed(2048),
          "0 and %d railed, 2048 not", VS_ADC_FULL_SCALE);
}

/* ----------------------------------------------------------------- frame --- */

static void test_frame(void)
{
    char buf[64];

    check("zero formats without a sign", vs_format_mv(buf, sizeof buf, 0) > 0 && strcmp(buf, "0.0000") == 0,
          "\"%s\"", buf);
    vs_format_mv(buf, sizeof buf, 1234567);
    check("positive nanovolts render as millivolts", strcmp(buf, "1.2346") == 0, "\"%s\"", buf);
    vs_format_mv(buf, sizeof buf, -1234567);
    check("negative values keep their sign", strcmp(buf, "-1.2346") == 0, "\"%s\"", buf);
    vs_format_mv(buf, sizeof buf, -40);
    check("values below the last decimal round to zero without a stray sign",
          strcmp(buf, "0.0000") == 0, "\"%s\"", buf);
    /* -INT32_MIN overflows int32_t; the formatter must widen before negating. */
    check("INT32_MIN does not overflow the formatter",
          vs_format_mv(buf, sizeof buf, INT32_MIN) > 0 && buf[0] == '-', "\"%s\"", buf);

    check("counts convert to nanovolts via the generated calibration",
          vs_counts_to_nv(VS_ADC_ZERO_CODE) == 0 &&
              vs_counts_to_nv(VS_ADC_ZERO_CODE + 1000) == 1000 * VS_NV_PER_COUNT,
          "zero at code %d, %d nV per count", VS_ADC_ZERO_CODE, VS_NV_PER_COUNT);

    int32_t samples[8];
    for (int i = 0; i < 8; ++i) samples[i] = VS_ADC_ZERO_CODE + i * 100;
    vs_frame_input_t in = {
        .device_id = "esp32-01", .seq = 7, .t_device_ms = 1234, .fs = VS_SAMPLE_RATE_HZ,
        .ecg_counts = samples, .n_samples = 8, .leads_off = false,
        .sensors = { .spo2_valid = false, .temp_valid = true, .temp_milli = 36800,
                     .battery_valid = true, .battery_milli = 3940 },
    };
    char frame[1024];
    const size_t len = vs_frame_build(frame, sizeof frame, &in);
    check("a frame is built", len > 0 && len < sizeof frame, "%zu bytes", len);
    check("the frame declares protocol version 2", strstr(frame, "\"v\":2") != NULL, "found");
    check("a frame with no PPG sensor says so explicitly rather than omitting the field",
          strstr(frame, "\"ppg\":null") != NULL, "found");

    /* With a sensor attached, raw red and infrared counts travel in the frame and the
     * server derives saturation from them (protocol v2, D-03). */
    const uint32_t red[3] = { 79812u, 79790u, 79805u };
    const uint32_t ir[3] = { 99486u, 99451u, 99470u };
    const vs_ppg_block_t block = { red, ir, 3, 100.0f };
    in.ppg = &block;
    char pframe[2048];
    const size_t plen = vs_frame_build(pframe, sizeof pframe, &in);
    check("a frame carries raw PPG when the sensor has samples", plen > 0, "%zu bytes", plen);
    check("the PPG block names its own sample rate",
          strstr(pframe, "\"ppg\":{\"fs\":100") != NULL, "found");
    check("red and infrared travel as separate equal-length arrays",
          strstr(pframe, "\"red\":[79812,79790,79805]") != NULL &&
              strstr(pframe, "\"ir\":[99486,99451,99470]") != NULL,
          "both arrays present");
    check("the PPG capacity estimate covers the real frame",
          vs_frame_capacity_with_ppg(8, strlen("esp32-01"), 3) > plen,
          "%zu estimated vs %zu actual", vs_frame_capacity_with_ppg(8, strlen("esp32-01"), 3), plen);
    in.ppg = NULL;
    check("an absent SpO2 reading is null, not omitted and not stale",
          strstr(frame, "\"spo2\":null") != NULL, "found");
    check("a present reading carries its value", strstr(frame, "\"temp\":36.800") != NULL, "found");
    check("leadsOff is always present", strstr(frame, "\"leadsOff\":false") != NULL, "found");
    check("fs renders as an integer the server can match exactly",
          strstr(frame, "\"fs\":360") != NULL, "found");

    /* A truncated medical data frame is worse than no frame at all. */
    const size_t needed = vs_frame_capacity(8, strlen("esp32-01"));
    check("capacity estimate covers the real frame", needed > len, "%zu estimated vs %zu actual", needed, len);
    check("a buffer that is too small yields nothing, not a partial frame",
          vs_frame_build(frame, len - 1, &in) == 0, "refused");
    check("null inputs are refused", vs_frame_build(NULL, 100, &in) == 0 &&
          vs_frame_build(frame, sizeof frame, NULL) == 0, "refused");
    in.n_samples = 0;
    check("a frame with no samples is refused", vs_frame_build(frame, sizeof frame, &in) == 0, "refused");
}

/* ------------------------------------------------------------- max30102 --- */

/* A simulated part on the bus: register file plus a FIFO the test can load. */
typedef struct {
    uint8_t regs[256];
    uint8_t fifo[VS_MAX30102_FIFO_DEPTH][6];
    uint32_t fifo_read_index;
    int fail_after;   /* -1 = never fail; otherwise fail once this many ops have run */
    int ops;
    uint8_t writes[64];
    uint8_t write_regs[64];
    int n_writes;
} fake_part_t;

static bool fake_read(void *ctx, uint8_t reg, uint8_t *dst, uint32_t len)
{
    fake_part_t *f = (fake_part_t *)ctx;
    if (f->fail_after >= 0 && f->ops++ >= f->fail_after) return false;
    if (reg == 0x07) { /* FIFO_DATA */
        for (uint32_t i = 0; i < len && i < 6; ++i) {
            dst[i] = f->fifo[f->fifo_read_index % VS_MAX30102_FIFO_DEPTH][i];
        }
        f->fifo_read_index++;
        return true;
    }
    for (uint32_t i = 0; i < len; ++i) dst[i] = f->regs[(reg + i) & 0xFF];
    return true;
}

static bool fake_write(void *ctx, uint8_t reg, uint8_t value)
{
    fake_part_t *f = (fake_part_t *)ctx;
    if (f->fail_after >= 0 && f->ops++ >= f->fail_after) return false;
    f->regs[reg] = value;
    if (f->n_writes < 64) {
        f->write_regs[f->n_writes] = reg;
        f->writes[f->n_writes] = value;
        f->n_writes++;
    }
    return true;
}

static fake_part_t make_part(void)
{
    fake_part_t f;
    memset(&f, 0, sizeof f);
    f.regs[0xFF] = VS_MAX30102_PART_ID;
    f.fail_after = -1;
    return f;
}

static bool wrote(const fake_part_t *f, uint8_t reg, uint8_t value)
{
    for (int i = 0; i < f->n_writes; ++i) {
        if (f->write_regs[i] == reg && f->writes[i] == value) return true;
    }
    return false;
}

static void test_max30102(void)
{
    fake_part_t part = make_part();
    vs_i2c_t bus = { fake_read, fake_write, &part };
    vs_max30102_t dev;

    check("init succeeds against a correct part", vs_max30102_init(&dev, bus), "part 0x15");
    check("init leaves the driver ready", vs_max30102_ready(&dev), "ready");
    check("init resets the part before configuring it", wrote(&part, 0x09, 0x40), "MODE_CONFIG = 0x40");
    check("init clears both FIFO pointers and the overflow counter",
          wrote(&part, 0x04, 0) && wrote(&part, 0x05, 0) && wrote(&part, 0x06, 0), "all zeroed");
    check("init selects SpO2 mode last, after configuration", part.write_regs[part.n_writes - 1] == 0x09 &&
          part.writes[part.n_writes - 1] == 0x03, "MODE_CONFIG = 0x03");
    check("init requests 100 Hz, the rate the filters are designed for",
          ((part.regs[0x0A] >> 2) & 0x07) == 0x03, "SPO2_CONFIG sample-rate field = 011");
    check("init requests 18-bit resolution", (part.regs[0x0A] & 0x03) == 0x03, "pulse width = 411 us");
    check("both LEDs are driven", part.regs[0x0C] > 0 && part.regs[0x0D] > 0,
          "red 0x%02X, ir 0x%02X", part.regs[0x0C], part.regs[0x0D]);

    /* An unknown part must be refused: its register map is a guess. */
    fake_part_t wrong = make_part();
    wrong.regs[0xFF] = 0x11;
    vs_i2c_t wrong_bus = { fake_read, fake_write, &wrong };
    vs_max30102_t wrong_dev;
    check("a part that identifies as something else is refused",
          !vs_max30102_init(&wrong_dev, wrong_bus) && !vs_max30102_ready(&wrong_dev), "init returned false");

    /* A bus that fails must not leave the driver claiming to be ready. */
    fake_part_t dead = make_part();
    dead.fail_after = 0;
    vs_i2c_t dead_bus = { fake_read, fake_write, &dead };
    vs_max30102_t dead_dev;
    check("a failing bus is not mistaken for a working sensor",
          !vs_max30102_init(&dead_dev, dead_bus) && !vs_max30102_ready(&dead_dev), "init returned false");
    check("null callbacks are refused", !vs_max30102_init(&dead_dev, (vs_i2c_t){ NULL, NULL, NULL }), "refused");

    /* 18-bit decoding: the top six bits of the 24 are undefined and must be masked. */
    const uint8_t clean[3] = { 0x01, 0x02, 0x03 };
    check("three bytes decode as an 18-bit value", vs_max30102_decode(clean) == 0x010203u,
          "0x%06X", vs_max30102_decode(clean));
    const uint8_t noisy[3] = { 0xFC, 0x00, 0x00 };
    check("undefined high bits are masked off, not added to the reading",
          vs_max30102_decode(noisy) == 0, "0x%06X", vs_max30102_decode(noisy));
    const uint8_t full[3] = { 0xFF, 0xFF, 0xFF };
    check("full scale is 2^18 - 1", vs_max30102_decode(full) == 0x3FFFFu, "%u", vs_max30102_decode(full));

    /* Reading the FIFO. */
    vs_ppg_sample_t out[VS_MAX30102_FIFO_DEPTH];
    part = make_part();
    bus.ctx = &part;
    vs_max30102_init(&dev, bus);
    part.regs[0x04] = 0; /* wr */
    part.regs[0x06] = 0; /* rd */
    check("an empty FIFO yields no samples", vs_max30102_read(&dev, out, 32) == 0, "0 samples");

    part.regs[0x04] = 5;
    part.regs[0x06] = 0;
    for (int i = 0; i < 5; ++i) {
        part.fifo[i][0] = 0x01; part.fifo[i][1] = 0x00; part.fifo[i][2] = (uint8_t)i;      /* red */
        part.fifo[i][3] = 0x02; part.fifo[i][4] = 0x00; part.fifo[i][5] = (uint8_t)(i + 1); /* ir  */
    }
    part.fifo_read_index = 0;
    uint32_t got = vs_max30102_read(&dev, out, 32);
    check("five queued samples are read", got == 5, "%u samples", got);
    check("red and infrared are decoded from the right halves of each sample",
          out[0].red == 0x010000u && out[0].ir == 0x020001u,
          "red 0x%06X, ir 0x%06X", out[0].red, out[0].ir);
    check("samples come out in order", out[4].red == 0x010004u, "0x%06X", out[4].red);

    /* Wrap-around: a write pointer behind the read pointer means the FIFO wrapped,
     * not that it is empty. */
    part.regs[0x04] = 2;
    part.regs[0x06] = 30;
    part.fifo_read_index = 0;
    got = vs_max30102_read(&dev, out, 32);
    check("a wrapped FIFO reports the right count, not zero", got == 4, "%u samples", got);

    /* Overflow is counted rather than hidden. */
    part.regs[0x04] = 8;
    part.regs[0x06] = 0;
    part.regs[0x05] = 7; /* overflow counter */
    part.fifo_read_index = 0;
    const uint32_t before = dev.overflow_total;
    vs_max30102_read(&dev, out, 32);
    check("dropped samples are counted, not silently lost", dev.overflow_total == before + 7,
          "overflow total %u", dev.overflow_total);

    /* The caller's buffer bounds the read. */
    part.regs[0x04] = 20;
    part.regs[0x06] = 0;
    part.regs[0x05] = 0;
    part.fifo_read_index = 0;
    got = vs_max30102_read(&dev, out, 4);
    check("the read never exceeds the caller's buffer", got == 4, "%u samples", got);

    check("reading from an uninitialised driver yields nothing",
          vs_max30102_read(&wrong_dev, out, 32) == 0, "0 samples");
}

int main(void)
{
    printf("\nfirmware host test\n");
    printf("--------------------------------------------------------------------------------\n");
    test_notch();
    test_ringbuf();
    test_leadsoff();
    test_frame();
    test_max30102();
    printf("--------------------------------------------------------------------------------\n");
    printf("%d/%d checks passed\n\n", passed, total);
    return (passed == total) ? 0 : 1;
}
