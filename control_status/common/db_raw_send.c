//
// Created by Wolfgang Christl on 30.11.17.
// This file is part of DroneBridge
// https://github.com/seeul8er/DroneBridge
//

#include <sys/socket.h>
#include <stdint.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <linux/if_ether.h>
#include <string.h>
#include <stdlib.h>
#include <zconf.h>
#include <linux/if_packet.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "db_protocol.h"
#include "db_raw_send.h"

uint8_t radiotap_header_pre[] = {

        0x00, 0x00, // <-- radiotap version
        0x0c, 0x00, // <- radiotap header length
        0x04, 0x80, 0x00, 0x00, // <-- bitmap
        0x24,       // data rate (will be overwritten)
        0x00,
        0x18, 0x00
};
const uint8_t frame_control_pre_data[] =
        {
                0x08, 0x00, 0x00, 0x00
        };
const uint8_t frame_control_pre_beacon[] =
        {
                0x80, 0x00, 0x00, 0x00
        };

struct ifreq if_idx;
struct ifreq if_mac;
char interfaceName[IFNAMSIZ];
struct sockaddr_ll socket_address;
char mode = 'm';
int socket_ground_comm;

//uint8_t monitor_framebuffer_uni[RADIOTAP_LENGTH + DB80211_HEADER_LENGTH + DATA_UNI_LENGTH];
struct radiotap_header *rth_uni = (struct radiotap_header *) monitor_framebuffer_uni;
struct db_80211_header *db802_uni = (struct db_80211_header *) (monitor_framebuffer_uni + RADIOTAP_LENGTH);
struct data_uni *monitor_databuffer_uni = (struct data_uni *) (monitor_framebuffer_uni + RADIOTAP_LENGTH + DB80211_HEADER_LENGTH);

/**
 * Set the transmission bit rate in the radiotap header. Only works with ralink cards.
 * @param bitrate_option
 */
void set_bitrate(int bitrate_option) {
    switch (bitrate_option){
        case 1:
            radiotap_header_pre[8] = 0x05;
            break;
        case 2:
            radiotap_header_pre[8] = 0x09;
            break;
        case 3:
            radiotap_header_pre[8] = 0x0c;
            break;
        case 4:
            radiotap_header_pre[8] = 0x18;
            break;
        case 5:
            radiotap_header_pre[8] = 0x24;
            break;
        default:
            fprintf(stderr,"DB_SEND: Wrong bitrate option\n");
            exit(1);
    }

}

/**
 * Setup of the the DroneBridge raw protocol header
 * @param comm_id
 * @param bitrate_option
 * @param frame_type
 * @param direction
 * @return The socket file descriptor in case of a success or -1 if we screwed up
 */
int conf_monitor(uint8_t comm_id[4], int bitrate_option, int frame_type, uint8_t direction) {
    memset(monitor_framebuffer_uni, 0, (RADIOTAP_LENGTH + DB80211_HEADER_LENGTH + DATA_UNI_LENGTH));
    set_bitrate(bitrate_option);
    memcpy(rth_uni->bytes, radiotap_header_pre, RADIOTAP_LENGTH);
    // build custom DroneBridge 802.11 header
    if (frame_type == 1) {
        memcpy(db802_uni->frame_control_field, frame_control_pre_data, 4);
    } else {
        memcpy(db802_uni->frame_control_field, frame_control_pre_beacon, 4);
    }
    memcpy(db802_uni->comm_id, comm_id, 4);
    db802_uni->odd = 0x01;
    db802_uni->direction_dstmac = direction;
    memcpy(db802_uni->src_mac_bytes, ((uint8_t *) &if_mac.ifr_hwaddr.sa_data), 6);
    db802_uni->version_bytes = DB_VERSION;
    db802_uni->direction_bytes = direction;
    db802_uni->undefined[0] = 0x10;
    db802_uni->undefined[1] = 0x86;
    uint8_t crc8_db_header = 0;
    crc8_db_header ^= (db802_uni->version_bytes & 0xFF);
    crc8_db_header ^= (db802_uni->port_bytes & 0xFF);
    crc8_db_header ^= (db802_uni->direction_bytes & 0xFF);
    crc8_db_header ^= (db802_uni->payload_length_bytes[0] & 0xFF);
    crc8_db_header ^= (db802_uni->payload_length_bytes[1] & 0xFF);
    db802_uni->crc_bytes = crc8_db_header;
    if (setsockopt(socket_ground_comm, SOL_SOCKET, SO_BINDTODEVICE, interfaceName, IFNAMSIZ) < 0) {
        printf("DB_SEND: Error binding monitor socket to interface. Closing socket. Please restart.\n");
        close(socket_ground_comm);
        return -1;
    }
    /* Index of the network device */
    socket_address.sll_ifindex = if_idx.ifr_ifindex;

    return socket_ground_comm;
}

/**
 * Opens and configures a socket for sending DroneBridge raw protocol frames.
 * @param ifName Name of the network interface the socket is bound to
 * @param comm_id The communication ID
 * @param trans_mode The transmission mode (m|w) for monitor or wifi
 * @param bitrate_option Transmission bit rate. Only works with Ralink cards
 * @param frame_type Is it a atheros or ralink card. Determines if data frame or beacon frame
 * @param direction Is the packet for the drone or the groundstation
 * @return the socket file descriptor or -1 if something went wrong
 */
int open_socket_sending(char ifName[IFNAMSIZ], uint8_t comm_id[4], char trans_mode, int bitrate_option, int frame_type,
                        uint8_t direction){
    mode = trans_mode;

    if (mode == 'w') {
        // TODO: ignore for now. I will be UDP in future.
        if ((socket_ground_comm = socket(AF_PACKET, SOCK_RAW, IPPROTO_RAW)) == -1) {
            perror("DB_SEND: socket");
            return -1;
        }else{
            printf("DB_SEND: Opened socket for wifi mode\n");
        }

    } else {
        if ((socket_ground_comm = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_802_2))) == -1) {
            perror("DB_SEND: socket");
            return -1;
        }else{
            printf("DB_SEND: Opened raw socket for monitor mode\n");
        }
    }

    /* Get the index of the interface to send on */
    memset(&if_idx, 0, sizeof(struct ifreq));
    strncpy(if_idx.ifr_name, ifName, IFNAMSIZ - 1);
    if (ioctl(socket_ground_comm, SIOCGIFINDEX, &if_idx) < 0) {
        perror("DB_SEND: SIOCGIFINDEX");
        return -1;
    }
    /* Get the MAC address of the interface to send on */
    memset(&if_mac, 0, sizeof(struct ifreq));
    strncpy(if_mac.ifr_name, ifName, IFNAMSIZ - 1);
    if (ioctl(socket_ground_comm, SIOCGIFHWADDR, &if_mac) < 0) {
        perror("DB_SEND: SIOCGIFHWADDR");
        return -1;
    }

    if (trans_mode == 'w') {
        printf("DB_SEND: Wifi mode is not yet supported!\n");
        return -1;
        //return conf_ethernet(dest_mac);
    } else {
        return conf_monitor(comm_id, bitrate_option, frame_type, direction);
    }
}

/**
 * Overwrites payload set inside monitor_framebuffer_uni with the provided payload.
 * @param payload The payload bytes of the message to be sent. Does use memcpy to write payload into buffer.
 * @param dest_port The DroneBridge destination port of the message (see db_protocol.h)
 * @param payload_length The length of the payload in bytes
 * @return 0 if success or -1 if failure
 */
int send_packet(int8_t payload[], uint8_t dest_port, u_int16_t payload_length){
    db802_uni->payload_length_bytes[0] = (uint8_t) (payload_length & 0xFF);
    db802_uni->payload_length_bytes[1] = (uint8_t) ((payload_length >> 8) & 0xFF);
    db802_uni->port_bytes = dest_port;
    memcpy(monitor_databuffer_uni->bytes, payload, payload_length);
    if (sendto(socket_ground_comm, monitor_framebuffer_uni, (size_t) (RADIOTAP_LENGTH + DB80211_HEADER_LENGTH +
            payload_length), 0, (struct sockaddr *) &socket_address, sizeof(struct sockaddr_ll)) < 0) {
        printf("DB_SEND: Send failed (monitor): %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Use this function for maximum performance. No memcpy used. Create a pointer directly pointing inside the
 * monitor_framebuffer_uni array in your script. Fill that with your payload and call this function.
 * This function only sends the monitor_framebuffer_uni. You need to make sure you get the payload inside it.
 * E.g. This will create such a pointer structure:
 * struct data_rc_status_update *rc_status_update_data = (struct data_rc_status_update *)
            (monitor_framebuffer_uni + RADIOTAP_LENGTH + DB80211_HEADER_LENGTH);
 * Make sure you update your data every time before sending.
 * @param dest_port The DroneBridge destination port of the message (see db_protocol.h)
 * @param payload_length The length of the payload in bytes
 * @return 0 if success or -1 if failure
 */
int send_packet_hp(uint8_t dest_port, u_int16_t payload_length){
    db802_uni->payload_length_bytes[0] = (uint8_t) (payload_length & 0xFF);
    db802_uni->payload_length_bytes[1] = (uint8_t) ((payload_length >> 8) & 0xFF);
    db802_uni->port_bytes = dest_port;
    if (sendto(socket_ground_comm, monitor_framebuffer_uni, (size_t) (RADIOTAP_LENGTH + DB80211_HEADER_LENGTH +
                       payload_length), 0, (struct sockaddr *) &socket_address, sizeof(struct sockaddr_ll)) < 0) {
        printf("DB_SEND: Send failed (monitor): %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

void close_socket_send(){
    close(socket_ground_comm);
}