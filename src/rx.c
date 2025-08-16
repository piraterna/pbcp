#include "protocol.h"
#include "afsk/afsk.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <portaudio.h>
#include <unistd.h>

#define PCM_BUFFER_SIZE 2048
#define MAX_PACKET_BYTES 1024

// ---------------- RX Helpers ----------------
int recv_packet(PaStream *stream, afsk_decoder_t *dec, pbcp_pkt_header_t *hdr, uint8_t **payload) {
    float pcm[PCM_BUFFER_SIZE];
    int nsamples = Pa_ReadStream(stream, pcm, PCM_BUFFER_SIZE);
    if(nsamples <= 0) return -1;

    // Debug: first few PCM samples
    printf("[RX DEBUG] PCM[0..7]: ");
    for(int i=0;i<8 && i<nsamples;i++) printf("%.3f ", pcm[i]);
    printf("\n");

    uint8_t bits[MAX_PACKET_BYTES*8];
    int nbits = afsk_decode_pcm(dec, pcm, nsamples, bits, NULL, sizeof(bits));
    if(nbits <= 0) return -1;

    // Debug: first few decoded bits
    printf("[RX DEBUG] Bits[0..15]: ");
    for(int i=0;i<16 && i<nbits;i++) printf("%d", bits[i]);
    printf("\n");

    size_t nbytes = nbits / 8;
    if(nbytes < sizeof(pbcp_pkt_header_t)) return -1;

    uint8_t pkt_buf[MAX_PACKET_BYTES];
    for(size_t i=0;i<nbytes;i++){
        pkt_buf[i] = 0;
        for(int b=0;b<8;b++)
            pkt_buf[i] |= bits[i*8+b] << b;
    }

    memcpy(hdr, pkt_buf, sizeof(pbcp_pkt_header_t));

    if(hdr->length > 0) {
        *payload = realloc(*payload, hdr->length);
        memcpy(*payload, pkt_buf + sizeof(pbcp_pkt_header_t), hdr->length);
    }

    return 0;
}

int send_packet(PaStream *stream, afsk_encoder_t *enc, const pbcp_pkt_header_t *hdr, const uint8_t *payload) {
    uint8_t pkt_buf[MAX_PACKET_BYTES];
    size_t pkt_len = sizeof(pbcp_pkt_header_t) + hdr->length;
    if(pkt_len > sizeof(pkt_buf)) return -1;

    memcpy(pkt_buf, hdr, sizeof(pbcp_pkt_header_t));
    if(payload && hdr->length) memcpy(pkt_buf + sizeof(pbcp_pkt_header_t), payload, hdr->length);

    uint8_t bits[MAX_PACKET_BYTES*8];
    size_t nbits = 0;
    for(size_t i=0;i<pkt_len;i++)
        for(int b=0;b<8;b++)
            bits[nbits++] = (pkt_buf[i] >> b) & 1;

    float pcm[PCM_BUFFER_SIZE];
    int nsamples = afsk_encode_bits(enc, bits, nbits, pcm, PCM_BUFFER_SIZE);
    Pa_WriteStream(stream, pcm, nsamples);
    return 0;
}

// ---------------- Main RX ----------------
int main() {
    afsk_config_t cfg = {
        .sample_rate = 48000.0,
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

    // Initialize PortAudio
    Pa_Initialize();
    PaStream *rx_stream, *tx_stream;
    Pa_OpenDefaultStream(&rx_stream, 1, 0, paFloat32, cfg.sample_rate, PCM_BUFFER_SIZE, NULL, NULL);
    Pa_OpenDefaultStream(&tx_stream, 0, 1, paFloat32, cfg.sample_rate, PCM_BUFFER_SIZE, NULL, NULL);
    Pa_StartStream(rx_stream);
    Pa_StartStream(tx_stream);

    printf("[RX] Listening on input device: SampleRate=%.2f\n", cfg.sample_rate);

    pbcp_pkt_header_t hdr;
    uint8_t *payload = NULL;
    char msg_buf[8192] = {0};
    size_t msg_offset = 0;

    // Wait for SYNC with retries
    int retries = 0;
    while(1) {
        if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_SYNC) {
            printf("[RX] Received SYNC\n");
            break;
        }
        usleep(1000);
        if(++retries % 5000 == 0) {
            printf("[RX] Still waiting for SYNC...\n");
            afsk_decoder_reset(&dec);
        }
    }

    // Send ACK
    pbcp_pkt_header_t ack = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_ACK, 0};
    send_packet(tx_stream, &enc, &ack, NULL);
    printf("[RX] Sent ACK\n");

    // Send INFO
    pbcp_payload_info_t info = {0x12345678, 1, 0, 0};
    pbcp_pkt_header_t info_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_INFO, sizeof(info)};
    send_packet(tx_stream, &enc, &info_hdr, (uint8_t*)&info);
    printf("[RX] Sent INFO\n");

    // Receive DATA until END
    while(1) {
        if(recv_packet(rx_stream, &dec, &hdr, &payload) < 0) {
            usleep(500);
            continue;
        }

        if(hdr.type == PBCP_TYPE_DATA) {
            memcpy(msg_buf + msg_offset, payload, hdr.length);
            msg_offset += hdr.length;
            printf("[RX] Received DATA: ");
            for(size_t i=0;i<hdr.length;i++) printf("%02X ", payload[i]);
            printf("\n");
        } else if(hdr.type == PBCP_TYPE_END) {
            printf("[RX] Received END\n");
            pbcp_pkt_header_t final_ack = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_ACK, 0};
            send_packet(tx_stream, &enc, &final_ack, NULL);
            printf("[RX] Sent final ACK\n");
            break;
        } else if(hdr.type == PBCP_TYPE_ERR) {
            pbcp_payload_err_t *err = (pbcp_payload_err_t*)payload;
            fprintf(stderr, "[RX] Received ERR code 0x%02X\n", err->code);
            break;
        }

        free(payload);
        payload = NULL;
    }

    printf("[RX] Message received:\n%s\n", msg_buf);

    Pa_StopStream(rx_stream);
    Pa_StopStream(tx_stream);
    Pa_CloseStream(rx_stream);
    Pa_CloseStream(tx_stream);
    Pa_Terminate();
    free(payload);

    return 0;
}
