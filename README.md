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

## 5.4 Stream ID

### Stream ID

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

- `200` — requested resource was found
- `400` — malformed or invalid request
- `404` — requested resource was not found
- `500` — internal server error