#include "afsk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

static void write_wav_header(FILE *f, int sample_rate, int nsamples) {
    int bits_per_sample = 16;
    int num_channels = 1;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int data_chunk_size = nsamples * block_align;
    int fmt_chunk_size = 16;
    int riff_chunk_size = 4 + (8 + fmt_chunk_size) + (8 + data_chunk_size);

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    uint32_t val32 = riff_chunk_size;
    fwrite(&val32, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    val32 = fmt_chunk_size;
    fwrite(&val32, 4, 1, f);
    uint16_t val16 = 1; // PCM
    fwrite(&val16, 2, 1, f);
    val16 = num_channels;
    fwrite(&val16, 2, 1, f);
    val32 = sample_rate;
    fwrite(&val32, 4, 1, f);
    val32 = byte_rate;
    fwrite(&val32, 4, 1, f);
    val16 = block_align;
    fwrite(&val16, 2, 1, f);
    val16 = bits_per_sample;
    fwrite(&val16, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    val32 = data_chunk_size;
    fwrite(&val32, 4, 1, f);
}

int main(void) {
    /* Bell 202-ish defaults: 1200 baud, mark=1200 Hz, space=2200 Hz @ 48 kHz */
    afsk_config_t cfg = {
        .sample_rate = 48000.0,
        .baud = 1200.0,
        .f_mark = 1200.0,
        .f_space = 2200.0,
        .amplitude = 0.8,
        .hard_decisions = 0
    };

    afsk_encoder_t enc;
    afsk_decoder_t dec;
    afsk_encoder_init(&enc, &cfg);
    afsk_decoder_init(&dec, &cfg);

    /* Generate ~5 seconds worth of random bits */
    int target_seconds = 5;
    size_t nbits = (size_t)(cfg.baud * target_seconds);

    uint8_t *bits = malloc(nbits);
    srand((unsigned)time(NULL));
    for (size_t i = 0; i < nbits; i++) {
        bits[i] = rand() & 1;
    }

    /* Allocate PCM buffer */
    size_t approx_samples = (size_t)((cfg.sample_rate / cfg.baud) * nbits + 8);
    float *pcm = malloc(sizeof(float) * approx_samples);

    int nsamp = afsk_encode_bits(&enc, bits, nbits, pcm, approx_samples);
    if (nsamp <= 0) { fprintf(stderr, "encode failed\n"); return 1; }

    /* Write WAV file */
    FILE *fwav = fopen("afsk.wav", "wb");
    if (!fwav) { perror("fopen"); return 1; }
    write_wav_header(fwav, (int)cfg.sample_rate, nsamp);

    for (int i = 0; i < nsamp; i++) {
        float s = pcm[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t si = (int16_t)(s * 32767.0f);
        fwrite(&si, sizeof(int16_t), 1, fwav);
    }

    fclose(fwav);
    printf("Wrote %d samples (%.2f sec) to afsk.wav\n",
           nsamp, nsamp / cfg.sample_rate);

    /* Decode PCM back into bits */
    uint8_t *rx_bits = malloc(nbits);
    double  *soft = malloc(sizeof(double) * nbits);

    int got = afsk_decode_pcm(&dec, pcm, nsamp, rx_bits, soft, nbits);

    /* Compare original vs decoded */
    size_t errors = 0;
    size_t compared = (got < (int)nbits) ? got : nbits;
    for (size_t i = 0; i < compared; i++) {
        if (bits[i] != rx_bits[i]) {
            errors++;
        }
    }

    double ber = compared ? ((double)errors / (double)compared) : 0.0;

    printf("\n==== AFSK Loopback Test ====\n");
    printf("Config: %.0f baud, mark=%.0f Hz, space=%.0f Hz, Fs=%.0f Hz\n",
           cfg.baud, cfg.f_mark, cfg.f_space, cfg.sample_rate);
    printf("Generated bits : %zu\n", nbits);
    printf("Decoded bits   : %d\n", got);
    printf("Compared bits  : %zu\n", compared);
    printf("Bit errors     : %zu\n", errors);
    printf("Bit Error Rate : %.6f\n", ber);

    /* Print first 64 bits side-by-side for inspection */
    size_t show = (compared < 64) ? compared : 64;
    printf("\nFirst %zu bits:\n", show);
    printf("TX: ");
    for (size_t i = 0; i < show; i++) printf("%d", bits[i]);
    printf("\nRX: ");
    for (size_t i = 0; i < show; i++) printf("%d", rx_bits[i]);
    printf("\n");

    free(bits);
    free(pcm);
    free(rx_bits);
    free(soft);
    return 0;
}
