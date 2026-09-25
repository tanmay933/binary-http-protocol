#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define HEADER_SIZE 12
#define MAX_PATH_SIZE 65535

#define MAGIC_0 0x42
#define MAGIC_1 0x48
#define BHTTP_VERSION 0x01

#define FRAME_REQUEST 0x01
#define FLAG_END_STREAM 0x01

ssize_t write_full(int fd, const void *buffer, size_t count) {
    size_t total = 0;
    const char *ptr = buffer;

    while (total < count) {
        ssize_t n = write(fd, ptr + total, count - total);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        total += n;
    }

    return total;
}

int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port, &hints, &result);

    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    int sockfd = -1;

    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd < 0) {
            continue;
        }

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);

    return sockfd;
}

void write_uint16(unsigned char *data, uint16_t value) {
    data[0] = (value >> 8) & 0xFF;
    data[1] = value & 0xFF;
}

void write_uint32(unsigned char *data, uint32_t value) {
    data[0] = (value >> 24) & 0xFF;
    data[1] = (value >> 16) & 0xFF;
    data[2] = (value >> 8) & 0xFF;
    data[3] = value & 0xFF;
}

void write_uint24(unsigned char *data, uint32_t value) {
    data[0] = (value >> 16) & 0xFF;
    data[1] = (value >> 8) & 0xFF;
    data[2] = value & 0xFF;
}

int main(int argc, char *argv[]) {
    int verbose = 0;
    const char *address;
    const char *path;

    if (argc == 4 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
        address = argv[2];
        path = argv[3];
    } else if (argc == 3) {
        address = argv[1];
        path = argv[2];
    } else {
        fprintf(stderr, "Usage: %s [-v] <host:port> <path>\n", argv[0]);
        return 1;
    }

    if (path[0] != '/') {
        fprintf(stderr, "Path must begin with '/'\n");
        return 1;
    }

    size_t path_length = strlen(path);

    if (path_length > MAX_PATH_SIZE) {
        fprintf(stderr, "Path is too long\n");
        return 1;
    }

    char address_copy[1024];

    if (strlen(address) >= sizeof(address_copy)) {
        fprintf(stderr, "Address is too long\n");
        return 1;
    }

    strcpy(address_copy, address);

    char *colon = strrchr(address_copy, ':');

    if (colon == NULL) {
        fprintf(stderr, "Invalid address. Expected host:port\n");
        return 1;
    }

    *colon = '\0';

    const char *host = address_copy;
    const char *port = colon + 1;

    int sockfd = connect_to_server(host, port);

    if (sockfd < 0) {
        fprintf(stderr, "Could not connect to %s:%s\n", host, port);
        return 1;
    }

    size_t payload_length = 1 + 2 + path_length;

    unsigned char header[HEADER_SIZE];
    unsigned char *payload = malloc(payload_length);

    if (payload == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        close(sockfd);
        return 1;
    }

    header[0] = MAGIC_0;
    header[1] = MAGIC_1;
    header[2] = BHTTP_VERSION;
    header[3] = FRAME_REQUEST;
    header[4] = FLAG_END_STREAM;

    write_uint24(header + 5, payload_length);
    write_uint32(header + 8, 1);

    payload[0] = 0x01;
    write_uint16(payload + 1, path_length);
    memcpy(payload + 3, path, path_length);

    if (verbose) {
        printf("Request frame:\n");

        for (size_t i = 0; i < HEADER_SIZE; i++) {
            printf("%02X ", header[i]);
        }

        for (size_t i = 0; i < payload_length; i++) {
            printf("%02X ", payload[i]);
        }

        printf("\n");
    }

    if (write_full(sockfd, header, HEADER_SIZE) != HEADER_SIZE) {
        perror("write");
        free(payload);
        close(sockfd);
        return 1;
    }

    if (write_full(sockfd, payload, payload_length) != (ssize_t)payload_length) {
        perror("write");
        free(payload);
        close(sockfd);
        return 1;
    }

    free(payload);
    close(sockfd);

    return 0;
}