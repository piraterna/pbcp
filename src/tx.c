#include "protocol.h"
#include "afsk/afsk.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <portaudio.h>
#include <unistd.h>

#define PCM_BUFFER_SIZE 1024
#define SYNC_RETRY_US 500000  // 0.5s
#define SYNC_MAX_RETRIES 20

// ---------------- TX Helpers ----------------
int send_packet(PaStream *stream, afsk_encoder_t *enc, const pbcp_pkt_header_t *hdr, const uint8_t *payload) {
    uint8_t pkt_buf[1024];
    size_t pkt_len = sizeof(pbcp_pkt_header_t) + hdr->length;
    if(pkt_len > sizeof(pkt_buf)) return -1;

    memcpy(pkt_buf, hdr, sizeof(pbcp_pkt_header_t));
    if(payload && hdr->length) memcpy(pkt_buf + sizeof(pbcp_pkt_header_t), payload, hdr->length);

    uint8_t bits[1024*8];
    size_t nbits = 0;
    for(size_t i=0;i<pkt_len;i++)
        for(int b=0;b<8;b++)
            bits[nbits++] = (pkt_buf[i] >> b) & 1;

    float pcm[PCM_BUFFER_SIZE];
    int nsamples = afsk_encode_bits(enc, bits, nbits, pcm, PCM_BUFFER_SIZE);
    Pa_WriteStream(stream, pcm, nsamples);
    return 0;
}

int recv_packet(PaStream *stream, afsk_decoder_t *dec, pbcp_pkt_header_t *hdr, uint8_t **payload) {
    float pcm[PCM_BUFFER_SIZE];
    int nsamples = Pa_ReadStream(stream, pcm, PCM_BUFFER_SIZE);
    if(nsamples <= 0) return -1;

    uint8_t bits[1024*8];
    int nbits = afsk_decode_pcm(dec, pcm, nsamples, bits, NULL, sizeof(bits));
    if(nbits <= 0) return -1;

    size_t nbytes = nbits / 8;
    if(nbytes < sizeof(pbcp_pkt_header_t)) return -1;

    uint8_t pkt_buf[1024];
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

// ---------------- Main TX ----------------
int main() {
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

    // Init PortAudio
    Pa_Initialize();
    PaStream *tx_stream, *rx_stream;
    Pa_OpenDefaultStream(&tx_stream, 0, 1, paFloat32, cfg.sample_rate, PCM_BUFFER_SIZE, NULL, NULL);
    Pa_OpenDefaultStream(&rx_stream, 1, 0, paFloat32, cfg.sample_rate, PCM_BUFFER_SIZE, NULL, NULL);
    Pa_StartStream(tx_stream);
    Pa_StartStream(rx_stream);

    pbcp_pkt_header_t hdr;
    uint8_t *payload = NULL;

    // ---------------- Handshake ----------------
    pbcp_pkt_header_t sync = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_SYNC, 0};
    int retries = 0;
    while(retries < SYNC_MAX_RETRIES) {
        send_packet(tx_stream, &enc, &sync, NULL);
        printf("[TX] Sent SYNC (try %d)\n", retries+1);

        usleep(SYNC_RETRY_US);

        if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_ACK) {
            printf("[TX] Received ACK\n");
            free(payload); payload = NULL;
            break;
        }

        retries++;
        if(retries == SYNC_MAX_RETRIES) {
            fprintf(stderr, "[TX] Failed to handshake with receiver\n");
            return 1;
        }
    }

    // Wait for INFO from RX
    while(1) {
        if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_INFO) {
            pbcp_payload_info_t *info = (pbcp_payload_info_t*)payload;
            printf("[TX] Received INFO: ID=0x%08X, Capabilities=0x%02X\n", info->receiver_id, info->capabilities);
            free(payload); payload = NULL;
            break;
        }
        usleep(50000);
    }

    // ---------------- Send DATA ----------------
    const char *messages[] = {"Hello, ", "World!"};
    for(size_t i=0;i<sizeof(messages)/sizeof(messages[0]);i++){
        pbcp_pkt_header_t data_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_DATA, (uint16_t)strlen(messages[i])};
        send_packet(tx_stream, &enc, &data_hdr, (uint8_t*)messages[i]);
        printf("[TX] Sent DATA %zu\n", i+1);
        usleep(200000); // short delay between packets
    }

    // ---------------- Send END ----------------
    pbcp_pkt_header_t end_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_END, 0};
    send_packet(tx_stream, &enc, &end_hdr, NULL);
    printf("[TX] Sent END\n");

    // Wait final ACK
    while(1) {
        if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_ACK) {
            printf("[TX] Received final ACK\n");
            free(payload); payload = NULL;
            break;
        }
        usleep(50000);
    }

    Pa_StopStream(tx_stream);
    Pa_StopStream(rx_stream);
    Pa_CloseStream(tx_stream);
    Pa_CloseStream(rx_stream);
    Pa_Terminate();
    return 0;
}
