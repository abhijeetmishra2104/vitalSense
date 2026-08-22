/* Host-side device emulator.
 *
 * Runs the portable firmware core - fixed-point notch, leads-off detector,
 * store-and-forward ring buffer, frame builder - on a laptop and writes the frames it
 * would have sent over WebSocket to stdout, one JSON object per line. Nothing here is
 * a re-implementation: the translation units linked are the ones the ESP32 image links
 * (D-25). `validate_frames.mjs` feeds this output into the server's own
 * validateSampleFrame(), so "the firmware speaks the protocol" is measured rather than
 * assumed.
 *
 * Input: one ADC count per line on stdin (12-bit codes, 0..4095; blank lines and lines
 * starting with '#' are ignored). Typically produced by sim/afe_model.py from a MIT-BIH
 * record, or synthesised by the caller.
 *
 * Options:
 *   --device-id ID          device identifier (default host-01)
 *   --frame-samples N       samples per frame (default 36 = 100 ms at 360 Hz)
 *   --lo-from S --lo-to S   assert the AD8232 LO pins for samples [S, S)
 *   --outage-from S --outage-to S
 *                           link down for samples [S, S): frames are held in the ring
 *                           buffer and drained once the link returns (SR-05)
 *   --buffer-seconds T      ring buffer depth (default 30, as on the device)
 *   --no-prime              start the notch from zero instead of priming on the first sample
 *
 * Exit status is 0 when every sample was either sent or accounted for as dropped; the
 * summary on stderr reports frames sent, samples buffered through the outage, and
 * samples lost to overflow - the loss is reported, never silent (D-27).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notch_coeffs.h"
#include "vs_calibration.h"
#include "vs_frame.h"
#include "vs_leadsoff.h"
#include "vs_notch.h"
#include "vs_ringbuf.h"

#define MAX_FRAME_SAMPLES 2048u

static const char *device_id = "host-01";
static uint32_t frame_samples = 36;
static uint32_t lo_from = 0, lo_to = 0;
static uint32_t outage_from = 0, outage_to = 0;
static uint32_t buffer_seconds = 30;
static bool prime = true;

static uint32_t seq = 0;
static uint32_t frames_sent = 0;
static uint32_t samples_sent = 0;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--device-id ID] [--frame-samples N] [--lo-from S --lo-to S]\n"
            "          [--outage-from S --outage-to S] [--buffer-seconds T] [--no-prime] < counts.txt\n",
            argv0);
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--no-prime") == 0) {
            prime = false;
        } else if (strcmp(a, "--device-id") == 0 && v) {
            device_id = v;
            ++i;
        } else if (strcmp(a, "--frame-samples") == 0 && v) {
            if (!parse_u32(v, &frame_samples)) return false;
            ++i;
        } else if (strcmp(a, "--lo-from") == 0 && v) {
            if (!parse_u32(v, &lo_from)) return false;
            ++i;
        } else if (strcmp(a, "--lo-to") == 0 && v) {
            if (!parse_u32(v, &lo_to)) return false;
            ++i;
        } else if (strcmp(a, "--outage-from") == 0 && v) {
            if (!parse_u32(v, &outage_from)) return false;
            ++i;
        } else if (strcmp(a, "--outage-to") == 0 && v) {
            if (!parse_u32(v, &outage_to)) return false;
            ++i;
        } else if (strcmp(a, "--buffer-seconds") == 0 && v) {
            if (!parse_u32(v, &buffer_seconds)) return false;
            ++i;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            return false;
        }
    }
    if (frame_samples == 0 || frame_samples > MAX_FRAME_SAMPLES) {
        fprintf(stderr, "--frame-samples must be 1..%u\n", MAX_FRAME_SAMPLES);
        return false;
    }
    return true;
}

/* Emit one frame of `n` samples popped from the ring buffer. `first_index` is the
 * absolute sample index of the frame's first sample, used for the device timestamp. */
static bool emit_frame(vs_ringbuf_t *rb, uint32_t n, uint32_t first_index, bool leads_off, char *buf, size_t cap)
{
    int16_t block[MAX_FRAME_SAMPLES];
    int32_t counts[MAX_FRAME_SAMPLES];
    const uint32_t got = vs_ringbuf_pop_block(rb, block, n);
    if (got == 0) return true;
    for (uint32_t i = 0; i < got; ++i) counts[i] = block[i];

    const vs_frame_input_t in = {
        .device_id = device_id,
        .seq = seq,
        .t_device_ms = (uint32_t)((uint64_t)first_index * 1000u / (uint32_t)VS_SAMPLE_RATE_HZ),
        .fs = VS_SAMPLE_RATE_HZ,
        .ecg_counts = counts,
        .n_samples = got,
        .leads_off = leads_off,
        /* No PPG or temperature sensor on the host: those readings are absent and the
         * frame says so with null rather than a plausible constant (D-16). */
        .sensors = { .spo2_valid = false, .temp_valid = false, .battery_valid = true, .battery_milli = 3940 },
        .ppg = NULL,
    };
    const size_t len = vs_frame_build(buf, cap, &in);
    if (len == 0) {
        fprintf(stderr, "frame %lu: buffer too small (%zu bytes)\n", (unsigned long)seq, cap);
        return false;
    }
    fwrite(buf, 1, len, stdout);
    fputc('\n', stdout);
    seq += 1;
    frames_sent += 1;
    samples_sent += got;
    return true;
}

int main(int argc, char **argv)
{
    if (!parse_args(argc, argv)) {
        usage(argv[0]);
        return 2;
    }

    const uint32_t capacity = buffer_seconds * (uint32_t)VS_SAMPLE_RATE_HZ;
    int16_t *storage = malloc(sizeof(int16_t) * capacity);
    if (!storage) {
        fprintf(stderr, "cannot allocate %u-sample buffer\n", capacity);
        return 2;
    }
    vs_ringbuf_t rb;
    vs_ringbuf_init(&rb, storage, capacity);

    const size_t cap = vs_frame_capacity(frame_samples, strlen(device_id));
    char *frame = malloc(cap);
    if (!frame) {
        fprintf(stderr, "cannot allocate frame buffer\n");
        free(storage);
        return 2;
    }

    vs_notch_t notch;
    vs_notch_reset(&notch);
    vs_leadsoff_t lo;
    vs_leadsoff_init(&lo, VS_SAMPLE_RATE_HZ);

    uint32_t index = 0;          /* absolute sample index */
    uint32_t frame_start = 0;    /* index of the first sample in the pending frame */
    uint32_t in_frame = 0;       /* samples accumulated towards the next frame */
    bool frame_leads_off = false;
    bool link_was_down = false;
    uint32_t buffered_through_outage = 0;
    bool ok = true;
    char line[64];

    while (ok && fgets(line, sizeof line, stdin)) {
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\r') continue;
        char *end = NULL;
        long value = strtol(line, &end, 10);
        if (end == line) continue;
        if (value < 0) value = 0;
        if (value > VS_ADC_FULL_SCALE) value = VS_ADC_FULL_SCALE;

        if (prime && !notch.primed) vs_notch_prime(&notch, (int32_t)value);
        const int32_t filtered = vs_notch_process(&notch, (int32_t)value);
        const bool lo_pins = index >= lo_from && index < lo_to;
        const bool leads_off = vs_leadsoff_update(&lo, lo_pins, (int32_t)value);

        /* The acquisition task never stops: samples go into the buffer whether or not
         * the link is up. That is the whole of SR-05. */
        if (vs_ringbuf_full(&rb)) { /* overwrite-oldest is counted inside the ring buffer */ }
        vs_ringbuf_push(&rb, (int16_t)filtered);
        if (in_frame == 0) frame_start = index;
        in_frame += 1;
        /* A frame is marked leads-off if the electrodes were off for any of it. */
        frame_leads_off = frame_leads_off || leads_off;

        const bool link_down = index >= outage_from && index < outage_to;
        if (link_down) {
            buffered_through_outage += 1;
            link_was_down = true;
        }

        if (in_frame == frame_samples) {
            if (!link_down) {
                /* Drain everything the buffer holds, oldest first - during an outage
                 * that is several frames; normally exactly one. */
                uint32_t backlog_start = frame_start;
                if (link_was_down) {
                    /* Frames held during the outage start earlier than this one. */
                    backlog_start = index + 1 - vs_ringbuf_count(&rb);
                    link_was_down = false;
                }
                while (vs_ringbuf_count(&rb) >= frame_samples) {
                    ok = emit_frame(&rb, frame_samples, backlog_start, frame_leads_off, frame, cap);
                    if (!ok) break;
                    backlog_start += frame_samples;
                }
            }
            in_frame = 0;
            frame_leads_off = false;
        }
        index += 1;
    }

    /* Flush a final partial frame so the tail of the recording is not lost. */
    if (ok && vs_ringbuf_count(&rb) > 0 && !(index >= outage_from && index < outage_to)) {
        const uint32_t remaining = vs_ringbuf_count(&rb);
        uint32_t start = index - remaining;
        while (ok && vs_ringbuf_count(&rb) > 0) {
            const uint32_t n = vs_ringbuf_count(&rb) < frame_samples ? vs_ringbuf_count(&rb) : frame_samples;
            ok = emit_frame(&rb, n, start, frame_leads_off, frame, cap);
            start += n;
        }
    }
    fflush(stdout);

    fprintf(stderr,
            "host_device: %u samples in, %u frames sent (%u samples), %u buffered through outage, %u dropped\n",
            index, frames_sent, samples_sent, buffered_through_outage, rb.dropped);

    free(frame);
    free(storage);
    if (!ok) return 1;
    /* Every sample must be either sent or reported dropped. Anything else is a silent loss. */
    return (samples_sent + rb.dropped + vs_ringbuf_count(&rb) == index) ? 0 : 1;
}
