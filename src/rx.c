#include "protocol.h"
#include "afsk/afsk.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <portaudio.h>
#include <unistd.h>
#include <math.h>

#define PCM_BUFFER_SIZE 256
#define MAX_PACKET_BYTES 1024
#define SYNC_RETRY_LOG 5000

// Decode PCM into a packet
int recv_packet(PaStream *stream, afsk_decoder_t *dec, pbcp_pkt_header_t *hdr, uint8_t **payload) {
    float pcm[PCM_BUFFER_SIZE];
    int nsamples = Pa_ReadStream(stream, pcm, PCM_BUFFER_SIZE);
    if (nsamples < 0) {
        fprintf(stderr, "[RX ERROR] Pa_ReadStream failed: %s\n", Pa_GetErrorText(nsamples));
        return -1;
    }
    if (nsamples == 0) return -1;

    double rms = 0;
    for (int i = 0; i < nsamples; i++) rms += pcm[i] * pcm[i];
    rms = sqrt(rms / nsamples);
    printf("[RX DEBUG] Read %d samples, RMS=%.3f\n", nsamples, rms);

    uint8_t bits[MAX_PACKET_BYTES * 8];
    int nbits = afsk_decode_pcm(dec, pcm, nsamples, bits, NULL, sizeof(bits));
    if (nbits <= 0) return -1;

    size_t nbytes = nbits / 8;
    if (nbytes < sizeof(pbcp_pkt_header_t)) return -1;

    uint8_t pkt_buf[MAX_PACKET_BYTES];
    for (size_t i = 0; i < nbytes; i++) {
        pkt_buf[i] = 0;
        for (int b = 0; b < 8; b++)
            pkt_buf[i] |= bits[i * 8 + b] << b;
    }

    memcpy(hdr, pkt_buf, sizeof(pbcp_pkt_header_t));
    if (hdr->length > 0) {
        *payload = realloc(*payload, hdr->length);
        memcpy(*payload, pkt_buf + sizeof(pbcp_pkt_header_t), hdr->length);
    }

    return 0;
}

// Encode a packet and send PCM
int send_packet(PaStream *stream, afsk_encoder_t *enc, const pbcp_pkt_header_t *hdr, const uint8_t *payload) {
    uint8_t pkt_buf[MAX_PACKET_BYTES];
    size_t pkt_len = sizeof(pbcp_pkt_header_t) + hdr->length;
    if (pkt_len > sizeof(pkt_buf)) return -1;

    memcpy(pkt_buf, hdr, sizeof(pbcp_pkt_header_t));
    if (payload && hdr->length) memcpy(pkt_buf + sizeof(pbcp_pkt_header_t), payload, hdr->length);

    uint8_t bits[MAX_PACKET_BYTES * 8];
    size_t nbits = 0;
    for (size_t i = 0; i < pkt_len; i++)
        for (int b = 0; b < 8; b++)
            bits[nbits++] = (pkt_buf[i] >> b) & 1;

    float pcm[PCM_BUFFER_SIZE];
    int nsamples = afsk_encode_bits(enc, bits, nbits, pcm, PCM_BUFFER_SIZE);
    Pa_WriteStream(stream, pcm, nsamples);
    return 0;
}

int main() {
    printf("[#] RX starting: sample_rate=%.2f, baud=%.1f\n", 44100.0, 1200.0);

    afsk_config_t cfg = {
        .sample_rate = 44100.0,
        .baud = 1200.0,
        .f_mark = 1200.0,
        .f_space = 2200.0,
        .amplitude = 0.5,
        .hard_decisions = 1
    };

    afsk_encoder_t enc;
    afsk_encoder_init(&enc, &cfg);
    afsk_decoder_t dec;
    afsk_decoder_init(&dec, &cfg);

    PaError err = Pa_Initialize();
    if (err != paNoError) { fprintf(stderr, "[ERROR] Pa_Initialize failed: %s\n", Pa_GetErrorText(err)); return 1; }

    int numDevices = Pa_GetDeviceCount();
    printf("[#] PortAudio devices:\n");
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        printf("[%d] %s (inputs=%d, outputs=%d)\n", i, info->name, info->maxInputChannels, info->maxOutputChannels);
    }

    int input_device = Pa_GetDefaultInputDevice();
    int output_device = Pa_GetDefaultOutputDevice();
    if (input_device == paNoDevice) { fprintf(stderr, "[ERROR] No default input device\n"); return 1; }
    if (output_device == paNoDevice) { fprintf(stderr, "[ERROR] No default output device\n"); return 1; }

    const PaDeviceInfo *in_info = Pa_GetDeviceInfo(input_device);
    const PaDeviceInfo *out_info = Pa_GetDeviceInfo(output_device);

    PaStreamParameters inputParams = {
        .device = input_device,
        .channelCount = 1,
        .sampleFormat = paFloat32,
        .suggestedLatency = in_info->defaultLowInputLatency,
        .hostApiSpecificStreamInfo = NULL
    };
    PaStreamParameters outputParams = {
        .device = output_device,
        .channelCount = 1,
        .sampleFormat = paFloat32,
        .suggestedLatency = out_info->defaultLowOutputLatency,
        .hostApiSpecificStreamInfo = NULL
    };

    PaStream *stream = NULL;
    err = Pa_OpenStream(&stream, &inputParams, &outputParams, 44100, PCM_BUFFER_SIZE, paClipOff, NULL, NULL);
    if (err != paNoError) { fprintf(stderr, "[ERROR] Pa_OpenStream failed: %s\n", Pa_GetErrorText(err)); return 1; }

    err = Pa_StartStream(stream);
    if (err != paNoError) { fprintf(stderr, "[ERROR] Pa_StartStream failed: %s\n", Pa_GetErrorText(err)); return 1; }

    printf("[#] Listening on device %d (%s)\n", input_device, in_info->name);

    pbcp_pkt_header_t hdr;
    uint8_t *payload = NULL;
    char msg_buf[8192] = {0};
    size_t msg_offset = 0;
    int retries = 0;

    // Wait for SYNC
    printf("[#] Waiting for SYNC packet...\n");
    while (1) {
        if (recv_packet(stream, &dec, &hdr, &payload) == 0) {
            printf("[RX] Received packet type 0x%02X\n", hdr.type);
            if (hdr.type == PBCP_TYPE_SYNC) {
                printf("[RX] SYNC received\n");
                break;
            }
        } else {
            usleep(500);
            if (++retries % SYNC_RETRY_LOG == 0) {
                printf("[RX] Still waiting for SYNC... resetting decoder\n");
                afsk_decoder_reset(&dec);
            }
            continue;
        }
        free(payload);
        payload = NULL;
    }

    // Send ACK
    pbcp_pkt_header_t ack_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_ACK, 0};
    send_packet(stream, &enc, &ack_hdr, NULL);
    printf("[RX] Sent ACK\n");

    // Send INFO
    pbcp_payload_info_t rxInfo = {0x12345678, 1, 0, 0};
    pbcp_pkt_header_t info_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_INFO, sizeof(rxInfo)};
    send_packet(stream, &enc, &info_hdr, (uint8_t*)&rxInfo);
    printf("[RX] Sent INFO\n");

    // Receive DATA/END
    while (1) {
        if (recv_packet(stream, &dec, &hdr, &payload) < 0) { usleep(500); continue; }

        if (hdr.type == PBCP_TYPE_DATA) {
            if (msg_offset + hdr.length < sizeof(msg_buf)) {
                memcpy(msg_buf + msg_offset, payload, hdr.length);
                msg_offset += hdr.length;
            }
            printf("[RX] Received DATA (%zu bytes): ", hdr.length);
            for (size_t i = 0; i < hdr.length; i++) printf("%02X ", payload[i]);
            printf("\n");
        } else if (hdr.type == PBCP_TYPE_END) {
            printf("[RX] Received END packet\n");
            pbcp_pkt_header_t final_ack = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_ACK, 0};
            send_packet(stream, &enc, &final_ack, NULL);
            printf("[RX] Sent final ACK\n");
            break;
        } else if (hdr.type == PBCP_TYPE_ERR) {
            pbcp_payload_err_t *err = (pbcp_payload_err_t*)payload;
            fprintf(stderr, "[RX] Received ERR code 0x%02X\n", err->code);
            break;
        }

        free(payload);
        payload = NULL;
    }

    printf("[RX] Full message received:\n%s\n", msg_buf);

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    free(payload);

    printf("[#] RX finished\n");
    return 0;
}
