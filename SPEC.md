# BHTTP/1 — Specification (two-page version)

BHTTP/1 is a binary, HTTP-inspired request/response protocol that runs over one persistent TCP connection. This page is enough to write a compatible client or server without reading any source. A byte-level example is in `hexdump.txt`.

All multi-byte integers are **big-endian**. "Must" means a conforming peer has to do it.

## 1. Frame

Every message is one frame: a fixed **12-byte header**, then `Length` bytes of payload.

```text
 0      2      3      4      5          8              12
 +------+------+------+------+----------+--------------+---------...
 |Magic |Ver   |Type  |Flags |Length    |Stream ID     | Payload
 |"BH"  |0x01  |1 B   |1 B   |24-bit    |32-bit        | Length bytes
 +------+------+------+------+----------+--------------+---------...
```

| Field | Bytes | Rules |
|---|---:|---|
| Magic | 2 | `0x42 0x48` ("BH"). A receiver must close the connection on any other value. |
| Version | 1 | `0x01`. Any other value: close the connection. |
| Type | 1 | `0x01` REQUEST, `0x02` RESPONSE, `0x03` ERROR (reserved). |
| Flags | 1 | `0x01` END_STREAM. All other bits reserved: senders set 0, receivers ignore. |
| Length | 3 | Payload size in bytes, not counting the header. Max 16,777,215. |
| Stream ID | 4 | Non-zero. Client starts at 1 and adds 1 per request. A RESPONSE echoes its request's ID. |

**Unknown frame types must be skipped:** read the 12-byte header, read and discard `Length` bytes, carry on. The connection stays open. This is what lets a version 2 add frame types without breaking a version 1 peer.

## 2. REQUEST (type 0x01)

| Field | Bytes | Meaning |
|---|---:|---|
| Method | 1 | `0x01` = GET (the only method) |
| Path Length | 2 | Number of path bytes |
| Path | N | UTF-8, starts with `/`, no NUL bytes |

`Length` must equal `3 + Path Length`, otherwise the server answers 400. The client sets END_STREAM (a GET has no body).

## 3. RESPONSE (type 0x02)

| Field | Bytes | Meaning |
|---|---:|---|
| Status | 2 | `200`, `400`, `404`, `500` |
| Header Count | 1 | Number of headers that follow |
| Headers | var. | Each: ID (1) + Value Length (2) + Value |
| Body | var. | Everything after the last header, to the end of the payload |

Header IDs: `0x01` Content-Length (decimal ASCII), `0x02` Content-Type. A 200 carries both; 400/404/500 carry no headers and no body. A receiver ignores header IDs it does not know (it still knows how many bytes to skip from the value length). The server sets END_STREAM on every RESPONSE.

## 4. Behaviour

- **One connection, many requests.** The client sends a REQUEST, waits for the matching RESPONSE, then may send the next one. END_STREAM ends the stream, not the TCP connection. The client closes the connection when it is done.
- **Statuses.** 200 file sent; 404 no such regular file; 400 malformed request, wrong method, bad path, or `..` escaping the document root; 500 server failure or file too large for one frame.
- **Path safety.** The server resolves `.`/`..` lexically, rejects any path that climbs above the document root (400), then resolves symlinks and refuses anything that lands outside the root (404).
- **Bad framing.** Wrong magic or version means the byte stream can no longer be trusted, so the server closes the connection.
- **MIME types** come from the file extension (`.html`, `.txt`, `.css`, `.js`, `.json`, `.png`, `.jpg`, `.jpeg`, `.gif`); anything else is `application/octet-stream`.

## 5. Why these widths

HTTP/2 uses a 9-byte header split 24 / 8 / 8 / 1+31 (length, type, flags, reserved bit + stream ID). BHTTP/1 keeps that shape and adds magic and version, so the header is 12 bytes.

| Field | Width | Reasoning |
|---|---|---|
| Magic | 2 B | HTTP/2 can skip it because the connection preface and ALPN already identify the protocol. We have neither, so a stray HTTP client or a desynchronised stream is caught on the first frame. 2 bytes gives a 1 in 65,536 chance of an accidental match, for 2 bytes of cost. |
| Version | 1 B | 256 versions is far more than needed. A byte is the smallest unit that can be read on its own. |
| Type | 1 B | 256 types leaves plenty of room for new frames. Because unknown types are skipped, adding one is not a breaking change. |
| Flags | 1 B | One bit is used today. Seven spare bits cost nothing, and "ignore unknown bits" makes new flags safe to add. |
| Length | 3 B (24-bit) | 16 bits (64 KiB) is too small for a normal web page or image. 32 bits wastes a byte and lets one hostile header ask for a 4 GiB allocation. 24 bits allows 16 MiB, which fits every file this server sends in a single frame, and it is the same width HTTP/2 chose. |
| Stream ID | 4 B (32-bit) | 1 byte would wrap after 255 requests on a long-lived connection. 4 bytes gives about 4 billion and leaves room for multiplexing later. Unlike HTTP/2 we use all 32 bits, because we have no reserved bit. |
| Header total | 12 B | A multiple of 4, so the 32-bit field sits on a natural boundary. The code still decodes byte by byte, so alignment never matters for correctness. |
| Path Length | 2 B | A 16-bit length covers any realistic URL (up to 65,535 bytes) and keeps the request tiny. |
| Header ID | 1 B | The only two header names we send are numbered (`1`, `2`) instead of spelled out, the same idea as HPACK's static table. Saves 12 and 14 bytes per response. 254 IDs remain free. |
| Header Value Length | 2 B | Values are short strings; 16 bits is plenty and matches the path length. |
| Status | 2 B | Holds any 100–599 code with room to spare. |

## 6. Command-line tools

```text
./bserve [docroot] <port>          e.g. ./bserve ./www 9000   (docroot defaults to ./www)
./bcurl [-v] host:port/path ...    e.g. ./bcurl -v localhost:9000/index.html
./bcurl [-v] host:port path ...    (older form, still accepted)
```

`bcurl` writes the body to stdout, `-v` hexdumps every frame, and it exits `0` on success, `2` if any response was 4xx/5xx, and `1` on a usage or connection error. Several paths on one command line share one TCP connection.
