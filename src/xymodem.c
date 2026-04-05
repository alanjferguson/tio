/*
 * Minimalistic implementation of the xmodem-1k and ymodem sender protocol.
 * https://en.wikipedia.org/wiki/XMODEM
 * https://en.wikipedia.org/wiki/YMODEM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later OR MIT-0
 *
 */

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <poll.h>
#include "xymodem.h"
#include "print.h"
#include "misc.h"

#define SOH 0x01
#define STX 0x02
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define EOT 0x04

#define SOH_STR "\001"
#define ACK_STR "\006"
#define NAK_STR "\025"
#define CAN_STR "\030"
#define EOT_STR "\004"

#define OK  0
#define ERR (-1)
#define ERR_FATAL (-2)
#define USER_CAN  (-5)

#define RX_IGNORE 5

#define min(a, b)       ((a) < (b) ? (a) : (b))

/* Packet header: type + seq + nseq */
#define XMODEM_HDR_SIZE 3

struct xpacket_1k {
    uint8_t  type;
    uint8_t  seq;
    uint8_t  nseq;
    uint8_t  data[1024];
} __attribute__((packed));

struct xpacket {
    uint8_t  type;
    uint8_t  seq;
    uint8_t  nseq;
    uint8_t  data[128];
} __attribute__((packed));

/* See https://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks */
static uint16_t crc16(const uint8_t *data, uint16_t size)
{
    uint16_t crc, s;

    for (crc = 0; size > 0; size--) {
        s = *data++ ^ (crc >> 8);
        s ^= (s >> 4);
        crc = (crc << 8) ^ s ^ (s << 5) ^ (s << 12);
    }
    return crc;
}

/* Simple 8-bit checksum for original XMODEM */
static uint8_t checksum8(const uint8_t *data, uint16_t size)
{
    uint8_t sum = 0;

    for (uint16_t i = 0; i < size; i++)
    {
        sum += data[i];
    }
    return sum;
}

/*
 * Write a complete XMODEM packet to the serial port: header+data from the
 * packet struct, followed by the appropriate error check bytes (2-byte CRC
 * or 1-byte checksum).
 */
static int write_packet(int sio, const void *pkt, size_t pkt_size,
                        const uint8_t *payload, size_t payload_size,
                        bool use_crc)
{
    uint8_t  buf[XMODEM_HDR_SIZE + 1024 + 2]; /* max: hdr + 1K data + CRC */
    size_t   sz;
    int      rc;
    char    *from;

    /* Copy header + data */
    memcpy(buf, pkt, pkt_size);
    sz = pkt_size;

    /* Append error check */
    if (use_crc)
    {
        uint16_t crc = crc16(payload, payload_size);
        buf[sz++] = crc >> 8;
        buf[sz++] = crc & 0xff;
    }
    else
    {
        buf[sz++] = checksum8(payload, payload_size);
    }

    /* Send the complete packet */
    from = (char *) buf;
    while (sz) {
        if (key_hit)
            return ERR;
        if ((rc = write(sio, from, sz)) < 0) {
            if (errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            tio_error_print("Write packet to serial failed");
            return ERR;
        }
        from += rc;
        sz   -= rc;
    }

    return OK;
}

static int xmodem_1k(int sio, const void *data, size_t len, int seq)
{
    struct xpacket_1k  packet;
    const uint8_t  *buf = data;
    char            resp = 0;
    int             rc;
    bool            use_crc = true;

    /* Drain pending characters from serial line.
     * Accept 'C' (CRC mode) or NAK (checksum mode).
     */
    while(1) {
        if (key_hit)
            return -1;
        rc = read_poll(sio, &resp, 1, 50);
        if (rc == 0) {
            if (resp == 'C')
            {
                use_crc = true;
                break;
            }
            if (resp == NAK)
            {
                use_crc = false;
                break;
            }
            if (resp == CAN) return ERR;
            continue;
        }
        else if (rc < 0) {
            tio_error_print("Read sync from serial failed");
            return ERR;
        }
    }

    /* Always work with 1K packets */
    packet.seq  = seq;
    packet.type = STX;

    while (len) {
        size_t  z = 0;
        char    status;

        /* Build next packet, pad with 0 to full seq */
        z = min(len, sizeof(packet.data));
        memcpy(packet.data, buf, z);
        memset(packet.data + z, 0, sizeof(packet.data) - z);
        packet.nseq = 0xff - packet.seq;

        /* Send packet with error check */
        rc = write_packet(sio, &packet, sizeof(packet),
                          packet.data, sizeof(packet.data), use_crc);
        if (rc < 0)
            return ERR;

        /* Clear response */
        resp = 0;

        /* 'lrzsz' does not ACK ymodem's fin packet */
        if (seq == 0 && packet.data[0] == 0) resp = ACK;

        /* Read receiver response, timeout 1 s */
        for(int n=0; n < 20; n++) {
            if (key_hit)
                return ERR;
            rc = read_poll(sio, &resp, 1, 50);
            if (rc < 0) {
                tio_error_print("Read ack/nak from serial failed");
                return ERR;
            } else if(rc > 0) {
                break;
            }
        }

        /* Update "progress bar" */
        switch (resp) {
        case NAK: status = 'N'; break;
        case ACK: status = '.'; break;
        case 'C': status = 'C'; break;
        case CAN: status = '!'; return ERR;
        default:  status = '?';
        }
        write(STDOUT_FILENO, &status, 1);

        /* Move to next block after ACK */
        if (resp == ACK) {
            packet.seq++;
            len -= z;
            buf += z;
        }
    }

    /* Send EOT at 1 Hz until ACK or CAN received */
    while (seq) {
        if (key_hit)
            return ERR;
        if (write(sio, EOT_STR, 1) < 0) {
            tio_error_print("Write EOT to serial failed");
            return ERR;
        }
        write(STDOUT_FILENO, "|", 1);
        /* 1s timeout */
        rc = read_poll(sio, &resp, 1, 1000);
        if (rc < 0) {
            tio_error_print("Read from serial failed");
            return ERR;
        } else if(rc == 0) {
            continue;
        }
        if (resp == ACK || resp == CAN) {
            write(STDOUT_FILENO, "\r\n", 2);
            return (resp == ACK) ? OK : ERR;
        }
    }
    return 0; /* not reached */
}

static int xmodem(int sio, const void *data, size_t len)
{
    struct xpacket  packet;
    const uint8_t  *buf = data;
    char            resp = 0;
    int             rc;
    bool            use_crc = true;

    /* Drain pending characters from serial line.
     * Accept 'C' (CRC mode) or NAK (checksum mode).
     */
    while(1) {
        if (key_hit)
            return -1;
        rc = read_poll(sio, &resp, 1, 50);
        if (rc == 0) {
            if (resp == 'C')
            {
                use_crc = true;
                break;
            }
            if (resp == NAK)
            {
                use_crc = false;
                break;
            }
            if (resp == CAN) return ERR;
            continue;
        }
        else if (rc < 0) {
            tio_error_print("Read sync from serial failed");
            return ERR;
        }
    }

    /* Always work with 128b packets */
    packet.seq  = 1;
    packet.type = SOH;

    while (len) {
        size_t  z = 0;
        char    status;

        /* Build next packet, pad with 0 to full seq */
        z = min(len, sizeof(packet.data));
        memcpy(packet.data, buf, z);
        memset(packet.data + z, 0, sizeof(packet.data) - z);
        packet.nseq = 0xff - packet.seq;

        /* Send packet with error check */
        rc = write_packet(sio, &packet, sizeof(packet),
                          packet.data, sizeof(packet.data), use_crc);
        if (rc < 0)
            return ERR;

        /* Clear response */
        resp = 0;

        /* Read receiver response, timeout 1 s */
        for(int n=0; n < 20; n++) {
            if (key_hit)
                return ERR;
            rc = read_poll(sio, &resp, 1, 50);
            if (rc < 0) {
                tio_error_print("Read ack/nak from serial failed");
                return ERR;
            } else if(rc > 0) {
                break;
            }
        }

        /* Update "progress bar" */
        switch (resp) {
        case NAK: status = 'N'; break;
        case ACK: status = '.'; break;
        case 'C': status = 'C'; break;
        case CAN: status = '!'; return ERR;
        default:  status = '?';
        }
        write(STDOUT_FILENO, &status, 1);

        /* Move to next block after ACK */
        if (resp == ACK) {
            packet.seq++;
            len -= z;
            buf += z;
        }
    }

    /* Send EOT at 1 Hz until ACK or CAN received */
    while (1) {
        if (key_hit)
            return ERR;
        if (write(sio, EOT_STR, 1) < 0) {
            tio_error_print("Write EOT to serial failed");
            return ERR;
        }
        write(STDOUT_FILENO, "|", 1);
        /* 1s timeout */
        rc = read_poll(sio, &resp, 1, 1000);
        if (rc < 0) {
            tio_error_print("Read from serial failed");
            return ERR;
        } else if(rc == 0) {
            continue;
        }
        if (resp == ACK || resp == CAN) {
            write(STDOUT_FILENO, "\r\n", 2);
            return (resp == ACK) ? OK : ERR;
        }
    }
    return 0; /* not reached */
}

/*
 * Start an XMODEM receive session.
 *
 * Per the XMODEM/CRC spec, try sending 'C' first to request CRC mode.
 * After several failed attempts, fall back to NAK to request checksum mode.
 * Sets *use_crc to indicate the negotiated mode.
 */
static int start_receive(int sio, bool *use_crc)
{
    int rc;
    struct pollfd fds;
    fds.events = POLLIN;
    fds.fd = sio;

    /* Try CRC mode first: send 'C' up to 10 times */
    for (int n = 0; n < 10; n++)
    {
        rc = write(sio, "C", 1);
        if (rc < 0) {
            if (errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            tio_error_print("Write start byte to serial failed");
            return ERR;
        }
        rc = poll(&fds, 1, 3000);
        if (rc < 0)
        {
            tio_error_print("%s", strerror(errno));
            return rc;
        }
        else if (rc > 0)
        {
            if (fds.revents & POLLIN)
            {
                *use_crc = true;
                return rc;
            }
        }
        if (key_hit)
            return USER_CAN;
    }

    /* Fall back to checksum mode: send NAK up to 10 times */
    for (int n = 0; n < 10; n++)
    {
        rc = write(sio, NAK_STR, 1);
        if (rc < 0) {
            if (errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            tio_error_print("Write start byte to serial failed");
            return ERR;
        }
        rc = poll(&fds, 1, 3000);
        if (rc < 0)
        {
            tio_error_print("%s", strerror(errno));
            return rc;
        }
        else if (rc > 0)
        {
            if (fds.revents & POLLIN)
            {
                *use_crc = false;
                return rc;
            }
        }
        if (key_hit)
            return USER_CAN;
    }

    return 0;
}

static int receive_packet(int sio, struct xpacket packet, int fd, bool use_crc)
{
    char rxSeq1, rxSeq2 = 0;
    char resp = 0;
    uint16_t calcCrc = 0;
    uint8_t calcSum = 0;
    int rc;

    struct pollfd fds;
    fds.events = POLLIN;
    fds.fd = sio;

    /* Read seq bytes */
    rc = read_poll(sio, &rxSeq1, 1, 3000);
    if (rc == 0) {
        tio_error_print("Timeout waiting for first seq byte");
        return ERR;
    } else if (rc < 0) {
        tio_error_print("Error reading first seq byte");
        return ERR_FATAL;
    }
    rc = read_poll(sio, &rxSeq2, 1, 3000);
    if (rc == 0) {
        tio_error_print("Timeout waiting for second seq byte");
        return ERR;
    } else if (rc < 0) {
        tio_error_print("Error reading second seq byte");
        return ERR_FATAL;
    }
    if (key_hit)
        return USER_CAN;

    /* Read packet data */
    for (unsigned ix = 0; (ix < sizeof(packet.data)); ix++)
    {
        rc = read_poll(sio, &resp, 1, 3000);
        if (rc == 0)
        {
            tio_error_print("Timeout waiting for next packet char");
            rc = write(sio, CAN_STR, 1);
            if (rc < 0) {
                tio_error_print("Write cancel packet to serial failed");
                return ERR_FATAL;
            }
            return ERR;
        } else if (rc < 0) {
            tio_error_print("Error reading next packet char");
            rc = write(sio, CAN_STR, 1);
            if (rc < 0) {
                tio_error_print("Write cancel packet to serial failed");
            }
            return ERR_FATAL;
        }
        packet.data[ix] = (uint8_t) resp;
        if (use_crc)
        {
            calcCrc = calcCrc ^ ((uint16_t)((uint8_t)resp) << 8);
            for (int b = 0; b < 8; b++)
            {
                if (calcCrc & 0x8000)
                    calcCrc = (calcCrc << 1) ^ 0x1021;
                else
                    calcCrc <<= 1;
            }
        }
        else
        {
            calcSum += (uint8_t) resp;
        }
        if (key_hit)
            return USER_CAN;
    }

    /* Read error check bytes */
    bool check_ok = false;
    if (use_crc)
    {
        /* Read 2-byte CRC */
        uint16_t rxCrc = 0;

        rc = read_poll(sio, &resp, 1, 3000);
        if (rc == 0) {
            tio_error_print("Timeout waiting for first CRC byte");
            return ERR;
        } else if (rc < 0) {
            tio_error_print("Error reading first CRC byte");
            return ERR_FATAL;
        }
        rxCrc = ((uint16_t)(uint8_t)resp) << 8;

        rc = read_poll(sio, &resp, 1, 3000);
        if (rc == 0) {
            tio_error_print("Timeout waiting for second CRC byte");
            return ERR;
        } else if (rc < 0) {
            tio_error_print("Error reading second CRC byte");
            return ERR_FATAL;
        }
        rxCrc |= (uint16_t)(uint8_t)resp;

        check_ok = (calcCrc == rxCrc);
        if (!check_ok)
        {
            tio_debug_printf("CRC read: %u", rxCrc);
            tio_debug_printf("CRC calculated: %u", calcCrc);
        }
    }
    else
    {
        /* Read 1-byte checksum */
        uint8_t rxSum = 0;

        rc = read_poll(sio, &resp, 1, 3000);
        if (rc == 0) {
            tio_error_print("Timeout waiting for checksum byte");
            return ERR;
        } else if (rc < 0) {
            tio_error_print("Error reading checksum byte");
            return ERR_FATAL;
        }
        rxSum = (uint8_t)resp;

        check_ok = (calcSum == rxSum);
        if (!check_ok)
        {
            tio_debug_printf("Checksum read: %u", rxSum);
            tio_debug_printf("Checksum calculated: %u", calcSum);
        }
    }

    if (key_hit)
        return USER_CAN;

    /* Verify no stale data in receive buffer */
    rc = poll(&fds, 1, 10);
    if (rc < 0)
    {
        tio_error_print("%s", strerror(errno));
        tio_error_print("Poll check error after packet finish");
        rc = write(sio, CAN_STR, 1);
        if (rc < 0) {
            tio_error_print("Write cancel packet to serial failed");
        }
        return ERR_FATAL;
    }
    else if (rc > 0)
    {
        if (fds.revents & POLLIN)
        {
            tio_error_print("RX sync error");
            char dummy = 0;
            /* Drain buffer */
            while (read_poll(sio, &dummy, 1, 100) > 0) {}
            return ERR;
        }
    }

    uint8_t tester = 0xff;
    uint8_t seq1 = rxSeq1;
    uint8_t seq2 = rxSeq2;

    if (check_ok && (seq1 == packet.seq - 1) && ((seq1 ^ seq2) == tester))
    {
        /* Resend of previously processed packet. */
        rc = write(sio, ACK_STR, 1);
        if (rc < 0) {
            tio_error_print("Write acknowledgement packet to serial failed");
            return ERR_FATAL;
        }
        return RX_IGNORE;
    }
    else if (!check_ok || (seq1 != packet.seq) || ((seq1 ^ seq2) != tester))
    {
        /* Fail if the error check or sequence number is not correct or if
           the two received sequence numbers are not the complement of one
           another. */
        tio_error_print("Bad %s or sequence number", use_crc ? "CRC" : "checksum");
        tio_debug_printf("Seq read: %hhu", rxSeq1);
        tio_debug_printf("Seq should be: %hhu", packet.seq);
        tio_debug_printf("inv seq: %hhu", rxSeq2);
        return ERR;
    }
    else
    {
        /* The data is good.  Process the packet then ACK it to the sender. */
        rc = write(fd, packet.data, sizeof(packet.data));
        if (rc < 0)
        {
            tio_error_print("Problem writing to file");
            rc = write(sio, CAN_STR, 1);
            if (rc < 0) {
                tio_error_print("Write cancel packet to serial failed");
            }
            return ERR_FATAL;
        }
        rc = write(sio, ACK_STR, 1);
        if (rc < 0)
        {
            tio_error_print("Write acknowledgement packet to serial failed");
            return ERR_FATAL;
        }
    }

    return OK;
}

static int xmodem_receive_transfer(int sio, int fd)
{
    struct xpacket  packet;
    char            resp = 0;
    int             rc;
    bool            complete = false;
    bool            use_crc = true;
    char            status;

    /* Drain pending characters from serial line. */
    while(1) {
        if (key_hit)
            return -1;
        rc = read_poll(sio, &resp, 1, 50);
        if (rc == 0) {
            if (resp == CAN) return ERR;
            break;
        }
        else if (rc < 0) {
            if (rc != USER_CAN) {
                tio_error_print("Read sync from serial failed");
            }
            return ERR;
        }
    }

    /* Always work with 128b packets */
    packet.seq  = 1;
    packet.type = SOH;

    /* Start receive - negotiates CRC or checksum mode */
    rc = start_receive(sio, &use_crc);
    if (rc == 0)
    {
        tio_error_print("Timeout waiting for transfer to start");
        return ERR;
    } else if (rc < 0) {
        tio_error_print("Error starting XMODEM receive");
        return ERR;
    }

    while (!complete) {
        /* Poll for 1 new byte for 3 seconds */
        rc = read_poll(sio, &resp, 1, 3000);
        if (rc == 0) {
            tio_error_print("Timeout waiting for start of next packet");
            return ERR;
        } else if (rc < 0) {
            tio_error_print("Error reading start of next packet");
            return ERR;
        }
        if (key_hit)
            return USER_CAN;

        switch(resp)
        {
            case SOH:
            /* Start of a packet */
            rc = receive_packet(sio, packet, fd, use_crc);
            if (rc == OK) {
                packet.seq++;
                status = '.';
            } else if (rc == ERR) {
                rc = write(sio, NAK_STR, 1);
                if (rc < 0) {
                    tio_error_print("Writing not acknowledge packet to serial failed");
                    return ERR;
                }
                status = 'N';
            } else if (rc == ERR_FATAL) {
                tio_error_print("Receive cancelled due to fatal error");
                return ERR;
            } else if (rc == USER_CAN) {
                rc = write(sio, CAN_STR, 1);
                if (rc < 0) {
                    tio_error_print("Writing cancel to serial failed");
                    return ERR;
                }
                return USER_CAN;
            } else if (rc == RX_IGNORE) {
                status = ':';
            }
            break;

            case EOT:
            /* End of Transfer */
            rc = write(sio, ACK_STR, 1);
            if (rc < 0)
            {
                tio_error_print("Write acknowledgement packet to serial failed");
                return ERR;
            }
            complete = true;
            status = '\0';
            write(STDOUT_FILENO, "|\r\n", 3);
            break;

            case CAN:
            /* Cancel from sender */
            tio_error_print("Transmission cancelled from sender");
            return ERR;
            break;

            default:
            tio_error_print("Unexpected character received waiting for next packet");
            return ERR;
            break;
        }

        /* Update "progress bar" */
        write(STDOUT_FILENO, &status, 1);
    }
    return OK;
}

int xymodem_send(int sio, const char *filename, modem_mode_t mode)
{
    size_t         len;
    int            rc, fd;
    struct stat    stat;
    const uint8_t *buf;

    /* Open file, map into memory */
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        tio_error_print("Could not open file");
        return ERR;
    }
    fstat(fd, &stat);
    len = stat.st_size;
    buf = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!buf) {
        close(fd);
        tio_error_print("Could not mmap file");
        return ERR;
    }

    /* Do transfer */
    key_hit = 0;
    if (mode == XMODEM_1K) {
        rc = xmodem_1k(sio, buf, len, 1);
    }
    else if (mode == XMODEM_CRC) {
        rc = xmodem(sio, buf, len);
    }
    else {
        /* Ymodem: hdr + file + fin */
        while(1) {
            char hdr[1024], *p;

            rc = -1;
            if (strlen(filename) > 977) break; /* hdr block overrun */
            p  = stpncpy(hdr, filename, 1024) + 1;
            p += sprintf(p, "%ld %lo %o", len, stat.st_mtime, stat.st_mode);

            if (xmodem_1k(sio, hdr, p - hdr, 0) < 0) break; /* hdr with metadata */
            if (xmodem_1k(sio, buf, len,     1) < 0) break; /* xmodem file */
            if (xmodem_1k(sio, "",  1,       0) < 0) break; /* empty hdr = fin */
            rc = 0;                               break;
        }
    }
    key_hit = 0xff;

    /* Flush serial and release resources */
    tcflush(sio, TCIOFLUSH);
    munmap((void *)buf, len);
    close(fd);
    return rc;
}

int xymodem_receive(int sio, const char *filename, modem_mode_t mode)
{
    int            rc, fd;

    /* Create new file */
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        tio_error_print("Could not open file");
        return ERR;
    }

    /* Do transfer */
    key_hit = 0;
    if (mode == XMODEM_1K) {
        tio_error_print("Not supported");
        rc = -1;
    }
    else if (mode == XMODEM_CRC) {
        rc = xmodem_receive_transfer(sio, fd);
    }
    else {
        tio_error_print("Not supported");
        rc = -1;
    }
    key_hit = 0xff;

    /* Flush serial and release resources */
    tcflush(sio, TCIOFLUSH);
    close(fd);
    return rc;
}
