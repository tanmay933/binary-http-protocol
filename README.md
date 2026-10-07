# BHTTP/1 — Custom Binary HTTP Protocol

![C](https://img.shields.io/badge/C-00599C?style=for-the-badge\&logo=c\&logoColor=white)
![TCP](https://img.shields.io/badge/TCP-00758F?style=for-the-badge)
![POSIX%20Sockets](https://img.shields.io/badge/POSIX%20Sockets-6A5ACD?style=for-the-badge)
![Computer%20Networking](https://img.shields.io/badge/Computer%20Networking-8B5CF6?style=for-the-badge)
![Binary%20Protocol](https://img.shields.io/badge/Binary%20Protocol-D32F2F?style=for-the-badge)
![Network%20Architecture](https://img.shields.io/badge/Network%20Architecture-1976D2?style=for-the-badge)

A small custom application-layer protocol built over TCP in C with POSIX sockets. The project focuses on the mechanics of designing a binary wire format and using it reliably across a persistent connection.

> **BHTTP/1 is not HTTP/1.1.** It is a custom HTTP-inspired binary protocol designed to demonstrate application-layer protocol design, message framing, persistent TCP connections, and binary request/response communication.

---

## Overview

The implementation is split into two programs that communicate using the BHTTP/1 wire format:

* **`bserve`** — BHTTP/1 server that serves files from a configured document root.
* **`bcurl`** — BHTTP/1 client that sends requests and decodes responses.

> **Short spec:** [`SPEC.md`](SPEC.md) is a two-page, implementation-independent description of the protocol, including the reasoning behind each header field width. The rest of this README is the long-form documentation. [`hexdump.txt`](hexdump.txt) is an annotated byte-level request and response.

The implementation covers:

* TCP socket programming
* Reliable message framing
* Persistent TCP connections
* Request/response protocols
* Binary protocol design
* Network byte order
* Explicit payload lengths
* Stream ID tracking
* Request and response validation
* Multiple requests over one TCP connection

---

## Architecture

```text
                         Persistent TCP Connection
┌──────────────┐       BHTTP/1 Binary Frames       ┌──────────────┐
│    bcurl     │ <────────────────────────────────> │    bserve   │
│   Client     │                                    │    Server    │
└──────────────┘                                    └──────┬───────┘
                                                           │
                                                           │ File access
                                                           ▼
                                                    ┌──────────────┐
                                                    │     www/     │
                                                    │ Document Root│
                                                    └──────────────┘
```

The client establishes a TCP connection to the server and can perform multiple request/response exchanges over the same connection.

The server resolves requested resources relative to its configured document root.

---

## Main Features

* Fixed **12-byte binary frame header**
* Header fields for magic, version, type, flags, length, and Stream ID
* Persistent TCP connections
* Multiple sequential request/response exchanges
* Stream IDs beginning at `1` and incrementing per request
* Binary `REQUEST` and `RESPONSE` payloads
* HTTP-style status codes: `200`, `400`, `404`, `500`
* `Content-Length` and `Content-Type` response headers
* MIME type detection based on file extension
* Document-root containment and path traversal protection
* Unknown-frame skipping using the `Length` field
* Reliable partial read/write handling through `read_full()` and `write_full()`

---

## Protocol Summary

| Property         | Value                                               |
| ---------------- | --------------------------------------------------- |
| Transport        | TCP                                                 |
| Byte Order       | Big-endian / network byte order                     |
| Frame Header     | 12 bytes                                            |
| Frame Types      | `REQUEST (0x01)`, `RESPONSE (0x02)`, `ERROR (0x03)` |
| Defined Flag     | `END_STREAM (0x01)`                                 |
| Payload Length   | Unsigned 24-bit integer                             |
| Maximum Payload  | 16,777,215 bytes                                    |
| Stream ID        | Unsigned 32-bit                                     |
| Connection Model | Persistent TCP connection                           |
| Supported Method | `GET`                                               |
| Status Codes     | `200`, `400`, `404`, `500`                          |

---

## Web Dashboard

`www/index.html` is a small static dashboard page that gives a visual overview of the protocol (frame layout, request flow). It is only a file for the server to serve. `bserve` speaks BHTTP/1, not HTTP, so a normal browser cannot fetch it from `http://localhost:9000/`. Open `www/index.html` directly as a local file, or fetch it through the protocol client:

```bash
./bcurl localhost:9000/index.html
```

## Running the Project

### 1. Compile the programs

Compile the server:

```bash
cc -std=c11 -Wall -Wextra -O2 -o bserve bserve.c
```

Compile the client:

```bash
cc -std=c11 -Wall -Wextra -O2 -o bcurl bcurl.c
```

### 2. Launch the server

```bash
./bserve ./www 9000
```

Usage is `./bserve [docroot] <port>`. The document root defaults to `./www` (relative to the current directory), so `./bserve 9000` also works when run from the project root.

Example output:

```text
bserve listening on port 9000, docroot=/path/to/www
```

The server handles one TCP connection at a time (it does not serve several clients concurrently).

### 3. Send a request

```bash
./bcurl -v localhost:9000/index.html
```

Usage is `./bcurl [-v] <host:port/path> [more-paths...]`. The older two-argument form `./bcurl -v localhost:9000 /index.html` is also accepted.

`bcurl` prints the response body to stdout, `-v` hexdumps every frame, and the exit status is:

| Exit status | Meaning |
| ----------: | ------- |
| `0` | every response was successful |
| `2` | at least one response was `4xx`/`5xx` (or malformed) |
| `1` | usage error or the connection failed |

### 4. Test a persistent connection

```bash
./bcurl -v localhost:9000/index.html /does-not-exist.html
```

Every path on one command line is sent over the same TCP connection.

---

## Basic Verification

A simple end-to-end check is to request an existing file and then request a missing file on the same connection:

```bash
./bcurl -v localhost:9000 /index.html /does-not-exist.html
```

The first request should produce a successful response, while the second should produce a `404` response. Using both paths in one client invocation also demonstrates that the TCP connection can remain open for multiple exchanges.

The accompanying `hexdump.txt` records a complete binary exchange and can be used to verify the 12-byte frame header and payload lengths manually.

# Protocol Specification

## Transport

BHTTP/1 runs over **TCP**.

The server listens on a configurable TCP port. A client establishes one TCP connection and may send multiple requests over the same connection.

The server keeps the connection open while requests can still be processed.

---

## Byte Order

All multi-byte integer fields use **network byte order (big-endian)**.

---

## Frame Format

Every BHTTP/1 frame consists of a fixed-size **12-byte header** followed by a variable-length payload.

| Field     |    Size |
| --------- | ------: |
| Magic     | 2 bytes |
| Version   |  1 byte |
| Type      |  1 byte |
| Flags     |  1 byte |
| Length    | 3 bytes |
| Stream ID | 4 bytes |

**Total header size: 12 bytes.**

### Frame Layout

```text
┌────────┬─────────┬──────┬───────┬─────────────────┬──────────────────────┐
│ Magic  │ Version │ Type │ Flags │ Length (24-bit) │ Stream ID (32-bit)   │
│ 2 byte │  1 byte │1 byte│1 byte │     3 bytes     │       4 bytes        │
├────────┴─────────┴──────┴───────┴─────────────────┴──────────────────────┤
│                              Payload                                     │
└──────────────────────────────────────────────────────────────────────────┘
```

The 12-byte header is followed immediately by the frame payload.

The `Length` field specifies the number of bytes in the payload and does not include the 12-byte header.

---

## Frame Header

| Offset | Field     |     Size | Description                           |
| -----: | --------- | -------: | ------------------------------------- |
|      0 | Magic     |  2 bytes | Fixed value `0x42 0x48` (`BH`)        |
|      2 | Version   |   1 byte | Protocol version, currently `0x01`    |
|      3 | Type      |   1 byte | Identifies the frame type             |
|      4 | Flags     |   1 byte | Frame-specific flags                  |
|      5 | Length    |  3 bytes | Unsigned 24-bit payload length        |
|      8 | Stream ID |  4 bytes | Identifies the logical request stream |
|     12 | Payload   | Variable | Frame-specific payload                |

### Magic

Every frame begins with:

```text
0x42 0x48
```

These correspond to the ASCII characters `B` and `H`.

A receiver must reject a frame whose magic value is incorrect.

### Version

The current protocol version is:

```text
0x01
```

A receiver must reject a frame using an unsupported version.

### Length

`Length` is an unsigned 24-bit integer stored in network byte order.

It specifies the number of bytes in the payload and does not include the 12-byte frame header.

Maximum payload size:

```text
16,777,215 bytes
```

### Stream ID

Each request/response exchange uses a unique non-zero Stream ID.

* The client starts with Stream ID `1`.
* Each new request on the same TCP connection increments the Stream ID.
* The server uses the same Stream ID in the corresponding `RESPONSE` frame.
* `END_STREAM` indicates that no more frames will be sent for that request/response stream.
* `END_STREAM` does **not** close the underlying TCP connection.

A future version may support multiple simultaneous streams.

---

## Frame Types

| Type       |  Value | Meaning                    |
| ---------- | -----: | -------------------------- |
| `REQUEST`  | `0x01` | Client requests a resource |
| `RESPONSE` | `0x02` | Server returns a response  |
| `ERROR`    | `0x03` | Protocol-level error       |

Values not currently assigned are reserved.

A receiver encountering an unknown frame type uses the `Length` field to skip the frame payload and continue processing subsequent frames.

---

## Flags

Currently defined:

| Flag         |  Value | Meaning                                                  |
| ------------ | -----: | -------------------------------------------------------- |
| `END_STREAM` | `0x01` | Indicates that no more frames will be sent on the stream |

All other flag bits are currently reserved and must be ignored by receivers.

---

## REQUEST Frame

A `REQUEST` frame is sent by the client to request a resource from the server.

The frame type is:

```text
0x01
```

### REQUEST Payload

| Field       |     Size | Description             |
| ----------- | -------: | ----------------------- |
| Method      |   1 byte | Request method          |
| Path Length |  2 bytes | Length of the path      |
| Path        | Variable | Requested resource path |

### Method

BHTTP/1 currently supports only:

```text
0x01 = GET
```

### Path Length

Path Length is an unsigned 16-bit integer in network byte order.

It specifies the number of bytes in the Path field.

### Path

The path is encoded as UTF-8 bytes.

The path must:

* Begin with `/`
* Identify a resource relative to the server document root
* Not allow traversal outside the document root

Examples:

```text
/index.html
/images/logo.png
```

The server rejects paths containing traversal components such as `../`, or paths that resolve outside the configured document root.

### Example REQUEST

A request for:

```text
/index.html
```

has the following payload:

```text
01
00 0B
2F 69 6E 64 65 78 2E 68 74 6D 6C
```

Where:

```text
01       = GET
00 0B    = Path Length = 11
2F ...   = "/index.html"
```

The complete frame consists of the 12-byte BHTTP/1 header followed by this payload.

---

## RESPONSE Frame

A `RESPONSE` frame is sent by the server after receiving a valid `REQUEST` frame.

The RESPONSE payload contains:

| Field        |     Size | Description                |
| ------------ | -------: | -------------------------- |
| Status Code  |  2 bytes | HTTP-style numeric status  |
| Header Count |   1 byte | Number of response headers |
| Headers      | Variable | Encoded response headers   |
| Body         | Variable | Resource contents          |

### Status Codes

| Status | Meaning                      |
| -----: | ---------------------------- |
|  `200` | Request successful           |
|  `400` | Malformed or invalid request |
|  `404` | Requested resource not found |
|  `500` | Internal server error        |

### Response Header Encoding

Each response header is encoded as:

| Field        |    Size | Description                |
| ------------ | ------: | -------------------------- |
| Header ID    |  1 byte | Identifies the header name |
| Value Length | 2 bytes | Length of the header value |
| Value        | N bytes | Header value bytes         |

All multi-byte values use network byte order.

Defined header IDs:

|     ID | Header         |
| -----: | -------------- |
| `0x01` | Content-Length |
| `0x02` | Content-Type   |

For a successful file response, the server sends:

* `Content-Length` — number of bytes in the response body
* `Content-Type` — MIME type of the requested file

The `Header Count` field specifies the number of encoded headers.

The body follows all encoded headers and contains the requested file contents.

---

## ERROR Frame

The `ERROR` frame is reserved for protocol-level errors that prevent normal request/response processing, such as malformed frame headers or unsupported protocol versions.

The frame type is:

```text
0x03
```

In the current implementation, client-request-related failures are reported using `RESPONSE` frames with status codes:

* `400` — malformed or invalid request
* `404` — missing file
* `500` — internal server error

The `ERROR` frame remains part of the protocol for future protocol-level signaling.

---

## MIME Types

The server determines the `Content-Type` response header from the requested file extension.

| Extension | MIME Type                |
| --------- | ------------------------ |
| `.html`   | `text/html`              |
| `.htm`    | `text/html`              |
| `.txt`    | `text/plain`             |
| `.css`    | `text/css`               |
| `.js`     | `application/javascript` |
| `.json`   | `application/json`       |
| `.png`    | `image/png`              |
| `.jpg`    | `image/jpeg`             |
| `.jpeg`   | `image/jpeg`             |
| `.gif`    | `image/gif`              |

Files with an unsupported extension use:

```text
application/octet-stream
```

For successful file responses, the server includes both `Content-Length` and `Content-Type`.

---

## Connection Lifecycle

BHTTP/1 uses a persistent TCP connection.

```text
Client                         Server
  |                              |
  |------ TCP connection ------->|
  |                              |
  |------ REQUEST, Stream 1 ---->|
  |<----- RESPONSE, Stream 1 ----|
  |                              |
  |------ REQUEST, Stream 2 ---->|
  |<----- RESPONSE, Stream 2 ----|
  |                              |
  |------ REQUEST, Stream 3 ---->|
  |<----- RESPONSE, Stream 3 ----|
  |                              |
  |---------- TCP close -------->|
```

The TCP connection remains open between requests.

For example:

```text
Stream 1 -> /index.html
Stream 2 -> /does-not-exist.html
Stream 3 -> /hello.txt
```

The server uses the same Stream ID in the corresponding response.

The `END_STREAM` flag indicates that the logical request/response stream has finished. It does not terminate the TCP connection.

---

## Security and Path Handling

The server treats the configured document root as the boundary for all requested resources.

Before serving a file, the server checks that:

* The path begins with `/`.
* Path traversal components such as `..` are rejected.
* The resolved path remains inside the configured document root.
* The requested path refers to a regular file.
* Files outside the document root are never served.

Valid example:

```text
/index.html
```

Rejected traversal examples:

```text
/../secret.txt
/foo/../../secret.txt
```

These requests result in:

```text
400 Bad Request
```

This prevents a client from using path traversal to access files outside the server's configured document root.

---

## Error Handling

BHTTP/1 uses HTTP-style status codes inside `RESPONSE` frames for errors associated with a client request.

| Status | Meaning               | Example                           |
| -----: | --------------------- | --------------------------------- |
|  `200` | Success               | Requested file exists             |
|  `400` | Bad Request           | Invalid request or path traversal |
|  `404` | Not Found             | Requested file does not exist     |
|  `500` | Internal Server Error | Unexpected server-side failure    |

Example 404 response:

```text
Status Code = 404
Header Count = 1
Content-Length = 0
```

The response uses the same Stream ID as the corresponding request.

Protocol-level errors that cannot be represented as normal request responses are reserved for the `ERROR` frame type.

---

## Unknown Frames

The `Length` field allows implementations to skip frames they do not understand.

If a receiver encounters an unknown frame type, it:

1. Reads the frame header.
2. Uses the `Length` field to determine the payload size.
3. Skips that payload.
4. Continues processing the connection.

This allows future versions of BHTTP/1 to introduce additional frame types without requiring older implementations to terminate the TCP connection.

---

# Example Exchange

A complete request for `/index.html` uses Stream ID `1`.

### Request

```text
Frame Type: REQUEST
Stream ID: 1
Flags: END_STREAM

Method: GET
Path: /index.html
```

Request payload:

```text
01
00 0B
2F 69 6E 64 65 78 2E 68 74 6D 6C
```

Where:

```text
01       = GET
00 0B    = Path Length = 11
2F ...   = /index.html
```

### Response

```text
Frame Type: RESPONSE
Stream ID: 1
Flags: END_STREAM

Status Code: 200
Header Count: 2

Content-Length: 7697
Content-Type: text/html

Body:
contents of www/index.html
```

The response Stream ID matches the request Stream ID.

A complete annotated byte-level example is provided in `hexdump.txt`. That example uses the small `www/hex-example.txt` fixture so the full frame remains readable; it is not a dump of the larger dashboard page.

---

# Persistent Connection Example

The client can request multiple resources sequentially using one TCP connection:

```bash
./bcurl -v localhost:9000 /index.html /does-not-exist.html
```

The client sends:

```text
Stream 1 -> GET /index.html
Stream 2 -> GET /does-not-exist.html
```

The server responds:

```text
Stream 1 -> 200
Stream 2 -> 404
```

Both exchanges occur over the same TCP connection while maintaining different Stream IDs. The reference server handles one TCP connection at a time; this project does not claim concurrent client handling.

---

# Testing

The implementation was tested using the BHTTP/1 client and server.

## Successful Request

```bash
./bcurl -v localhost:9000 /index.html
```

Expected result:

```text
Status: 200
Content-Length: 7697
Content-Type: text/html
```

The response body contains the contents of `www/index.html`.

## Missing Resource

```bash
./bcurl -v localhost:9000 /does-not-exist.html
```

Expected result:

```text
Status: 404
```

The server remains available for subsequent requests.

## Path Traversal Protection

```bash
./bcurl -v localhost:9000 /../secret.txt
```

Expected result:

```text
Status: 400
```

Another traversal attempt:

```bash
./bcurl -v localhost:9000 /foo/../../secret.txt
```

Expected result:

```text
Status: 400
```

## Persistent Connection

```bash
./bcurl -v localhost:9000 /index.html /does-not-exist.html
```

Expected result:

```text
=== Stream 1: /index.html ===
Status: 200

=== Stream 2: /does-not-exist.html ===
Status: 404
```

This verifies that multiple request/response exchanges use the same underlying TCP connection while maintaining different Stream IDs.

### Test Results Summary

| Test Case                                   |  Expected Status | Result |
| ------------------------------------------- | ---------------: | ------ |
| Existing file (`/index.html`)               |            `200` | PASS ✓ |
| Missing file (`/does-not-exist.html`)       |            `404` | PASS ✓ |
| Traversal attempt (`/../secret.txt`)        |            `400` | PASS ✓ |
| Traversal attempt (`/foo/../../secret.txt`) |            `400` | PASS ✓ |
| Two requests, one TCP connection            | `200` then `404` | PASS ✓ |
| `bcurl` exit status on `200`                |              `0` | PASS ✓ |
| `bcurl` exit status on `404` / `400`        |              `2` | PASS ✓ |

---

# Project Structure

```text
binary-http-protocol/
├── bserve.c
├── bcurl.c
├── SPEC.md
├── README.md
├── hexdump.txt
├── LICENSE
└── www/
    ├── index.html
    └── hex-example.txt
```

## Components

### `bserve.c`

BHTTP/1 server implementation responsible for:

* TCP socket creation
* Accepting client connections
* Reading BHTTP/1 frames
* Validating frame headers
* Decoding `REQUEST` frames
* Validating requested paths
* Reading files
* Determining MIME types
* Constructing `RESPONSE` frames
* Returning status codes
* Maintaining persistent TCP connections

### `bcurl.c`

BHTTP/1 client implementation responsible for:

* Establishing the TCP connection
* Constructing `REQUEST` frames
* Assigning Stream IDs
* Sending multiple requests over one connection
* Receiving `RESPONSE` frames
* Decoding response status and headers
* Displaying response bodies
* Verbose frame/hexdump output

### `www/`

Server document root containing resources that can be requested by clients.

### `hexdump.txt`

Contains an annotated example of the BHTTP/1 binary wire format.

---

# Design Summary

BHTTP/1 demonstrates the construction of a custom application-layer protocol over TCP.

The implementation combines:

* Fixed-size binary framing
* Explicit payload lengths
* Network byte order
* Request and response frame types
* Stream ID tracking
* Persistent TCP connections
* Binary response headers
* HTTP-style status codes
* MIME type handling
* Document-root path protection
* Unknown-frame skipping
* Reliable partial read/write handling

Together, `bserve` and `bcurl` provide an end-to-end client/server system capable of requesting and serving files using the custom BHTTP/1 binary protocol over a persistent TCP connection.
