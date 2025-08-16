#include "protocol.h"
#include "afsk/afsk.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#define MAX_PACKET_BYTES 1024
#define PCM_BUFFER_SIZE  48000
#define BITS_PER_BYTE    8

typedef struct {
    float buf[PCM_BUFFER_SIZE];
    size_t n_samples;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int ready;
} audio_channel_t;

audio_channel_t tx_to_rx = { .lock=PTHREAD_MUTEX_INITIALIZER, .cond=PTHREAD_COND_INITIALIZER, .ready=0 };

static size_t packet_to_bits(const uint8_t *pkt, size_t nbytes, uint8_t *bits) {
    for(size_t i=0;i<nbytes;i++)
        for(int b=0;b<8;b++) bits[i*8+b] = (pkt[i] >> b) & 1;
    return nbytes*8;
}

static size_t bits_to_packet(const uint8_t *bits, size_t nbits, uint8_t *pkt) {
    size_t nbytes = nbits/8;
    for(size_t i=0;i<nbytes;i++) {
        pkt[i] = 0;
        for(int b=0;b<8;b++) pkt[i] |= bits[i*8+b] << b;
    }
    return nbytes;
}

static void send_afsk_packet(const pbcp_pkt_header_t *hdr, const uint8_t *payload, afsk_encoder_t *enc) {
    uint8_t pkt_buf[MAX_PACKET_BYTES];
    size_t pkt_len = sizeof(pbcp_pkt_header_t) + hdr->length;
    memcpy(pkt_buf, hdr, sizeof(pbcp_pkt_header_t));
    if(hdr->length>0 && payload) memcpy(pkt_buf+sizeof(pbcp_pkt_header_t), payload, hdr->length);
    uint8_t bits[MAX_PACKET_BYTES*8];
    size_t nbits = packet_to_bits(pkt_buf, pkt_len, bits);
    float pcm[PCM_BUFFER_SIZE];
    int nsamples = afsk_encode_bits(enc, bits, nbits, pcm, PCM_BUFFER_SIZE);
    pthread_mutex_lock(&tx_to_rx.lock);
    memcpy(tx_to_rx.buf, pcm, nsamples*sizeof(float));
    tx_to_rx.n_samples = nsamples;
    tx_to_rx.ready = 1;
    pthread_cond_signal(&tx_to_rx.cond);
    pthread_mutex_unlock(&tx_to_rx.lock);
}

static void wait_afsk_packet(pbcp_pkt_header_t *hdr, uint8_t *payload, afsk_decoder_t *dec) {
    pthread_mutex_lock(&tx_to_rx.lock);
    while(!tx_to_rx.ready) pthread_cond_wait(&tx_to_rx.cond, &tx_to_rx.lock);
    float *pcm = tx_to_rx.buf;
    size_t nsamples = tx_to_rx.n_samples;
    tx_to_rx.ready = 0;
    pthread_mutex_unlock(&tx_to_rx.lock);
    uint8_t bits[MAX_PACKET_BYTES*8];
    int nbits = afsk_decode_pcm(dec, pcm, nsamples, bits, NULL, sizeof(bits));
    if(nbits<=0) { fprintf(stderr, "[!] Decode error\n"); return; }
    bits_to_packet(bits, nbits, (uint8_t*)hdr);
    if(hdr->length>0 && payload) memcpy(payload, ((uint8_t*)hdr)+sizeof(pbcp_pkt_header_t), hdr->length);
}

static int validate_info(const pbcp_payload_info_t *info) {
    return info->capabilities == 0x00; // capabilities are unimplemented
}

void* receiver(void *arg) {
    afsk_decoder_t dec;
    afsk_config_t cfg = {48000, 1200, 1200, 2200, 0.9, 1};
    afsk_decoder_init(&dec, &cfg);
    afsk_encoder_t enc;
    afsk_encoder_init(&enc, &cfg);

    pbcp_pkt_header_t hdr;
    uint8_t payload[256];
    char msg_buf[512] = {0};

    printf("[#] Receiver: Begin Handshake\n");
    wait_afsk_packet(&hdr, payload, &dec);
    if(hdr.type != PBCP_TYPE_SYNC) return NULL;
    pbcp_pkt_header_t ack = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_ACK, 0};
    send_afsk_packet(&ack, NULL, &enc);
    printf("[>] Receiver: Sent ACK\n");

    pbcp_payload_info_t info = {0x12345678,1,0,0};
    pbcp_pkt_header_t info_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_INFO, sizeof(info)};
    send_afsk_packet(&info_hdr, (uint8_t*)&info, &enc);
    printf("[>] Receiver: Sent INFO: ID=0x%08X, FW=%d.%d, Capabilities=0x%02X\n",
           info.receiver_id, info.firmware_major, info.firmware_minor, info.capabilities);

    wait_afsk_packet(&hdr, payload, &dec);
    if(hdr.type == PBCP_TYPE_ERR) {
        pbcp_payload_err_t *err = (pbcp_payload_err_t*)payload;
        fprintf(stderr, "[!] Receiver: Received ERR code 0x%02X (%s)\n", err->code, PBCP_ERROR_STR(err->code));
        return NULL;
    }
    if(hdr.type == PBCP_TYPE_DATA) {
        memcpy(msg_buf, payload, hdr.length);
        printf("[>] Receiver: Received DATA (hex): ");
        for(size_t i = 0; i < hdr.length; i++)
            printf("%02X ", payload[i]);
        printf("\n");
    }
    printf("[#] Receiver: Handshake complete\nMessage:\n------------------------\n%s\n------------------------\n", msg_buf);
    return NULL;
}

void* transmitter(void *arg) {
    afsk_encoder_t enc;
    afsk_config_t cfg = {48000, 1200, 1200, 2200, 0.9, 1};
    afsk_encoder_init(&enc, &cfg);
    afsk_decoder_t dec;
    afsk_decoder_init(&dec, &cfg);

    const char *msg = "Hello, World!";

    printf("[#] AFSK stream initialized: 1200 bps, 1200/2200 Hz @ 48kHz\n");
    pbcp_pkt_header_t sync = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_SYNC, 0};
    send_afsk_packet(&sync, NULL, &enc);
    printf("[<] Transmitter: Sent SYNC\n");

    pbcp_pkt_header_t hdr;
    uint8_t payload[256];
    wait_afsk_packet(&hdr, payload, &dec);
    if(hdr.type == PBCP_TYPE_ACK) printf("[<] Transmitter: Received ACK\n");

    wait_afsk_packet(&hdr, payload, &dec);
    if(hdr.type == PBCP_TYPE_ERR) {
        pbcp_payload_err_t *err = (pbcp_payload_err_t*)payload;
        fprintf(stderr, "[!] Transmitter: Received ERR code 0x%02X\n", err->code);
        return NULL;
    }
    if(hdr.type == PBCP_TYPE_INFO) {
        pbcp_payload_info_t *info = (pbcp_payload_info_t*)payload;
        printf("[<] Transmitter: Received INFO: ID=0x%08X, FW=%d.%d, Capabilities=0x%02X\n",
               info->receiver_id, info->firmware_major, info->firmware_minor, info->capabilities);
        if(!validate_info(info)) {
            pbcp_payload_err_t err_pkt = { PBCP_ERR_INVALID_CAPABILITIES };
            pbcp_pkt_header_t err_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_ERR, sizeof(err_pkt)};
            send_afsk_packet(&err_hdr, (uint8_t*)&err_pkt, &enc);
            return NULL;
        }
    }

    pbcp_pkt_header_t data_hdr = {PBCP_PREAMBLE, PBCP_MAGIC, PBCP_TYPE_DATA, (uint16_t)strlen(msg)};
    send_afsk_packet(&data_hdr, (uint8_t*)msg, &enc);
    printf("[<] Transmitter: Sent DATA\n");
    printf("[#] Transmitter: Transfer complete\n");
    return NULL;
}

int main() {
    pthread_t rx, tx;
    pthread_create(&rx, NULL, receiver, NULL);
    sleep(1);
    pthread_create(&tx, NULL, transmitter, NULL);
    pthread_join(tx, NULL);
    pthread_join(rx, NULL);
    return 0;
}
