#include "afsk.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======= Helpers ======= */
static double clamp(double v, double lo, double hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* Compute Goertzel coefficient for target frequency ft over N samples @ Fs:
 * coeff = 2*cos(2*pi*ft/Fs)
 */
static double goertzel_coeff(double ft, double Fs) {
    return 2.0 * cos(2.0 * M_PI * ft / Fs);
}

/* Power from Goertzel state after N samples:
 * P = s1^2 + s2^2 - coeff*s1*s2
 */
static double goertzel_power(double s1, double s2, double coeff) {
    return s1*s1 + s2*s2 - coeff*s1*s2;
}

/* ======= Encoder ======= */
afsk_status_t afsk_encoder_init(afsk_encoder_t *enc, const afsk_config_t *cfg) {
    if (!enc || !cfg) return AFSK_ERR_BAD_ARG;
    if (cfg->sample_rate <= 0 || cfg->baud <= 0 || cfg->f_mark <= 0 || cfg->f_space <= 0)
        return AFSK_ERR_BAD_ARG;

    memset(enc, 0, sizeof(*enc));
    enc->cfg = *cfg;
    enc->cfg.amplitude = clamp(enc->cfg.amplitude, -1.0, 1.0);

    enc->phase = 0.0;
    enc->phase_inc_mark  = 2.0 * M_PI * cfg->f_mark  / cfg->sample_rate;
    enc->phase_inc_space = 2.0 * M_PI * cfg->f_space / cfg->sample_rate;

    enc->samples_per_symbol = cfg->sample_rate / cfg->baud;
    enc->symbol_accum = 0.0;
    enc->cur_is_mark = 1;
    return AFSK_OK;
}

int afsk_encode_bits(afsk_encoder_t *enc, const uint8_t *bits, size_t nbits,
                     float *out, size_t out_len)
{
    if (!enc || !bits || !out) return AFSK_ERR_BAD_ARG;
    if (nbits == 0 || out_len == 0) return 0;

    size_t produced = 0;

    /* Emit symbol-by-symbol with fractional steps:
     * per bit: round(samples_per_symbol + carried_fraction).
     */
    for (size_t i = 0; i < nbits; ++i) {
        enc->cur_is_mark = bits[i] ? 1 : 0;

        /* Determine how many samples to write for this symbol. */
        double exact = enc->samples_per_symbol + enc->symbol_accum;
        int nS = (int)floor(exact + 0.5); /* nearest */
        enc->symbol_accum = exact - nS;

        for (int n = 0; n < nS; ++n) {
            if (produced >= out_len) return (int)produced;
            double inc = enc->cur_is_mark ? enc->phase_inc_mark : enc->phase_inc_space;
            double s = sin(enc->phase);
            out[produced++] = (float)(enc->cfg.amplitude * s);
            enc->phase += inc;
            /* keep phase bounded */
            if (enc->phase > 1e6) enc->phase = fmod(enc->phase, 2.0*M_PI);
        }
    }

    return (int)produced;
}

/* ======= Decoder ======= */
afsk_status_t afsk_decoder_init(afsk_decoder_t *dec, const afsk_config_t *cfg) {
    if (!dec || !cfg) return AFSK_ERR_BAD_ARG;
    if (cfg->sample_rate <= 0 || cfg->baud <= 0 || cfg->f_mark <= 0 || cfg->f_space <= 0)
        return AFSK_ERR_BAD_ARG;

    memset(dec, 0, sizeof(*dec));
    dec->cfg = *cfg;

    /* Choose integer samples per symbol, track fractional leftover for drift handling */
    double exact_N = cfg->sample_rate / cfg->baud;
    dec->N = (int)floor(exact_N + 0.5);
    if (dec->N < 4) dec->N = 4; /* sanity */

    dec->frac_carry = exact_N - dec->N;

    dec->coeff_mark  = goertzel_coeff(cfg->f_mark,  cfg->sample_rate);
    dec->coeff_space = goertzel_coeff(cfg->f_space, cfg->sample_rate);

    dec->s1_mark = dec->s2_mark = 0.0;
    dec->s1_space = dec->s2_space = 0.0;
    dec->idx_in_symbol = 0;

    return AFSK_OK;
}

void afsk_decoder_reset(afsk_decoder_t *dec) {
    if (!dec) return;
    dec->s1_mark = dec->s2_mark = 0.0;
    dec->s1_space = dec->s2_space = 0.0;
    dec->idx_in_symbol = 0;
}

/* Process nsamples of PCM; emit up to max_bits decisions. */
int afsk_decode_pcm(afsk_decoder_t *dec, const float *pcm, size_t nsamples,
                    uint8_t *bits_out, double *soft_out, size_t max_bits)
{
    if (!dec || !pcm || !bits_out) return AFSK_ERR_BAD_ARG;
    if (nsamples == 0 || max_bits == 0) return 0;

    size_t out_bits = 0;

    /* Symbol length may wobble by +/-1 sample as we accumulate frac_carry. */
    int N_target = dec->N;
    double frac = dec->frac_carry;

    for (size_t i = 0; i < nsamples; ++i) {
        /* One step of Goertzel for mark */
        double x = pcm[i];
        double s_mark = x + dec->coeff_mark * dec->s1_mark - dec->s2_mark;
        dec->s2_mark = dec->s1_mark;
        dec->s1_mark = s_mark;

        /* One step for space */
        double s_space = x + dec->coeff_space * dec->s1_space - dec->s2_space;
        dec->s2_space = dec->s1_space;
        dec->s1_space = s_space;

        dec->idx_in_symbol++;

        if (dec->idx_in_symbol >= N_target) {
            /* End of symbol: compute power and decide */
            double p_mark  = goertzel_power(dec->s1_mark,  dec->s2_mark,  dec->coeff_mark);
            double p_space = goertzel_power(dec->s1_space, dec->s2_space, dec->coeff_space);
            double metric = p_mark - p_space; /* >0 -> mark, <0 -> space */

            if (out_bits < max_bits) {
                bits_out[out_bits] = (metric >= 0.0) ? 1 : 0;
                if (!dec->cfg.hard_decisions && soft_out) {
                    soft_out[out_bits] = metric;
                }
                out_bits++;
            }

            /* Prepare next symbol window */
            dec->s1_mark = dec->s2_mark = 0.0;
            dec->s1_space = dec->s2_space = 0.0;
            dec->idx_in_symbol = 0;

            /* Adjust next N_target with fractional carry to track drift */
            double exact_N = dec->cfg.sample_rate / dec->cfg.baud;
            frac += (exact_N - (double)dec->N);
            N_target = dec->N + (frac >= 0.5 ? 1 : (frac <= -0.5 ? -1 : 0));
            if (frac >= 0.5) frac -= 1.0;
            if (frac <= -0.5) frac += 1.0;
            if (N_target < 4) N_target = 4;
        }
    }

    /* Save back */
    dec->frac_carry = frac;
    return (int)out_bits;
}
