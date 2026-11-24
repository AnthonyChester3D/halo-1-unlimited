# Send logs: players sending their logs to the developers

A player who hits a problem picks **Send logs** (the installer's *More...*; from the game, OPTIONS > INSTALLER opens the installer, v1.181). It says what's
sent and asks first, every time: nothing is ever sent otherwise. The logs go, packed, to a small Cloudflare Worker that
keeps them 30 days, and the player gets a code (`X5EB-3RLU`) to quote. The developers fetch them with
`port/oxbox/tools/fetch_logs.py`. No one has to copy files off an Xbox by FTP.

## What's sent

From Halo 1 Unlimited's folder: `oxbox_log.txt` and `oxbox_log_previous.txt` (the boot before: the game keeps it, since
after a crash that's the one that tells; their last 3 MB), `opence.ini` (any `password` line's value blanked),
`opence_release.txt`, `opence_showcase.txt`, `stabbed.txt`, `debug.txt` (its last 1 MB), and `perf\`'s files (up to 24,
1 MB each). From the installer's own folder: `installer_log.txt`. All of it at most 6 MB packed. No saves, profiles or
maps. The logs do hold the maps played, profile names and network addresses, and the player is told so.

## The bundle (OXLG, version 1)

Little-endian. A 0x40-byte header:

| Offset | Size | Value |
|---|---|---|
| 0x00 | 4 | `OXLG` |
| 0x04 | 4 | 1 |
| 0x08 | 8 | the code: 8 of `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` |
| 0x10 | 4 | who sent it: 1 the game, 2 the installer |
| 0x14 | 4 | the game's build (v1.NN's NN; 0 if not known) |
| 0x18 | 4 | the installer's version (0 from the game) |
| 0x1C | 4 | file count (at most 64) |
| 0x20 | 4 | the bundle's size |
| 0x24 | 4 | CRC32 (zlib's) of everything after the header |
| 0x28 | 24 | zeros |

Then each file: its name (64 bytes, zero-padded, `/` between folders: `H1U/perf/sweep_p4.csv`), its whole size, where the
part kept starts (0, or its last part's offset), how many bytes are kept, how many they pack to (four u32s), then the
packed bytes (one zlib stream).

## The Worker

`port/oxbox/tools/logs_worker.js`: `POST /v1/logs` takes a bundle (at most 8 MB, checked: header, size, CRC32) and keeps
it in a KV namespace for 30 days under `<UTC time>-<code>` (`20261123T155033-X5EB3RLU`), with its header's fields and the
sender's country (Cloudflare's guess), not its address; it answers `ok <code>`. `GET /v1/logs` (the list), `GET` and
`DELETE /v1/logs/<name>` need `Authorization: Bearer <READ_KEY>`. Tested with wrangler's local runtime and from xemu's
installer (installer 66).

The installer sends to `LOGS_DEFAULT_URL` (installer/logs.h: `https://h1u-logs.h1u.workers.dev/v1/logs` from installer
67), or developer.ini's `logs_url` (lab: wrangler dev at `http://10.0.2.2:8787/v1/logs` from xemu). With neither,
*Send logs* isn't shown. Tested over the internet from xemu (installer 67): sent, kept, fetched back by
`fetch_logs.py`. Like the installer's downloads, the
connection is HTTPS but the server's certificate isn't checked (the installer has no certificate authorities or clock to
check it with).

## Setting it up (once, in Cloudflare's dashboard)

A free Cloudflare account; no card needed (KV is part of the free Workers plan: 1 GB, 1,000 uploads a day). There's no
inactivity limit: it stays up whether or not anything is sent.

1. Sign up at dash.cloudflare.com (the Free plan) and confirm the email.
2. **Workers & Pages**: the first time, pick your `workers.dev` subdomain (for example `h1u`). Then **Create** ->
   **Create Worker**, name it `h1u-logs`, **Deploy** (the sample code; it's replaced next).
3. **Storage & Databases** -> **Workers KV** -> **Create** a namespace named `h1u-logs`.
4. Open the `h1u-logs` Worker -> **Edit code**, replace everything with `port/oxbox/tools/logs_worker.js`, **Deploy**.
5. The Worker's **Settings** -> **Bindings** -> **Add** -> **KV namespace**: variable name `LOGS`, namespace `h1u-logs`.
6. **Settings** -> **Variables and Secrets** -> **Add**: type **Secret**, name `READ_KEY`, value: the read key (the
   `key = ` line of `~/.config/h1u/logs.conf` on the laptop that fetches them). **Deploy**.
7. Its address (`https://h1u-logs.<subdomain>.workers.dev`) goes in `~/.config/h1u/logs.conf` (`url = `) and in
   `LOGS_DEFAULT_URL` with `/v1/logs` after it.

Then `fetch_logs.py list` should answer (an empty list), and a test upload from an Xbox confirms the rest.

## Fetching them

`fetch_logs.py list`; `fetch_logs.py fetch` (each new bundle into `~/h1u-lab/uploads/<name>/`, unpacked, with
`about.txt`); `fetch_logs.py delete NAME`. `fetch_logs.py pack` makes a bundle as a console does, for testing.
