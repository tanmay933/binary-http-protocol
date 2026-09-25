# BHTTP/1 — Binary HTTP Protocol

## 1. Overview

BHTTP/1 is a custom binary application-layer protocol built over TCP.

It is designed to transfer files from a server to a client using persistent TCP connections.

The project consists of:

- `bserve` — BHTTP/1 server
- `bcurl` — BHTTP/1 client

The server serves files from a specified document root.

The client requests files using the BHTTP/1 protocol.

## 2. Transport

BHTTP/1 uses TCP.

The server listens on a configurable TCP port.

A client establishes one TCP connection and may send multiple requests over the same connection.

The server keeps the connection open while requests can still be processed.

## 3. Byte Order

All multi-byte integer fields use network byte order (big-endian).

## 4. Frame Format

Every BHTTP/1 frame consists of a fixed-size 12-byte header followed by a variable-length payload.

| Field | Size |
|---|---:|
| Magic | 2 bytes |
| Version | 1 byte |
| Type | 1 byte |
| Flags | 1 byte |
| Length | 3 bytes |
| Stream ID | 4 bytes |

Total header size: **12 bytes**.

# 5. Frame Header

The 12-byte BHTTP/1 frame header is encoded as follows:

| Offset | Field | Size | Description |
|---:|---|---:|---|
| 0 | Magic | 2 bytes | Fixed value `0x42 0x48` (`BH`) |
| 2 | Version | 1 byte | Protocol version, currently `0x01` |
| 3 | Type | 1 byte | Identifies the frame type |
| 4 | Flags | 1 byte | Frame-specific flags |
| 5 | Length | 3 bytes | Payload length, unsigned 24-bit integer |
| 8 | Stream ID | 4 bytes | Identifies the logical request stream |
| 12 | Payload | variable | Frame-specific payload |

## 5.1 Magic

Every frame begins with two magic bytes:

```text
0x42 0x48
```

These correspond to the ASCII characters `B` and `H`.

A receiver MUST reject a frame whose magic value is incorrect.

## 5.2 Version

The current protocol version is:

```text
0x01
```

A receiver MUST reject a frame using an unsupported version.

## 5.3 Length

Length is an unsigned 24-bit integer stored in network byte order.

It specifies the number of bytes in the payload and does not include the 12-byte frame header.

The maximum payload size is:

```text
16,777,215 bytes
```

### 5.4 Stream ID

Each request/response exchange uses a unique non-zero Stream ID.

- The client starts with Stream ID `1`.
- Each new request on the same TCP connection increments the Stream ID.
- The server uses the same Stream ID in the corresponding RESPONSE frame.
- `END_STREAM` means that no more frames will be sent for that request/response stream.
- `END_STREAM` does NOT close the underlying TCP connection.

A future version may support multiple simultaneous streams.

# 6. Frame Types

| Type | Value | Meaning |
|---|---:|---|
| REQUEST | `0x01` | Client requests a resource |
| RESPONSE | `0x02` | Server returns a response |
| ERROR | `0x03` | Protocol-level error |

Values not currently assigned are reserved.

A receiver encountering an unknown frame type MUST use the Length field to skip the frame payload and continue processing subsequent frames.

# 7. Flags

Currently defined:

| Flag | Value | Meaning |
|---|---:|---|
| END_STREAM | `0x01` | Indicates that no more frames will be sent on the stream |

All other flag bits are currently reserved and MUST be ignored by receivers.

# 8. REQUEST Frame

A REQUEST frame is sent by the client to request a resource from the server.

The frame type MUST be:

```text
0x01
```

The REQUEST payload has the following format:

| Field | Size | Description |
|---|---|---|
| Method | 1 byte | Request method |
| Path Length | 2 bytes | Length of the path |
| Path | Variable | Requested resource path |

## 8.1 Method

BHTTP/1 currently supports only:

```text
0x01 = GET
```

## 8.2 Path Length

Path Length is an unsigned 16-bit integer in network byte order.

It specifies the number of bytes in the Path field.

## 8.3 Path

The path is encoded as UTF-8 bytes.

The path MUST:

- begin with `/`
- identify a resource relative to the server document root
- NOT allow traversal outside the document root

For example:

```text
/index.html
/images/logo.png
```

The server MUST reject paths containing traversal components such as:

```text
../
```

or paths that resolve outside the configured document root.

## 8.4 Example

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
00 0B    = path length (11)
2F ...   = "/index.html"
```

The complete frame consists of the 12-byte BHTTP/1 header followed by this payload.

## 9. RESPONSE Frame

A RESPONSE frame is sent by the server after receiving a valid REQUEST frame.

The RESPONSE payload begins with:

| Field | Size | Description |
|---|---:|---|
| Status Code | 2 bytes | HTTP-style numeric status |
| Header Count | 1 byte | Number of response headers |
| Headers | Variable | Encoded response headers |
| Body | Variable | Resource contents |

Supported status codes:

- `200` — Request successful
- `400` — Malformed or invalid request
- `404` — Requested resource not found
- `500` — Internal server error

### 9.1 Response Header Encoding

Each response header is encoded as:

| Field | Size | Description |
|---|---:|---|
| Header ID | 1 byte | Identifies the header name |
| Value Length | 2 bytes | Length of the header value |
| Value | N bytes | Header value bytes |

All multi-byte values use network byte order (big-endian).

Defined header IDs:

| ID | Header |
|---|---|
| `0x01` | Content-Length |
| `0x02` | Content-Type |

For a successful file response, the server sends:

- `Content-Length`: number of bytes in the response body
- `Content-Type`: MIME type of the requested file

The Header Count field specifies the number of encoded headers.

The Body follows all encoded headers and contains the requested file contents.

# 10. ERROR Frame

An ERROR frame is reserved for protocol-level errors that prevent normal
request/response processing.

The frame type is:

```text
0x03

# 11. MIME Types

The server determines the Content-Type response header from the requested
file extension.

Supported MIME types are:

| Extension | MIME Type |
|---|---|
| `.html` | `text/html` |
| `.htm` | `text/html` |
| `.txt` | `text/plain` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.png` | `image/png` |
| `.jpg` | `image/jpeg` |
| `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |

Files with an unsupported extension use:

```text
application/octet-stream
```

For successful file responses, the server includes both:

```text
Content-Length
Content-Type
```

in the RESPONSE payload.

# 12. Connection Lifecycle

BHTTP/1 uses a persistent TCP connection.

The connection lifecycle is:

```text
Client                         Server
  |                              |
  |------ TCP connection ------->|
  |                              |
  |------ REQUEST, Stream 1 ----->|
  |<----- RESPONSE, Stream 1 ----|
  |                              |
  |------ REQUEST, Stream 2 ----->|
  |<----- RESPONSE, Stream 2 ----|
  |                              |
  |------ REQUEST, Stream 3 ----->|
  |<----- RESPONSE, Stream 3 ----|
  |                              |
  |---------- TCP close --------->|
```

The TCP connection remains open between requests.

Each request receives a unique Stream ID.

For example:

```text
Stream 1 -> /index.html
Stream 2 -> /does-not-exist.html
Stream 3 -> /hello.txt
```

The server uses the same Stream ID in the corresponding response.

The END_STREAM flag indicates that the logical request/response stream has
finished. It does not terminate the TCP connection.

# 13. Security and Path Handling

The server treats the configured document root as the boundary for all
requested resources.

A requested path is converted into a filesystem path relative to the
document root.

The server performs multiple checks before serving a file:

- The path must begin with `/`.
- Path traversal components such as `..` are rejected.
- The resolved path must remain inside the configured document root.
- The requested path must refer to a regular file.
- Files outside the document root are never served.

For example:

```text
/index.html
```

is valid.

The following requests are rejected:

```text
/../secret.txt
/foo/../../secret.txt
```

These requests result in:

```text
400 Bad Request
```

This prevents a client from using path traversal to access files outside the
server's configured document root.

# 14. Error Handling

BHTTP/1 uses HTTP-style status codes inside RESPONSE frames for errors that
are associated with a client request.

| Status | Meaning | Example |
|---|---|---|
| 200 | Success | Requested file exists |
| 400 | Bad Request | Invalid request or path traversal |
| 404 | Not Found | Requested file does not exist |
| 500 | Internal Server Error | Unexpected server-side failure |

Example 404 response:

```text
Status Code = 404
Header Count = 1
Content-Length = 0
```

The response uses the same Stream ID as the corresponding request.

Protocol-level errors that cannot be represented as normal request responses
are reserved for the ERROR frame type.

# 15. Unknown Frames

The Length field allows implementations to skip frames they do not understand.

If a receiver encounters an unknown frame type, it reads the frame header,
uses the Length field to determine the payload size, skips that payload, and
continues processing the connection.

This allows future versions of BHTTP/1 to introduce additional frame types
without requiring older implementations to terminate the TCP connection.

# 16. Example Exchange

A complete request for `/index.html` uses Stream ID 1.

## Request

```text
Frame Type: REQUEST
Stream ID: 1
Flags: END_STREAM

Method: GET
Path: /index.html
```

The request payload is:

```text
01
00 0B
2F 69 6E 64 65 78 2E 68 74 6D 6C
```

where:

```text
01       = GET
00 0B    = Path Length = 11
2F ...   = /index.html
```

## Response

```text
Frame Type: RESPONSE
Stream ID: 1
Flags: END_STREAM

Status Code: 200
Header Count: 2

Content-Length: 197
Content-Type: text/html

Body:
contents of www/index.html
```

The response Stream ID matches the request Stream ID.

A complete annotated byte-level example is provided in:

```text
hexdump.txt
```

# 17. Persistent Connection Example

The client can request multiple resources using one TCP connection.

Example:

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

Both exchanges occur over the same TCP connection.

This demonstrates the persistent connection behavior of BHTTP/1.

# 18. Testing

The implementation was tested using the BHTTP/1 client and server.

## 18.1 Successful Request

```bash
./bcurl -v localhost:9000 /index.html
```

Expected result:

```text
Status: 200
Content-Length: 197
Content-Type: text/html
```

The response body contains the contents of:

```text
www/index.html
```

## 18.2 Missing Resource

```bash
./bcurl -v localhost:9000 /does-not-exist.html
```

Expected result:

```text
Status: 404
```

The server remains available for subsequent requests.

## 18.3 Path Traversal Protection

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

## 18.4 Persistent Connection

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

This test verifies that multiple request/response exchanges use the same
underlying TCP connection while maintaining different Stream IDs.

# 19. Building the Project

Compile the server:

```bash
gcc -Wall -Wextra -O2 -o bserve bserve.c
```

Compile the client:

```bash
gcc -Wall -Wextra -O2 -o bcurl bcurl.c
```

# 20. Running the Server

Start the server on port 9000:

```bash
./bserve 9000
```

The server uses the `www/` directory as its document root.

Example:

```text
bserve listening on port 9000
```

# 21. Running the Client

Request a single file:

```bash
./bcurl localhost:9000 /index.html
```

Enable verbose frame output:

```bash
./bcurl -v localhost:9000 /index.html
```

Request multiple files over the same TCP connection:

```bash
./bcurl -v localhost:9000 /index.html /does-not-exist.html
```

# 22. Project Structure

```text
binary-http-protocol/
├── bserve.c
├── bcurl.c
├── README.md
├── hexdump.txt
└── www/
    └── index.html
```

## Components

### bserve.c

BHTTP/1 server implementation.

Responsibilities include:

- TCP socket creation
- accepting client connections
- reading BHTTP/1 frames
- validating frame headers
- decoding REQUEST frames
- validating requested paths
- reading files
- determining MIME types
- constructing RESPONSE frames
- returning status codes
- maintaining persistent TCP connections

### bcurl.c

BHTTP/1 client implementation.

Responsibilities include:

- establishing the TCP connection
- constructing REQUEST frames
- assigning Stream IDs
- sending multiple requests over one connection
- receiving RESPONSE frames
- decoding response status and headers
- displaying response bodies
- verbose frame/hexdump output

### www/

Server document root containing resources that can be requested by clients.

### hexdump.txt

Contains an annotated example of the BHTTP/1 binary wire format.

# 23. Design Summary

BHTTP/1 demonstrates the construction of a custom application-layer protocol
over TCP.

The protocol defines:

- a fixed binary frame header
- explicit payload lengths
- network byte order
- request and response frame types
- Stream IDs
- persistent TCP connections
- binary response headers
- HTTP-style status codes
- MIME type handling
- path traversal protection
- extensibility through unknown-frame skipping

The implementation provides an end-to-end client/server system capable of
requesting and serving files using the custom BHTTP/1 binary protocol.