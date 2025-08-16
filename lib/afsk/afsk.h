#ifndef LIB_AFSK_H
#define LIB_AFSK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* ========= Error codes ========= */
typedef enum {
    AFSK_OK = 0,
    AFSK_ERR_BAD_ARG = -1,
    AFSK_ERR_BAD_STATE = -2
} afsk_status_t;

/* ========= Configuration =========
 * Mark/Space are in Hz; baud in symbols/sec; sample_rate in Hz.
 * amplitude in [-1.0, 1.0], typically <= 0.9 to avoid clipping.
 * samples_per_symbol is derived internally from (sample_rate / baud),
 * but you may override rounding for tighter timing if desired.
 */
typedef struct {
    double sample_rate;
    double baud;
    double f_mark;
    double f_space;
    double amplitude;           /* encoder amplitude */
    int    hard_decisions;      /* 1 => output bits; 0 => output soft metric too */
} afsk_config_t;

/* ========= Encoder ========= */
typedef struct {
    afsk_config_t cfg;
    /* internal */
    double phase;
    double phase_inc_mark;
    double phase_inc_space;
    double samples_per_symbol;
    double symbol_accum; /* for integer+fractional step */
    int    cur_is_mark;  /* current tone selection */
} afsk_encoder_t;

afsk_status_t afsk_encoder_init(afsk_encoder_t *enc, const afsk_config_t *cfg);

/* Generate samples for a buffer of bits (LSB first or MSB first is up to caller).
 * bits: pointer to bit array where each element is 0 or 1
 * nbits: number of bits
 * out: float buffer to receive PCM samples in [-1,1]
 * out_len: capacity of out buffer in samples
 * Returns number of samples written, or <0 on error.
 *
 * Notes:
 *  - Phase is continuous across calls.
 *  - One symbol per bit; if you have bytes, expand them to bits first.
 */
int afsk_encode_bits(afsk_encoder_t *enc, const uint8_t *bits, size_t nbits,
                     float *out, size_t out_len);

/* ========= Decoder (using Goertzel algorithm) ======== */
typedef struct {
    /* config copy */
    afsk_config_t cfg;

    /* windowing / timing */
    int    N;                /* samples per symbol (rounded) */
    double frac_carry;       /* to accumulate fractional samples per symbol */

    /* Goertzel coefficients for both tones */
    double coeff_mark;
    double coeff_space;

    /* Running state for a symbol window */
    double s1_mark, s2_mark;
    double s1_space, s2_space;
    int    idx_in_symbol;

    /* outputs */
    /* If hard_decisions==1: we fill bits_out with 0/1
       else: we fill bits_out and also soft_out with +metric (mark-space power) */
} afsk_decoder_t;

afsk_status_t afsk_decoder_init(afsk_decoder_t *dec, const afsk_config_t *cfg);

/* Feed PCM samples. PCM is expected in [-1,1].
 * For every complete symbol detected, we emit one bit (hard decision)
 * and optionally a soft metric (mark_power - space_power).
 *
 * soft_out can be NULL if you don't care.
 *
 * Returns number of *bits* emitted; <0 on error.
 */
int afsk_decode_pcm(afsk_decoder_t *dec, const float *pcm, size_t nsamples,
                    uint8_t *bits_out, double *soft_out, size_t max_bits);

/* Reset decoder symbol window (useful when re-syncing) */
void afsk_decoder_reset(afsk_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* LIB_AFSK_H */
