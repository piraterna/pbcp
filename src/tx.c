#include "protocol.h"
#include "afsk/afsk.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <portaudio.h>
#include <unistd.h>
#include <time.h>

#define PCM_BUFFER_SIZE 48000
#define SYNC_RETRY_MS 500   // milliseconds
#define SYNC_MAX_RETRIES 10

// Encode & send packet over audio stream
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

// Read audio and decode one packet
int recv_packet(PaStream *stream, afsk_decoder_t *dec, pbcp_pkt_header_t *hdr, uint8_t **payload) {
    float pcm[PCM_BUFFER_SIZE];
    int nsamples = Pa_ReadStream(stream, pcm, PCM_BUFFER_SIZE);
    if(nsamples <= 0) return -1;

    uint8_t bits[1024*8];
    int nbits = afsk_decode_pcm(dec, pcm, nsamples, bits, NULL, sizeof(bits));
    if(nbits <= 0) return -1;

    uint8_t pkt_buf[1024];
    size_t nbytes = nbits / 8;
    for(size_t i=0;i<nbytes;i++){
        pkt_buf[i] = 0;
        for(int b=0;b<8;b++)
            pkt_buf[i] |= bits[i*8+b] << b;
    }
    if(nbytes < sizeof(pbcp_pkt_header_t)) return -1;

    memcpy(hdr, pkt_buf, sizeof(pbcp_pkt_header_t));

    if(hdr->length > 0) {
        *payload = realloc(*payload, hdr->length);
        memcpy(*payload, pkt_buf + sizeof(pbcp_pkt_header_t), hdr->length);
    }
    return 0;
}

int main() {
    // Initialize AFSK
    afsk_encoder_t enc;
    afsk_config_t enc_cfg = {48000, 1200, 1200, 2200, 0.9, 1};
    afsk_encoder_init(&enc, &enc_cfg);

    afsk_decoder_t dec;
    afsk_config_t dec_cfg = enc_cfg;
    afsk_decoder_init(&dec, &dec_cfg);

    // Init PortAudio
    Pa_Initialize();
    PaStream *tx_stream, *rx_stream;
    Pa_OpenDefaultStream(&tx_stream, 0, 1, paFloat32, 48000, 256, NULL, NULL);
    Pa_OpenDefaultStream(&rx_stream, 1, 0, paFloat32, 48000, 256, NULL, NULL);
    Pa_StartStream(tx_stream);
    Pa_StartStream(rx_stream);

    // Begin handshake: send SYNC until ACK received
    pbcp_pkt_header_t hdr;
    uint8_t *payload = NULL;
    pbcp_pkt_header_t sync = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_SYNC, 0};
    int retries = 0;
    while(retries < SYNC_MAX_RETRIES) {
        send_packet(tx_stream, &enc, &sync, NULL);
        printf("[TX] Sent SYNC (try %d)\n", retries+1);

        // Wait for ACK with timeout
        time_t start = time(NULL);
        int got_ack = 0;
        while(difftime(time(NULL), start) * 1000 < SYNC_RETRY_MS) {
            if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_ACK) {
                got_ack = 1;
                printf("[TX] Received ACK\n");
                break;
            }
        }
        if(got_ack) break;
        retries++;
    }
    if(retries == SYNC_MAX_RETRIES) {
        fprintf(stderr, "[TX] Failed to handshake with receiver\n");
        return 1;
    }

    // Wait for INFO from RX
    while(1) {
        if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_INFO) {
            pbcp_payload_info_t *info = (pbcp_payload_info_t*)payload;
            printf("[TX] Received INFO: ID=0x%08X, Capabilities=0x%02X\n", info->receiver_id, info->capabilities);
            free(payload);
            payload = NULL;
            break;
        }
    }

    // Send DATA packets
    const char *messages[] = {"Hello, ", "World!"};
    for(size_t i=0;i<sizeof(messages)/sizeof(messages[0]);i++){
        pbcp_pkt_header_t data_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_DATA, (uint16_t)strlen(messages[i])};
        send_packet(tx_stream, &enc, &data_hdr, (uint8_t*)messages[i]);
        printf("[TX] Sent DATA %zu\n", i+1);
        usleep(200000); // short delay between packets
    }

    // Send END
    pbcp_pkt_header_t end_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_END, 0};
    send_packet(tx_stream, &enc, &end_hdr, NULL);
    printf("[TX] Sent END\n");

    // Wait final ACK
    while(1) {
        if(recv_packet(rx_stream, &dec, &hdr, &payload) == 0 && hdr.type == PBCP_TYPE_ACK) {
            printf("[TX] Received final ACK\n");
            break;
        }
    }

    Pa_StopStream(tx_stream);
    Pa_StopStream(rx_stream);
    Pa_CloseStream(tx_stream);
    Pa_CloseStream(rx_stream);
    Pa_Terminate();
    return 0;
}
