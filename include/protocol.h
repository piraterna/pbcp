#ifndef PBCP_PROTOCOL_H
#define PBCP_PROTOCOL_H

/**********************************/
/*  ALL VALUES ARE LITTLE ENDIAN  */
/**********************************/

#include <stdint.h>

// Fixed header values
#define PBCP_PREAMBLE 0x45 // 0b01000101
#define PBCP_MAGIC    0xD5 // 0b11010101

/**********************************/
/* Packet Types                    */
/**********************************/

// Handshake / control
#define PBCP_TYPE_SYNC  0x01 // Transmitter requests communication
#define PBCP_TYPE_ACK   0x02 // Receiver acknowledges
#define PBCP_TYPE_NACK  0x03 // Receiver rejects or not ready
#define PBCP_TYPE_INFO  0x04 // Receiver info (ID, version, capabilities)

// Data transfer
#define PBCP_TYPE_DATA  0x10 // Standard data packet
#define PBCP_TYPE_END   0x11 // End of transmission
#define PBCP_TYPE_ERR   0x12 // Error / retransmission request

/**********************************/
/* Error Codes                     */
/**********************************/
#define PBCP_ERR_INVALID_CAPABILITIES 0x01
#define PBCP_ERR_INVALID_PACKET       0x02
#define PBCP_ERR_LENGTH_MISMATCH      0x03
#define PBCP_ERR_UNKNOWN              0xFF

#define PBCP_ERROR_STR(code) \
((code) == PBCP_ERR_INVALID_CAPABILITIES ? "Invalid capabilities" : \
(code) == PBCP_ERR_INVALID_PACKET       ? "Invalid packet" : \
(code) == PBCP_ERR_LENGTH_MISMATCH      ? "Length mismatch" : \
(code) == PBCP_ERR_UNKNOWN              ? "Unknown error" : \
"Unrecognized error")

/**********************************/
/* Packet Header Structure         */
/**********************************/

typedef struct pbcp_pkt_header {
    uint8_t preamble;     // 8 bits, signals start of a packet
    uint8_t magic;        // 8 bits, used to verify this packet
    uint8_t type;         // 8 bits, indicates the packet type
    uint16_t length;      // 16 bits, length of payload (little-endian)
} __attribute__((packed)) pbcp_pkt_header_t;

/**********************************/
/* Payload Structures             */
/**********************************/

// INFO packet: receiver details
typedef struct pbcp_payload_info {
    uint32_t receiver_id;      // unique ID for receiver
    uint8_t firmware_major;    // firmware version major
    uint8_t firmware_minor;    // firmware version minor
    uint8_t capabilities;      // bitfield of features
} pbcp_payload_info_t;

// DATA packet: raw data payload
typedef struct pbcp_payload_data {
    // Variable length; actual length given in header.length
    uint8_t *data;
} pbcp_payload_data_t;

// ERR packet: error code
typedef struct pbcp_payload_err {
    uint8_t code;              // error code
} pbcp_payload_err_t;

#endif // PBCP_PROTOCOL_H
