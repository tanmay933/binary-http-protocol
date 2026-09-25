/*
 * bserve.c — BHTTP/1 server
 *
 * BHTTP/1 is a simple binary application-layer protocol running over a
 * persistent TCP connection. This file implements the server side:
 * frame parsing, REQUEST decoding, safe path resolution under a document
 * root, and RESPONSE/ERROR frame construction.
 *
 * Build:  cc -std=c11 -Wall -Wextra -O2 -o bserve bserve.c
 * Usage:  ./bserve <port>
 * Serves files out of ./www (fixed document root, per spec).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

#define STATUS_OK             200
#define STATUS_BAD_REQUEST    400
#define STATUS_NOT_FOUND      404
#define STATUS_INTERNAL_ERROR 500

#define DOCROOT "./www"

/* ---- Implementation-defined safety limits (not part of the wire format) */

/* A REQUEST payload is at most 1 (method) + 2 (path len) + 65535 (path).
 * Anything claiming to be larger is bogus; cap generously below the
 * 24-bit maximum (16 MiB) to avoid a hostile length field causing a huge
 * allocation. */
#define MAX_REQUEST_PAYLOAD (1 * 1024 * 1024)

/* Buffer large enough to hold the largest possible path (uint16 max) plus
 * a NUL terminator. */
#define REQ_PATH_MAX 65536

/* Defensive cap on path depth (number of '/'-separated segments). This is
 * not part of the protocol; it just bounds pathological inputs. */
#define MAX_SEGMENTS 256

/* Frame payload length field is 24 bits wide (max 0xFFFFFF). Leave some
 * headroom below that for response headers when deciding whether a file
 * is small enough to serve in a single RESPONSE frame. */
#define MAX_RESPONSE_BODY (0xFFFFFF - 128)

/* ============================================================
 * Reliable I/O helpers — required because TCP can deliver partial
 * reads/writes; never assume a single read()/write() call is complete.
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

/* Discard exactly n bytes from fd. Used to skip unknown frame types and
 * to keep header/payload framing in sync when we reject a frame without
 * fully buffering its payload. */
static int skip_payload(int fd, uint32_t n) {
    uint8_t buf[4096];
    while (n > 0) {
        size_t chunk = n < sizeof(buf) ? n : sizeof(buf);
        if (read_full(fd, buf, chunk) != (ssize_t)chunk) return -1;
        n -= (uint32_t)chunk;
    }
    return 0;
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

/* Reads and validates one 12-byte frame header.
 * Returns: 1 = success, 0 = clean EOF (no bytes read; connection closed
 * between frames), -1 = invalid magic/version or broken/partial read
 * (connection cannot be trusted to still be frame-aligned, so caller
 * must close it). */
static int read_frame_header(int fd, frame_header_t *hdr) {
    uint8_t buf[FRAME_HEADER_SIZE];
    ssize_t r = read_full(fd, buf, sizeof(buf));
    if (r == 0) return 0;
    if (r != (ssize_t)sizeof(buf)) return -1;

    if (buf[0] != MAGIC0 || buf[1] != MAGIC1) return -1;
    if (buf[2] != PROTOCOL_VERSION) return -1;

    hdr->type      = buf[3];
    hdr->flags     = buf[4];
    hdr->length    = be24_to_u32(buf + 5);
    hdr->stream_id = be32_to_u32(buf + 8);
    return 1;
}

/* ============================================================
 * RESPONSE frame construction
 * ============================================================ */

typedef struct {
    uint8_t        id;
    const uint8_t *value;
    uint16_t       value_len;
} response_header_t;

/* Builds and sends a complete RESPONSE frame (header + payload) in a
 * single write_full() call. header_count may be 0 (used for plain status
 * responses like 400/404/500). Returns 0 on success, -1 on failure
 * (socket error or payload too large for the 24-bit length field). */
static int send_response_frame(int fd, uint32_t stream_id, uint16_t status,
                                const response_header_t *headers,
                                uint8_t header_count,
                                const uint8_t *body, uint32_t body_len) {
    uint64_t payload_size = 2 + 1; /* status + header count */
    for (uint8_t i = 0; i < header_count; i++) {
        payload_size += 1 + 2 + headers[i].value_len;
    }
    payload_size += body_len;

    if (payload_size > 0xFFFFFFu) return -1; /* exceeds 24-bit length field */

    size_t frame_size = FRAME_HEADER_SIZE + (size_t)payload_size;
    uint8_t *frame = malloc(frame_size);
    if (!frame) return -1;

    frame[0] = MAGIC0;
    frame[1] = MAGIC1;
    frame[2] = PROTOCOL_VERSION;
    frame[3] = FRAME_TYPE_RESPONSE;
    frame[4] = FLAG_END_STREAM; /* every RESPONSE here completes its stream */
    u32_to_be24((uint32_t)payload_size, frame + 5);
    u32_to_be32(stream_id, frame + 8);

    size_t pos = FRAME_HEADER_SIZE;
    u16_to_be16(status, frame + pos); pos += 2;
    frame[pos++] = header_count;

    for (uint8_t i = 0; i < header_count; i++) {
        frame[pos++] = headers[i].id;
        u16_to_be16(headers[i].value_len, frame + pos); pos += 2;
        if (headers[i].value_len > 0) {
            memcpy(frame + pos, headers[i].value, headers[i].value_len);
            pos += headers[i].value_len;
        }
    }
    if (body_len > 0) {
        memcpy(frame + pos, body, body_len);
        pos += body_len;
    }

    int rc = (write_full(fd, frame, frame_size) == (ssize_t)frame_size) ? 0 : -1;
    free(frame);
    return rc;
}

static int send_status_response(int fd, uint32_t stream_id, uint16_t status) {
    return send_response_frame(fd, stream_id, status, NULL, 0, NULL, 0);
}

static int send_file_response(int fd, uint32_t stream_id,
                               const uint8_t *body, uint32_t body_len,
                               const char *mime) {
    char len_str[32];
    int len_str_len = snprintf(len_str, sizeof(len_str), "%u", body_len);
    if (len_str_len < 0) len_str_len = 0;

    response_header_t headers[2];
    headers[0].id        = HEADER_CONTENT_LENGTH;
    headers[0].value     = (const uint8_t *)len_str;
    headers[0].value_len = (uint16_t)len_str_len;

    headers[1].id        = HEADER_CONTENT_TYPE;
    headers[1].value     = (const uint8_t *)mime;
    headers[1].value_len = (uint16_t)strlen(mime);

    return send_response_frame(fd, stream_id, STATUS_OK, headers, 2,
                                body, body_len);
}

/* ============================================================
 * MIME type lookup
 * ============================================================ */

static const char *mime_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
        return "text/html";
    if (strcasecmp(dot, ".txt") == 0)  return "text/plain";
    if (strcasecmp(dot, ".css") == 0)  return "text/css";
    if (strcasecmp(dot, ".js") == 0)   return "application/javascript";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".png") == 0)  return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0)  return "image/gif";
    return "application/octet-stream";
}

/* ============================================================
 * Path safety
 *
 * Two layers of defense are used:
 *   1. Lexical normalization (below) rejects any ".." that would climb
 *      above the document root *before* the filesystem is touched at
 *      all. This blocks "/../secret" and "/foo/../../secret" directly.
 *   2. realpath() is then used to resolve symlinks in the remaining
 *      candidate, and the result is re-checked to still be inside the
 *      document root. This blocks a symlink placed inside www/ that
 *      points outside it.
 * ============================================================ */

/* Splits reqpath (which must start with '/') into '/'-separated segments,
 * resolves "." and ".." lexically, and rejects any ".." that would climb
 * above the root. Writes a normalized, root-relative path such as
 * "/a/b" (or "/" for the root itself) into out. Returns 0 on success,
 * -1 if the path is invalid or attempts to escape the root. */
static int normalize_relative_path(const char *reqpath, char *out, size_t outsz) {
    char buf[REQ_PATH_MAX];
    size_t len = strlen(reqpath);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, reqpath, len + 1);

    char *segments[MAX_SEGMENTS];
    int depth = 0;

    char *saveptr = NULL;
    char *token = strtok_r(buf, "/", &saveptr);
    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            /* no-op */
        } else if (strcmp(token, "..") == 0) {
            if (depth == 0) return -1; /* escape attempt above the root */
            depth--;
        } else {
            if (depth >= MAX_SEGMENTS) return -1;
            segments[depth++] = token;
        }
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (outsz < 2) return -1;
    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < depth; i++) {
        size_t seglen = strlen(segments[i]);
        if (pos + seglen + 1 >= outsz) return -1;
        memcpy(out + pos, segments[i], seglen);
        pos += seglen;
        if (i != depth - 1) out[pos++] = '/';
    }
    out[pos] = '\0';
    return 0;
}

/* Resolves reqpath under docroot_real and validates containment.
 * Returns one of STATUS_OK / STATUS_BAD_REQUEST / STATUS_NOT_FOUND /
 * STATUS_INTERNAL_ERROR. On STATUS_OK, out_resolved holds the final
 * absolute filesystem path of a regular file. */
static int resolve_and_check(const char *docroot_real, const char *reqpath,
                              char *out_resolved, size_t outsz) {
    char normalized[REQ_PATH_MAX];
    if (normalize_relative_path(reqpath, normalized, sizeof(normalized)) != 0) {
        /* Lexical ".." escape or malformed segment structure: this is an
         * invalid request, not merely a missing file. */
        return STATUS_BAD_REQUEST;
    }

    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s%s", docroot_real, normalized);
    if (n < 0 || (size_t)n >= sizeof(candidate)) return STATUS_BAD_REQUEST;

    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR) return STATUS_NOT_FOUND;
        return STATUS_INTERNAL_ERROR;
    }

    size_t rootlen = strlen(docroot_real);
    if (strncmp(resolved, docroot_real, rootlen) != 0 ||
        (resolved[rootlen] != '/' && resolved[rootlen] != '\0')) {
        /* realpath() resolved symlinks and the real target lands outside
         * the document root. Report as not-found rather than forbidden
         * so we don't confirm the existence of anything outside www/. */
        return STATUS_NOT_FOUND;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) return STATUS_NOT_FOUND;
    if (!S_ISREG(st.st_mode)) return STATUS_NOT_FOUND; /* only regular files */

    if (strlen(resolved) >= outsz) return STATUS_INTERNAL_ERROR;
    strcpy(out_resolved, resolved);
    return STATUS_OK;
}

/* ============================================================
 * File reading
 * ============================================================ */

static int read_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    /* O_NOFOLLOW is defense-in-depth against a TOCTOU race where a
     * symlink is swapped in between realpath() and open(); 'path' here
     * is already fully resolved and symlink-free, so this should never
     * reject a legitimate file. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    if (!S_ISREG(st.st_mode)) { close(fd); errno = EISDIR; return -1; }

    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size > 0 ? size : 1);
    if (!buf) { close(fd); errno = ENOMEM; return -1; }

    size_t total = 0;
    while (total < size) {
        ssize_t r = read(fd, buf + total, size - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); return -1;
        }
        if (r == 0) break; /* file truncated concurrently; serve what we got */
        total += (size_t)r;
    }
    close(fd);

    *out_buf = buf;
    *out_len = total;
    return 0;
}

/* ============================================================
 * REQUEST payload handling
 * ============================================================ */

/* Parses and services one REQUEST payload, sending exactly one RESPONSE
 * frame. Returns 0 (the connection stays open regardless of the HTTP-
 * style status sent). */
static int process_request_payload(int fd, uint32_t stream_id,
                                    const uint8_t *payload, uint32_t length,
                                    const char *docroot_real) {
    if (length < 3) {
        send_status_response(fd, stream_id, STATUS_BAD_REQUEST);
        return 0;
    }

    uint8_t  method   = payload[0];
    uint16_t path_len = be16_to_u16(payload + 1);

    /* The declared frame length must exactly match header + path; any
     * mismatch is a malformed frame. */
    if ((uint32_t)(3 + path_len) != length) {
        send_status_response(fd, stream_id, STATUS_BAD_REQUEST);
        return 0;
    }
    if (path_len == 0) {
        send_status_response(fd, stream_id, STATUS_BAD_REQUEST);
        return 0;
    }

    /* Reject embedded NUL bytes: the wire format is length-prefixed and
     * binary-safe, but path handling below uses C strings, and a hidden
     * NUL could make a path look safe while a different path is used. */
    for (uint16_t i = 0; i < path_len; i++) {
        if (payload[3 + i] == 0x00) {
            send_status_response(fd, stream_id, STATUS_BAD_REQUEST);
            return 0;
        }
    }

    char path_buf[REQ_PATH_MAX];
    memcpy(path_buf, payload + 3, path_len);
    path_buf[path_len] = '\0';

    if (path_buf[0] != '/') {
        send_status_response(fd, stream_id, STATUS_BAD_REQUEST);
        return 0;
    }
    if (method != METHOD_GET) {
        send_status_response(fd, stream_id, STATUS_BAD_REQUEST);
        return 0;
    }

    char resolved[PATH_MAX];
    int status = resolve_and_check(docroot_real, path_buf, resolved, sizeof(resolved));
    if (status != STATUS_OK) {
        send_status_response(fd, stream_id, (uint16_t)status);
        return 0;
    }

    uint8_t *body = NULL;
    size_t body_len = 0;
    if (read_file(resolved, &body, &body_len) != 0) {
        if (errno == ENOENT)
            send_status_response(fd, stream_id, STATUS_NOT_FOUND);
        else
            send_status_response(fd, stream_id, STATUS_INTERNAL_ERROR);
        return 0;
    }

    if (body_len > MAX_RESPONSE_BODY) {
        free(body);
        send_status_response(fd, stream_id, STATUS_INTERNAL_ERROR);
        return 0;
    }

    send_file_response(fd, stream_id, body, (uint32_t)body_len,
                        mime_type_for_path(resolved));
    free(body);
    return 0;
}

/* Reads a REQUEST frame's payload (given its already-read header) and
 * dispatches it. Returns 0 to keep the connection open, -1 if the
 * connection is broken and must be closed. */
static int handle_request_frame(int fd, const frame_header_t *hdr,
                                 const char *docroot_real) {
    if (hdr->length > MAX_REQUEST_PAYLOAD) {
        /* Still must drain the declared bytes to stay frame-aligned. */
        if (skip_payload(fd, hdr->length) != 0) return -1;
        send_status_response(fd, hdr->stream_id, STATUS_BAD_REQUEST);
        return 0;
    }

    uint8_t *payload = NULL;
    if (hdr->length > 0) {
        payload = malloc(hdr->length);
        if (!payload) {
            if (skip_payload(fd, hdr->length) != 0) return -1;
            send_status_response(fd, hdr->stream_id, STATUS_INTERNAL_ERROR);
            return 0;
        }
        if (read_full(fd, payload, hdr->length) != (ssize_t)hdr->length) {
            free(payload);
            return -1; /* connection broken mid-frame */
        }
    }

    int rc = process_request_payload(fd, hdr->stream_id, payload, hdr->length,
                                      docroot_real);
    free(payload);
    return rc;
}

/* ============================================================
 * Connection handling (persistent: many requests per TCP connection)
 * ============================================================ */

static void handle_connection(int fd, const char *docroot_real) {
    for (;;) {
        frame_header_t hdr;
        int rc = read_frame_header(fd, &hdr);
        if (rc == 0) break;  /* client closed cleanly between frames */
        if (rc < 0) break;   /* bad magic/version or broken read: framing
                               * can no longer be trusted, close connection */

        if (hdr.type == FRAME_TYPE_REQUEST) {
            if (handle_request_frame(fd, &hdr, docroot_real) != 0) break;
        } else {
            /* Unknown (or unexpected, e.g. RESPONSE/ERROR from a client)
             * frame type: skip its payload using the declared length
             * rather than trying to interpret it. */
            if (skip_payload(fd, hdr.length) != 0) break;
        }
    }
}

/* ============================================================
 * main
 * ============================================================ */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    long port = strtol(argv[1], NULL, 10);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }

    /* A client closing/resetting the connection must not kill the server
     * via SIGPIPE when we write() to it; write_full() relying on EPIPE
     * being returned as an error is the correct, safe behavior. */
    signal(SIGPIPE, SIG_IGN);

    static char docroot_real[PATH_MAX];
    if (realpath(DOCROOT, docroot_real) == NULL) {
        perror("realpath(docroot)");
        return 1;
    }
    struct stat rst;
    if (stat(docroot_real, &rst) != 0 || !S_ISDIR(rst.st_mode)) {
        fprintf(stderr, "document root is not a directory: %s\n", docroot_real);
        return 1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 16) != 0) {
        perror("listen");
        return 1;
    }

    printf("bserve listening on port %ld, docroot=%s\n", port, docroot_real);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        /* Serve every request on this connection sequentially until the
         * client closes it, then go back to accept(). */
        handle_connection(client_fd, docroot_real);
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}