#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HEADER_SIZE 12
#define MAX_PAYLOAD_SIZE 16777215

#define BHTTP_VERSION 0x01

#define FRAME_REQUEST  0x01
#define FRAME_RESPONSE 0x02
#define FRAME_ERROR    0x03

#define FLAG_END_STREAM 0x01

#define MAGIC_0 0x42
#define MAGIC_1 0x48

ssize_t read_full(int fd, void *buffer, size_t count) {
    size_t total = 0;
    char *ptr = buffer;

    while (total < count) {
        ssize_t n = read(fd, ptr + total, count - total);

        if (n == 0) {
            return total;
        }

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

uint32_t read_uint24(const unsigned char *data) {
    return ((uint32_t)data[0] << 16) |
           ((uint32_t)data[1] << 8) |
           (uint32_t)data[2];
}

uint32_t read_uint32(const unsigned char *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

int validate_header(const unsigned char *header) {
    if (header[0] != MAGIC_0 || header[1] != MAGIC_1) {
        fprintf(stderr, "Invalid magic\n");
        return 0;
    }

    if (header[2] != BHTTP_VERSION) {
        fprintf(stderr, "Unsupported version: %u\n", header[2]);
        return 0;
    }

    uint32_t payload_length = read_uint24(header + 5);

    if (payload_length > MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "Payload too large\n");
        return 0;
    }

    return 1;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <document_root> <port>\n", argv[0]);
        return 1;
    }

    const char *document_root = argv[1];
    int port = atoi(argv[2]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("BHTTP/1 server listening on port %d\n", port);
    printf("Document root: %s\n", document_root);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("Client connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        unsigned char header[HEADER_SIZE];

        ssize_t n = read_full(client_fd, header, HEADER_SIZE);

        if (n == 0) {
            printf("Client closed connection\n");
        } else if (n < 0) {
            perror("read");
        } else if (n != HEADER_SIZE) {
            fprintf(stderr, "Incomplete frame header\n");
        } else if (validate_header(header)) {
            uint8_t type = header[3];
            uint8_t flags = header[4];
            uint32_t length = read_uint24(header + 5);
            uint32_t stream_id = read_uint32(header + 8);

            printf(
                "Frame received: type=%u flags=%u length=%u stream_id=%u\n",
                type,
                flags,
                length,
                stream_id
            );
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}