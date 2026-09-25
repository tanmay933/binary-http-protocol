/*
 * bcurl.c — BHTTP/1 client
 *
 * A minimal command-line client for the custom binary BHTTP/1 protocol.
 * Connects to a server over TCP, sends a single REQUEST frame, reads and
 * decodes the corresponding RESPONSE frame, and prints the result in a
 * human-readable form.
 *
 * Usage:
 *   ./bcurl [-v] <host:port> <path>
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O2 -o bcurl bcurl.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

/* ---- Protocol constants (exactly as specified; do not change) --------- */

#define MAGIC0            0x42
#define MAGIC1            0x48
#define PROTOCOL_VERSION  0x01
#define FRAME_HEADER_SIZE 12

#define FRAME_TYPE_REQUEST  0x01
#define FRAME_TYPE_RESPONSE 0x02
#define FRAME_TYPE_ERROR    0x03

#define FLAG_END_STREAM 0x01

#define METHOD_GET 0x01

#define HEADER_CONTENT_LENGTH 0x01
#define HEADER_CONTENT_TYPE   0x02

#define FIRST_STREAM_ID 1

/* Implementation-defined safety cap on how large a RESPONSE payload we
 * are willing to allocate for. The wire format allows up to 0xFFFFFF
 * (16 MiB - 1); we accept up to that but guard against absurd values. */
#define MAX_RESPONSE_PAYLOAD 0xFFFFFFu

/* ============================================================
 * Reliable I/O helpers — TCP can deliver partial reads/writes, so a
 * single read()/write() call must never be assumed to be complete.
 * ============================================================ */

static ssize_t read_full(int fd, void *buf, size_t n) {
    size_t total = 0;
    uint8_t *p = buf;
    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break; /* EOF */
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static ssize_t write_full(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const uint8_t *p = buf;
    while (total < n) {
        ssize_t w = write(fd, p + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (ssize_t)total;
}

/* ============================================================
 * Big-endian integer helpers. Explicit byte-at-a-time encode/decode is
 * used instead of packed structs to avoid alignment/portability issues
 * when interpreting raw network bytes.
 * ============================================================ */

static uint16_t be16_to_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static void u16_to_be16(uint16_t v, uint8_t *p) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint32_t be24_to_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
static void u32_to_be24(uint32_t v, uint8_t *p) {
    p[0] = (uint8_t)((v >> 16) & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)(v & 0xFF);
}

static uint32_t be32_to_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void u32_to_be32(uint32_t v, uint8_t *p) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* ============================================================
 * Frame header
 * ============================================================ */

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint32_t length;    /* 24-bit value, stored widened */
    uint32_t stream_id;
} frame_header_t;

static void encode_frame_header(const frame_header_t *hdr, uint8_t out[FRAME_HEADER_SIZE]) {
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = PROTOCOL_VERSION;
    out[3] = hdr->type;
    out[4] = hdr->flags;
    u32_to_be24(hdr->length, out + 5);
    u32_to_be32(hdr->stream_id, out + 8);
}

static int decode_frame_header(const uint8_t in[FRAME_HEADER_SIZE], frame_header_t *hdr) {
    if (in[0] != MAGIC0 || in[1] != MAGIC1) return -1;
    if (in[2] != PROTOCOL_VERSION) return -1;
    hdr->type      = in[3];
    hdr->flags     = in[4];
    hdr->length    = be24_to_u32(in + 5);
    hdr->stream_id = be32_to_u32(in + 8);
    return 0;
}

/* ============================================================
 * Hex dump (used by -v)
 * ============================================================ */

static void hex_dump(const char *label, const uint8_t *buf, size_t len) {
    printf("%s (%zu bytes):\n", label, len);
    for (size_t i = 0; i < len; i += 16) {
        printf("  %04zx: ", i);
        size_t line_end = (i + 16 < len) ? i + 16 : len;
        for (size_t j = i; j < i + 16; j++) {
            if (j < line_end) printf("%02x ", buf[j]);
            else printf("   ");
        }
        printf(" ");
        for (size_t j = i; j < line_end; j++) {
            unsigned char c = buf[j];
            putchar(isprint(c) ? (int)c : '.');
        }
        printf("\n");
    }
}

/* ============================================================
 * Connection helpers
 * ============================================================ */

/* Splits "host:port" into separate host and port strings. Uses the
 * *last* colon as the separator so that hostnames themselves cannot
 * contain a literal ':' in a way that confuses parsing (this client
 * targets plain hostnames/IPv4, not bracketed IPv6 literals). Returns
 * 0 on success; on success, *host and *port are malloc'd and must be
 * freed by the caller. */
static int parse_host_port(const char *hostport, char **host, char **port) {
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport || *(colon + 1) == '\0') return -1;

    size_t host_len = (size_t)(colon - hostport);
    char *h = malloc(host_len + 1);
    char *p = malloc(strlen(colon + 1) + 1);
    if (!h || !p) {
        free(h);
        free(p);
        return -1;
    }
    memcpy(h, hostport, host_len);
    h[host_len] = '\0';
    strcpy(p, colon + 1);

    *host = h;
    *port = p;
    return 0;
}

static int connect_to(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_rc = getaddrinfo(host, port, &hints, &res);
    if (gai_rc != 0) {
        fprintf(stderr, "getaddrinfo(%s, %s): %s\n", host, port, gai_strerror(gai_rc));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "failed to connect to %s:%s\n", host, port);
        return -1;
    }
    return fd;
}

/* ============================================================
 * REQUEST construction and sending
 * ============================================================ */

/* Builds a complete REQUEST frame (header + payload) for a GET request
 * on the given path. Returns a malloc'd buffer via *out_frame and its
 * length via *out_len. Returns 0 on success, -1 on failure (e.g. path
 * too long to fit the uint16 length field). */
static int build_get_request_frame(uint32_t stream_id, const char *path,
                                    uint8_t **out_frame, size_t *out_len) {
    size_t path_len = strlen(path);
    if (path_len > 0xFFFF) return -1;

    size_t payload_len = 1 + 2 + path_len; /* method + path length + path */
    size_t frame_len = FRAME_HEADER_SIZE + payload_len;

    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;

    frame_header_t hdr;
    hdr.type      = FRAME_TYPE_REQUEST;
    hdr.flags     = FLAG_END_STREAM; /* a GET request has no body to follow */
    hdr.length    = (uint32_t)payload_len;
    hdr.stream_id = stream_id;
    encode_frame_header(&hdr, frame);

    size_t pos = FRAME_HEADER_SIZE;
    frame[pos++] = METHOD_GET;
    u16_to_be16((uint16_t)path_len, frame + pos);
    pos += 2;
    memcpy(frame + pos, path, path_len);
    pos += path_len;

    *out_frame = frame;
    *out_len = frame_len;
    return 0;
}

/* ============================================================
 * RESPONSE decoding
 * ============================================================ */

/* Reads one complete frame (header + payload) from fd.
 * Returns 0 on success, -1 on any I/O or protocol-level framing error
 * (bad magic/version, truncated read, payload too large). On success,
 * *out_payload is malloc'd (possibly zero-length, in which case it may
 * be NULL) and must be freed by the caller. */
static int read_frame(int fd, frame_header_t *hdr,
                       uint8_t **out_payload, uint32_t *out_payload_len) {
    uint8_t raw_header[FRAME_HEADER_SIZE];
    ssize_t r = read_full(fd, raw_header, sizeof(raw_header));
    if (r != (ssize_t)sizeof(raw_header)) {
        fprintf(stderr, "error: connection closed or truncated while reading frame header\n");
        return -1;
    }

    if (decode_frame_header(raw_header, hdr) != 0) {
        fprintf(stderr, "error: invalid magic or unsupported protocol version in response\n");
        return -1;
    }

    if (hdr->length > MAX_RESPONSE_PAYLOAD) {
        fprintf(stderr, "error: response payload length exceeds protocol limit\n");
        return -1;
    }

    uint8_t *payload = NULL;
    if (hdr->length > 0) {
        payload = malloc(hdr->length);
        if (!payload) {
            fprintf(stderr, "error: out of memory reading response payload\n");
            return -1;
        }
        if (read_full(fd, payload, hdr->length) != (ssize_t)hdr->length) {
            fprintf(stderr, "error: connection closed or truncated while reading response payload\n");
            free(payload);
            return -1;
        }
    }

    *out_payload = payload;
    *out_payload_len = hdr->length;
    return 0;
}

/* Decodes and prints a RESPONSE payload:
 *   2 bytes  Status Code
 *   1 byte   Header Count
 *   headers...
 *   body...
 * Returns 0 on success, -1 if the payload is malformed. */
static int print_response_payload(const uint8_t *payload, uint32_t len) {
    if (len < 3) {
        fprintf(stderr, "error: response payload too short\n");
        return -1;
    }

    uint16_t status = be16_to_u16(payload);
    uint8_t header_count = payload[2];
    size_t pos = 3;

    printf("Status: %u\n", status);

    for (uint8_t i = 0; i < header_count; i++) {
        if (pos + 3 > len) {
            fprintf(stderr, "error: truncated header in response\n");
            return -1;
        }
        uint8_t header_id = payload[pos];
        uint16_t value_len = be16_to_u16(payload + pos + 1);
        pos += 3;

        if (pos + value_len > len) {
            fprintf(stderr, "error: truncated header value in response\n");
            return -1;
        }

        const char *name;
        switch (header_id) {
            case HEADER_CONTENT_LENGTH: name = "Content-Length"; break;
            case HEADER_CONTENT_TYPE:   name = "Content-Type";   break;
            default:                    name = "X-Unknown-Header"; break;
        }

        printf("%s: %.*s\n", name, (int)value_len, (const char *)(payload + pos));
        pos += value_len;
    }

    printf("\n");

    size_t body_len = len - pos;
    if (body_len > 0) {
        fwrite(payload + pos, 1, body_len, stdout);
        /* Ensure trailing newline for readability if body doesn't end in one. */
        if (payload[len - 1] != '\n') printf("\n");
    }

    return 0;
}

/* ============================================================
 * main
 * ============================================================ */

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-v] <host:port> <path>\n", prog);
}

int main(int argc, char **argv) {
    int verbose = 0;
    const char *hostport_arg = NULL;
    const char *path_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (hostport_arg == NULL) {
            hostport_arg = argv[i];
        } else if (path_arg == NULL) {
            path_arg = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (hostport_arg == NULL || path_arg == NULL) {
        usage(argv[0]);
        return 1;
    }

    /* A server closing/resetting the connection must not kill this
     * client via SIGPIPE when we write() to it. */
    signal(SIGPIPE, SIG_IGN);

    char *host = NULL, *port = NULL;
    if (parse_host_port(hostport_arg, &host, &port) != 0) {
        fprintf(stderr, "error: invalid host:port '%s'\n", hostport_arg);
        return 1;
    }

    int fd = connect_to(host, port);
    if (fd < 0) {
        free(host);
        free(port);
        return 1;
    }
    free(host);
    free(port);

    uint32_t stream_id = FIRST_STREAM_ID;

    uint8_t *req_frame = NULL;
    size_t req_frame_len = 0;
    if (build_get_request_frame(stream_id, path_arg, &req_frame, &req_frame_len) != 0) {
        fprintf(stderr, "error: failed to build request (path too long?)\n");
        close(fd);
        return 1;
    }

    if (verbose) {
        hex_dump("Request frame", req_frame, req_frame_len);
    }

    if (write_full(fd, req_frame, req_frame_len) != (ssize_t)req_frame_len) {
        fprintf(stderr, "error: failed to send request: %s\n", strerror(errno));
        free(req_frame);
        close(fd);
        return 1;
    }
    free(req_frame);

    frame_header_t resp_hdr;
    uint8_t *resp_payload = NULL;
    uint32_t resp_payload_len = 0;

    if (read_frame(fd, &resp_hdr, &resp_payload, &resp_payload_len) != 0) {
        close(fd);
        return 1;
    }

    if (verbose) {
        uint8_t raw_header[FRAME_HEADER_SIZE];
        encode_frame_header(&resp_hdr, raw_header);

        size_t full_len = FRAME_HEADER_SIZE + (size_t)resp_payload_len;
        uint8_t *full_frame = malloc(full_len);
        if (full_frame) {
            memcpy(full_frame, raw_header, FRAME_HEADER_SIZE);
            if (resp_payload_len > 0) {
                memcpy(full_frame + FRAME_HEADER_SIZE, resp_payload, resp_payload_len);
            }
            hex_dump("Response frame", full_frame, full_len);
            free(full_frame);
        }
    }

    if (resp_hdr.stream_id != stream_id) {
        fprintf(stderr, "error: response stream ID %u does not match request stream ID %u\n",
                resp_hdr.stream_id, stream_id);
        free(resp_payload);
        close(fd);
        return 1;
    }

    int exit_code = 0;

    if (resp_hdr.type == FRAME_TYPE_RESPONSE) {
        if (print_response_payload(resp_payload, resp_payload_len) != 0) {
            exit_code = 1;
        }
    } else if (resp_hdr.type == FRAME_TYPE_ERROR) {
        fprintf(stderr, "error: server sent an ERROR frame (%u bytes payload)\n",
                resp_payload_len);
        if (verbose) {
            hex_dump("ERROR payload", resp_payload, resp_payload_len);
        }
        exit_code = 1;
    } else {
        fprintf(stderr, "error: unexpected frame type %u in response\n", resp_hdr.type);
        exit_code = 1;
    }

    if (!(resp_hdr.flags & FLAG_END_STREAM)) {
        fprintf(stderr, "warning: response did not set END_STREAM (unexpected for this client)\n");
    }

    free(resp_payload);

    /* The current operation (single request/response) is complete; only
     * now is it safe to close the connection. The code above reads a
     * full frame at a time via read_frame()/read_full(), so extending
     * this to send further REQUEST frames on the same fd before closing
     * would be a straightforward loop around the request/response logic. */
    close(fd);

    return exit_code;
}