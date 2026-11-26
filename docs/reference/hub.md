# quectop-hub reference

`quectop-hub` receives probe streams and keeps its data directory as a static website. The site holds the dashboard, a host index, and a copy of each host's files. Any static web server can serve the directory, or the hub can serve it with `--http`.

## Synopsis

```sh
quectop-hub [options]
quectop-hub --add-device NAME [--keys FILE]
```

## Options

| Option | Default | Description |
| --- | --- | --- |
| `--probes IPV4:PORT` | `127.0.0.1:7070` | Address for probe streams. |
| `--http IPV4:PORT` | off | Also serve the data directory over HTTP. See [Built-in HTTP server](#built-in-http-server). |
| `--cors ORIGIN` | off | Send `Access-Control-Allow-Origin: ORIGIN` on `--http` responses. `ORIGIN` is one origin, such as `https://example.com`, or `*`. Requires `--http`. |
| `--keys FILE` | `$HOME/.config/quectop-hub/keys` | Device keys file. See [Keys file](#keys-file). |
| `--add-device NAME` | | Add a key for `NAME` to the keys file, print the key line, and exit. |
| `--data-dir DIR` | `$HOME/.local/state/quectop-hub` | Data directory. Created with missing parents. |
| `--version`, `-v` | | Print `quectop-hub 0.1.0` and exit. |
| `--help` | | Print usage and exit. |

### Exit errors

At startup, the hub exits with status 1 in these cases:

- An option is unknown or has a bad value.
- Another hub holds the lock on the data directory.
- `--probes` is outside `127.0.0.0/8` and the keys file does not exist.
- Other users can read the keys file.

## Keys file

The keys file holds one device per line:

```text
NAME HEX
```

- `NAME` is 1–64 characters of `[A-Za-z0-9_.-]`, and does not start with a dot. It becomes the host ID.
- `HEX` is 64 hex digits: a 256-bit key, used as the Noise pre-shared key.
- A `#` starts a comment. Malformed lines are ignored.

The hub reads the file again on every new connection. An added line works for the next connection, and a deleted line stops the next connection from that device. You do not need to restart the hub.

`--add-device NAME` does these steps:

1. Creates the file with mode `0600` when it is missing, and creates its parent directories.
2. Locks the file.
3. Fails if `NAME` already has a line. To replace a key, delete the line first.
4. Appends `NAME` and a key from `getrandom`.
5. Prints the new line to stdout and a hint to stderr.

### Access rules

The keys file decides which devices may connect:

| Keys file | Who may connect |
| --- | --- |
| Exists | Only listed devices, with their keys. |
| Missing, `--probes` on `127.0.0.0/8` | Any probe that uses the all-zero key. The stream is encrypted, but not authenticated. |
| Missing, `--probes` elsewhere | The hub does not start. |

The hub refuses a keys file that other users can read. Fix it with `chmod 600 FILE`. Rejected devices are logged to stderr.

## Data directory

The data directory is the web root.

| Path | Contents |
| --- | --- |
| `index.html`, `style.css`, `app.js` | The dashboard. The hub installs them at startup when their revision changes. |
| `hosts.txt` | One line for each host: `ID`, a tab, `1` or `0` for connected, a tab, and the host name. |
| `h/ID/` | One host's replica: `1h.v1.bin`, `1d.v1.bin`, `7d.v1.bin`, `30d.v1.bin`, `90d.v1.bin`, `live.v1.bin`, and `disks.v1.bin`. |
| `.*` | Temporary files and the asset revision marker. Do not serve them. |

### Writes

- The hub replaces snapshots and `hosts.txt` atomically.
- It writes history buckets in place, and the dashboard checks each bucket's checksum.
- At startup, `hosts.txt` lists hosts from earlier runs as offline until their probes connect again.

### Asset revision

The revision is the first 16 hex characters of a SHA-256 hash. The hash covers each asset's name, its byte length (little-endian), and its bytes, in the order CSS, JavaScript, HTML. It detects changed assets and is not a signature.

## Built-in HTTP server

`--http` starts a minimal static file server for the data directory.

| Property | Behavior |
| --- | --- |
| Methods | `GET` and `HEAD`. Any other method receives `405 Method Not Allowed`. |
| Connections | One response for each connection, after which the server closes the connection. The request must arrive within 10 s. |
| Security | No authentication and no TLS. Before exposing the server, place it behind a reverse proxy. See [Serve the dashboard with nginx](../how-to/serve-dashboard-with-nginx.md). |

The server maps only these URL paths. Any other path gets `404 Not Found`.

| URL path | File |
| --- | --- |
| `/` | `index.html` |
| `/NAME.EXT` | A top-level file |
| `/h/ID/NAME.EXT` | A host file |

Each path segment is `[A-Za-z0-9_.-]` and does not start with a dot. The server does not follow symbolic links. The query string is ignored.

Every response carries these headers:

```text
Cache-Control: no-cache
X-Content-Type-Options: nosniff
Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'
Referrer-Policy: no-referrer
Connection: close
```

| Extension | `Content-Type` |
| --- | --- |
| `.html` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js` | `text/javascript; charset=utf-8` |
| `.txt` | `text/plain; charset=utf-8` |
| any other | `application/octet-stream` |

## Limits

| Resource | Limit |
| --- | --- |
| Hosts | 64 |
| Probe connections at once | 64 |
| HTTP connections at once | 32 |
| Time from connect to the first decrypted frame | 10 s |
| Time without a frame before a connected device is marked offline | 3 × the device's `--publish-ms` + 60 s |
| History ring file | 256 MiB |

A new stream for a host replaces that host's previous stream.

## Dashboard

The dashboard reads `hosts.txt`, then that host's `live.v1.bin`, `disks.v1.bin`, and the selected history file.

- The dashboard reads `hosts.txt` on every poll. When the file lists more than one host, the dashboard shows a host picker. The URL hash stores the selected host ID.
- While its tab is visible, the dashboard polls every 15 s. It fetches a history file only when the live snapshot reports a new window.
- After a hub upgrade, open dashboards may need to be reloaded.

The [dashboard reference](dashboard.md) describes the dashboard in detail.
