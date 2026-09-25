# BookOrbit integration (phase 1)

Browse a BookOrbit library from the CrossPoint Library screen, download EPUBs on open, and keep the reading position in step with BookOrbit. This document describes what is implemented, how it behaves on the device, its on-card layout, and what remains open. The design it implements is `docs/bookorbit-integration-plan.md`.

## Setup

1. In BookOrbit, create KOReader device credentials for your account (Settings → KOReader sync). BookOrbit's device API uses those credentials; the firmware never registers an account by itself.
2. On the device: **Settings → System → BookOrbit**.
   - **Server URL**: the BookOrbit origin, e.g. `https://books.example.com` (a path prefix such as `https://host/bookorbit` is kept; a pasted `/api/v1` suffix is stripped).
   - **Username** / **Password**: the KOReader device credentials.
   - **Test connection** connects to Wi-Fi and calls `GET /api/v1/koreader/users/auth`.
   - **Sync library now** runs a full network session (see below); both it and Test connection reboot back into this screen.
3. Open **Library**. The three sort tabs now show a **BookOrbit** section with a **Refresh** button in the header; the new **On device** tab shows what can be opened offline.

### TLS trust

- `https://` servers are verified against **ISRG Root X1** (Let's Encrypt) built into the firmware, or against `/.crosspoint/bookorbit-ca.pem` when that file exists on the card (a single PEM root for self-hosted CAs, at most 8 KiB).
- **Skip certificate check** disables verification for a private CA you cannot provision; it is off by default and logged whenever it is used.
- Verification needs a valid clock. The session seeds the process clock from the RTC or, on boards without one, from SNTP before the first handshake. Without either, verified TLS fails with "Cannot verify the server certificate".
- Known limitation: the SDK's `SecureClient` verifies the certificate chain but not the host name (no `wolfSSL_check_domain_name`), so a valid certificate for a different host would be accepted. This lives in `freeink-sdk` and is not changed here.
- `http://` servers work without TLS for LAN deployments.
- The firmware's wolfSSL build now enables SHA-384/512 (`scripts/patch_wolfssl.py`): Let's Encrypt's ECDSA intermediates (E5/E6) sign leaf certificates with `ecdsa-with-SHA384`, which the previous SHA-256-only build could not verify. When a handshake fails, the client probes plain TCP first so the message distinguishes an unreachable host ("Cannot reach the server") from a failed handshake ("Secure connection failed: check the certificate").

## Library behaviour

Toolbar icons are Lucide **eye** (Recent), **type** (Title), **user-round** (Author), **library-big** (Series), and **smartphone** (On device). Author and Series group lists use list mode; books inside them respect the list/grid setting.

| Where | What you see |
| --- | --- |
| Recent / Title tabs | **BookOrbit** section heading (with the last sync time, or why the cache is empty), then the cached catalogue in the tab's order. **Only on this device** section: the existing CLIX index (sideloaded files). |
| Author tab | Authors and book counts, in separate BookOrbit and local sections. Open one to see only that group’s books; Back returns to the selected author. Hold a BookOrbit author to confirm downloading their missing books. |
| Series tab | Every series in the catalogue (name, first author, book count, first book's cover). Open one for its books in series order; Back (or the tab) returns to the list. Hold a series to queue every book of it that is not on the card. Sideloaded files carry no series data, so the tab shows the catalogue only. |
| On device tab | Downloaded BookOrbit books plus sideloaded files, by title (hold the tab to flip direction), and every book in the download queue (badged **Queued**, **Downloading n%** or **Failed**). Works without Wi-Fi. |
| Row | Cover thumbnail (when cached), title, author, and a badge: **On device** or **In library**. Rows without a cached cover show the book icon. |
| Open a remote-only row | Connects, downloads the EPUB, verifies it, pulls BookOrbit's reading position, and opens the reader. |
| Open an On device row | Opens the local file immediately. |
| Hold a downloaded row | **Remove from device**: deletes the bytes, keeps the row as remote-only. The reader's saved position is kept so a later download of the same version resumes there. A row badged **Position conflict** (a background sync found both sides moved) opens the resolver instead: it fetches both positions and asks whether to keep the device's or take BookOrbit's. |
| Search | Covers both sections (folded title and author match, as for local books). |
| Refresh button (header) | Runs the network session and returns to the Library; the icon spins while a session is running. |
| View button (header) | Toggles between the list and the grid view (also in **Settings → System → Library view**). |

The BookOrbit "Recent" order is most-recently-read anywhere (then most recently added), newest first, so a book started on another device floats to the top.

### Grid view

The grid shows the same entries as the list (both sections, same tab order, same search) as cover cards with title, author and series (`Series #n`, from the catalogue's `seriesName`/`seriesIndex` or the EPUB's `calibre:series` / EPUB 3 `belongs-to-collection` metadata). The card size is physical, not pixel-based: covers are 15 mm wide at a 2:3 ratio using the per-device PPI table in `src/util/ScreenMetrics.h`, and the column and row counts fall out of the body size (clamped to 56–240 px covers so unknown panels still get a sane layout). Only the visible window has strings and thumbnails in memory; navigation moves a cell at a time and pages by row.

Cover thumbnails are one-bit BMPs at the grid's cover height, generated lazily one per `loop()` pass so the first paint is immediate: local EPUBs use the reader's existing thumbnail cache, BookOrbit books are transcoded from the JPEG retained beside their tile (`covers/<bookId>.bw1.jpg`) into `covers/<bookId>_<height>.bmp`. Books without a cover show the generic icon with the title on the card.

## Background progress sync

On PSRAM-equipped boards, **Sync reading progress** and a nonzero **Sync interval** enable automatic exchanges for downloaded books (30 minutes by default). The worker reconnects using saved Wi-Fi credentials, even if no pages changed locally, so it also checks changes from other devices. With **Keep Wi-Fi On** disabled, the radio turns off after the work finishes. When enabled, it stays connected only for **Wi-Fi Idle Timeout** after the last network job or transfer. New network work restarts that timer; page turns do not. Expiry shuts the radio off without reconnecting until another sync or download needs it. The setting itself never starts a connection.

The last progress attempt and its outcome are saved in the account's `sync.json`. Sleep and reboot do not restart the interval: after a 15-second startup grace period, the reader catches up if the saved deadline has passed. It does not wake itself from deep sleep. Without a trustworthy system clock or RTC it attempts a sync after startup, then uses uptime for subsequent intervals. Failed exchanges retry after five minutes (or the configured interval if shorter). Setting the interval to Off disables periodic exchanges; manual sync still works.

To verify: sync once, sleep/reboot before the interval expires, then wake after the deadline. Serial output should show `BOAGENT: periodic progress sync due`, a saved-network connection, and the progress upload/apply counts without a local page turn. Check `progressAttemptAt` and `progressAttemptOk` in `/.crosspoint/bookorbit/<account>/sync.json`. Repeat with the access point unavailable and then restored to check retries. A newer remote position for the currently open book is applied when that book is reopened.

## Download queue

Opening a book that is not on the card queues it (`<account>/queue.json`, up to 64 books, kept across reboots). On boards with the background agent the queue drains on its own, front to back, and resumes about 15 seconds after a boot; a failed download stays in the queue marked with its error. **Settings → System → BookOrbit → Download queue** (and the download button in the Library header on touch boards while something is queued) opens the queue screen: Confirm on a waiting row removes it, on a failed row retries it, holding a row removes it. Boards without the agent get a **Download queued books now** row there that drains the queue in one foreground session.

## Server capabilities

Every session reads `GET /koreader/plugin/version` after authenticating and selects optional routes from its `capabilities` list. `readingInsights` enables the official dashboard-widget and user-statistics insights API; `deviceThumbnails` enables sized 1-bit covers. Servers without these capabilities retain the base catalogue, progress and JPEG-cover behaviour. **Test connection** shows the version and advertised capabilities.

## Network session ("Refresh" / "Sync library now")

All network work happens in one bounded session; the Library itself never holds Wi-Fi. Every Wi-Fi activity in this firmware reboots on exit to clear heap fragmentation; the session reboots straight back into the Library.

1. Connect to Wi-Fi (the usual network picker) and authenticate.
2. **Catalogue**: `GET /koreader/plugin/catalog/books?format=epub&sort=title` page by page (40 per page), streamed through a SAX parser straight into a staging file, then installed atomically. Up to **1000 books** are cached (`CATALOG_MAX_BOOKS`); a larger server library shows "Showing the first N of M books". Author and Recent orders are sidecar index files built after the transfer.
3. **Covers**: missing covers, downloaded books first. With a BookOrbit advertising `deviceThumbnails` (`?w=&h=&depth=1&format=bmp` on the thumbnail route) the list tile and the grid thumbnail arrive as 1-bit BMPs at exactly the sizes the device draws (a few KB each, no decoding). An older server gets the JPEG path: all missing thumbnails, decoded on the device into both 48×48 tiles and device-sized grid BMPs. Cached JPEGs are reused to repair missing renditions without downloading them again; incomplete JPEGs from interrupted older syncs are fetched again. Failed conversions remain retryable. Covers of books no longer in the catalogue are evicted.
4. **Progress** (when "Sync reading progress" is on): for every downloaded book, compare the reader's saved position and BookOrbit's record against the last acknowledged pair and apply the plan's rules: local-only change → upload (including intentional backward moves); server-only change → apply locally; both changed → ask which to keep (never "the larger percentage"). Unresolved conflicts are asked again next time.

Back cancels between steps; a cancelled session keeps the previous catalogue.

The catalogue cache survives reboots and firmware updates: a file written by an older firmware (v1, before series fields) is upgraded in place on first open rather than dropped. On boards with the background agent, a missing catalogue or one older than 12 hours is refreshed automatically about 15 seconds after boot.

## Download and open

`GET /koreader/plugin/catalog/manifest?ids=<bookId>&format=epub` supplies the exact file identity (file id, size, content version, KOReader partial MD5). The bytes stream to `/.crosspoint/bookorbit/<account>/transfers/<fileId>.part`; completion requires a complete HTTP body, the manifest byte count, a ZIP container signature, and a matching partial MD5. Only then is the file moved to its managed path and recorded; a previous version is deleted after the new record is durable. Interrupted transfers restart from the beginning (the download route has no Range contract) and orphaned `.part` files are removed at the next session.

Before opening, the server's position for that file is pulled and written as the reader's saved position, so a book started elsewhere resumes where it was left. The open itself is the existing reader restore path: the managed path becomes `openEpubPath` and the device reboots into the reader.

## Home reading card

Home shows the saved reading percentage, a progress bar, and the cached EPUB chapter title. Its position status compares the local spine/content key with the managed file's acknowledgement, so reading backwards correctly becomes pending. Conflicts remain visible during retries. Status and position reload after background work completes; rendering uses a bounded snapshot and does not repeatedly scan the file index.

Hold a linked EPUB card, or hold Confirm on a selected card, to check exactly that book using the single-book resolver also used by Library. It presents the device/server conflict choices immediately after comparing that book, without running the account-wide progress, annotation, statistics, catalogue, cover, or insights sync. It returns to Home afterward. Failed or cancelled choices retain the conflict; a successful resolution clears the card’s error using its per-book acknowledgement without advancing the account-wide sync schedule. This status concerns reading positions, not a guarantee that all annotations or statistics are uploaded.

## Reading insights (phase 2)

**Home → Reading insights** (shown once an account is configured; the menu entry carries a one-line excerpt of the cached snapshot: current streak and this week's reading time). Five pages, all drawn from the on-card cache:

| Page | Content |
| --- | --- |
| Overview | This week's total and active days with seven daily bars, the last 30 days, the streak strip, goal progress, the Reader DNA archetype, and the account's book counts. |
| Reader DNA | BookOrbit's archetype and its five labelled scores (length, variety, rhythm, time of day, speed) as horizontal bars. With `booksAnalyzed` 0 the archetype is shown as "not enough reading yet" and the bars stay empty; a speed of `N/A` is shown as n/a, not as zero. |
| Goals & streaks | The year's book goal with a progress bar ("No goal set" when BookOrbit has none), the year-end projection and trend, current/longest streak and the last seven days. |
| Calendar | One month as a 7-column grid with four fixed intensity bands (under 15 m, 15–45 m, 45–90 m, over 90 m), the data watermark double-framed, days past it left blank rather than zero. Month total, active days, busiest day; select a day for its minutes and per-source split. Left/Right (or the arrows) step months; a month that is not cached is fetched in the background when the agent runs, else by Refresh. |
| Habits | Weekday and hour-of-day bars over the last 90 days with the best day and peak hour; hour buckets follow the account timezone. |

The sub-header shows when the snapshot on screen was fetched ("Updated …" / "Not synced yet"); that is the device cache's freshness, not the server's aggregation time. Refresh runs on the background agent where available (Confirm, or the Refresh target on touch boards), otherwise as a foreground session that reboots back to this screen. With **Sync reading progress** enabled, an explicit Insights refresh first syncs reading positions, completed page events, and annotations using the normal reading-sync flow, then fetches insights. Browsing an uncached calendar month only fetches insights. An unsupported Insights API does not prevent the preceding reading sync. Every catalogue refresh also refreshes the insights.

Server side: all eight official routes use `GET /koreader/plugin/insights/` with the catalogue's `x-auth-*` credentials: `reading-goal`, `reading-streak`, `reading-dna`, `year-projection`, `summary`, `reading-heatmap?days=N`, `favorite-days?days=90`, and `peak-hours?days=90`. Widget responses are objects; heatmap and habit responses are arrays. The API advertises `readingInsights` and requires KOReader sync access; it has no separate insights toggle or versioned response envelope. A 403 reports disabled access and a 404 reports unsupported routes; a successful refresh clears those flags.

The reader builds version-2 local snapshots for offline display. Older snapshots are ignored until refresh. Weekly and 30-day totals come from a 30-day heatmap. Calendar requests ask for the trailing UTC day window covering the selected month (within the API's 3650-day limit), stream it, and retain only that month. Memory is bounded independently of the history length. Date buckets and weekdays follow UTC; hour buckets follow the account timezone, whose name is not returned, so the reader does not invent a timezone label. Cached generation timestamps record the device's fetch time.

## On-card layout

```text
/.crosspoint/bookorbit.json                         account configuration (password obfuscated, not encrypted)
/.crosspoint/bookorbit-ca.pem                       optional trust anchor
/.crosspoint/bookorbit/<account>/catalog.bin        32-byte header + 264-byte records, server title order
/.crosspoint/bookorbit/<account>/catalog.author.idx uint16 ordinals, author order
/.crosspoint/bookorbit/<account>/catalog.recent.idx uint16 ordinals, recent order
/.crosspoint/bookorbit/<account>/catalog.series.idx uint16 ordinals, series order (books in a series first)
/.crosspoint/bookorbit/<account>/covers/<bookId>.bw1 48x48 one-bit tiles
/.crosspoint/bookorbit/<account>/covers/<bookId>.bw1.jpg  retained cover JPEG (grid source)
/.crosspoint/bookorbit/<account>/covers/<bookId>_<h>.bmp  grid thumbnails at cover height h
/.crosspoint/bookorbit/<account>/files.bin          128-byte records: file identity + progress acknowledgements
/.crosspoint/bookorbit/<account>/files/<bookId>-<fileId>.epub  managed downloads
/.crosspoint/bookorbit/<account>/transfers/         staging files
/.crosspoint/bookorbit/<account>/sync.json          last session outcome, insights freshness
/.crosspoint/bookorbit/<account>/insights/summary.json  local insights snapshot (re-parsed on screen entry)
/.crosspoint/bookorbit/<account>/insights/habits.json   weekday/hour buckets
/.crosspoint/bookorbit/<account>/insights/cal-YYYY-MM.json  cached calendar months (newest 3 kept)
```

`<account>` is an 8-hex-digit key of server origin + username, so signing into a different account detaches the previous cache, downloads, and acknowledgements. Managed downloads live under the hidden `/.crosspoint` tree on purpose: they never enter the CLIX index (the mapping index is their local source), and "Clear reading cache" does not touch them.

Every record format is little-endian and copied with `memcpy` (no in-place struct casts). Catalogue and file-index writes go through a staging file and rename; the sidecar indices are validated against the catalogue generation they were built from.

## Memory notes

- Catalogue pages stream; no page body is buffered. Small JSON bodies (auth, manifest, progress) are capped at 12 KiB.
- The Library holds at most one order index (2 KiB at the cap), one page of row strings, and one page of cover tiles (fallible; rows fall back to the book icon).
- The sort-index build needs a per-book key array (up to ~26 KiB for authors) after the TLS session is dropped; if it cannot be allocated the affected order falls back to title order.
- Chapter streaming for locator mapping and JPEG decoding run only after the TLS connection is closed (`Client::disconnect()`); the next request reconnects.

## What phase 1 does not include

- **Server-side changes** (plan package 5): the shared revision/idempotency progress contract, multi-format filtering, and small thumbnail variants are not implemented in the BookOrbit repository. Progress sync uses the existing KOSync routes with the acknowledgement-based arbitration above; timestamp semantics are the server's.
- **KEPUB** (package 7): only `format=epub` files are requested.
- **Progress upload for removed books**: a book removed from the device uploads its unsynced position only after it is downloaded again (locator generation needs the file's chapters).
- **Web UI settings** for BookOrbit; configuration is on-device only.

## Hardware verification

Verified on an M5Stack PaperS3 (`pio run -e papers3`, a touch-only development profile added alongside; M5GFX 0.2.29 because the SDK's pinned 0.2.20 is no longer in the registry): verified TLS 1.3 to a Let's Encrypt server, authentication, a 198-book catalogue refresh, cover fetch, download-on-open with the server position applied (opened at the synced spine), and the settings/Library reboot landings. Note the PaperS3 has no wake source, so its idle sleep is a real power-off (press the side button to turn it back on).

Not yet exercised:

- TLS: verified handshake against a Let's Encrypt server; a self-signed server with and without `bookorbit-ca.pem`; wrong password (401) and disabled sync (403) messages; a device with an unset RTC (SNTP seeding).
- Session on the X4 Pro and on a C3 board: heap floor before/after catalogue, covers, and progress phases (`LOG_DBG` heap traces in `BOAPI`/`BOSYNC`), cancellation mid-catalogue and mid-cover.
- Download: cancel, power cut mid-transfer (orphan cleanup), full card, server file replaced (size/hash mismatch), repeated taps, and open-after-download landing in the reader at the server position.
- Library: all four tabs in all orientations, search across sections, remove-from-device and re-download resuming at the saved position, 1000-book cache paging.
- Progress: two devices reading offline then syncing in either order, an intentional reread (backward move) uploading, and the conflict prompt.

## Host tests

```sh
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test
```

`test/bookorbit_catalog` covers the streaming page parser, record/header encoding, ISO-8601 parsing, and `\uXXXX` decoding; `test/bookorbit_progress_policy` covers the arbitration rules; `test/bookorbit_insights` covers the insights parsers (chunked feeds, schema versions, calendar rows landing by date, partial months) and the calendar date helpers.

Within an opened Series group, list and grid titles show the series position first (for example, `#1.5 · The Novella`). Books retain ascending numeric series order regardless of the series-group sort direction; equal or missing positions use title order. Books without a position do not show a number.


## Reading events and annotations

The periodic/manual reading sync also uploads completed page dwell events and
exchanges bookmarks and highlights for closed books. The open book's annotation
files are left alone; close the book before requesting annotation sync. Page
events can upload while reading. Visible-page spans of 5–300 seconds count;
shorter spans and longer idle/outlier spans are discarded, not clamped. Redraws
and menu round trips do not reset the five-minute budget for the same page.
After that budget is exceeded, tracking resumes on a different page.
The page coordinate is normalized to 0–10000, not a font-dependent page count.
Reader menus, subactivities, leaving the reader and sleep end the current dwell.
Events flush in batches of 16 and on leaving/sleep, so a sudden power cut can
lose the current dwell and the not-yet-flushed batch.

Events remain on SD until the server confirms every item as accepted or already
present. The queue holds 8192 events per book; each session sends up to eight
25-event batches per book. A durable clock anchor keeps retry timestamps stable.
Accepted events are copied to `/.crosspoint/reading-events/<book-key>.bin.synced`
before leaving the upload queue. These recovery archives retain the exact uploaded
events for 30 days after acknowledgement and are never automatically uploaded again.
Each little-endian 28-byte record contains the acknowledgement epoch (uint32), then
the existing 24-byte reading event (boot ID, uptime, start time, duration, page,
total pages, all uint32). Cleanup runs on subsequent reading syncs, including for
books no longer being read; an invalid clock defers expiry. Archive failures leave
the pending queue intact. A retry does not duplicate already archived records.
Events use valid RTC time while offline after a reboot. Every connection attempts
to establish system time, including HTTP and HTTPS with verification disabled;
on boards with an RTC, network time is saved there for subsequent offline reading.
An event from an earlier boot that never acquired a clock is preserved in a
`.undated` file and is not uploaded with an invented timestamp.

In the reader menu, **Highlight text** opens word selection. Choose the first
word, then the last word. The device resolves the selection against the original
XHTML and saves exact locators; an unmappable selection reports an error.
**Highlights** lists saved and received quotes; select one to jump there, or hold
it to delete it. A short line in the page margin marks a page containing a
highlight. Selection is confined to the current page. Long or unsupported text
remains unacknowledged rather than being silently truncated. Highlights are
scoped to the configured BookOrbit account.

Bookmark identities are saved before upload. Incoming changes are staged and
validated, then saved in server format without opening the EPUB. Acknowledgement
confirms durable receipt; highlights report `verified: false` until their locators
have been translated. Opening the book translates pending locators using the
already loaded EPUB and caches chapter/text offsets. Original locators and text
remain on SD, so failed translations retry on the next open. Successfully mapped
highlights report `verified: true` at the next reading sync. Pending bookmarks
are excluded from local deletion detection until they have been materialized in
the reader's bookmark file. The worker is drained while opening a book, including
a matched sideload, so mapping cannot race a background exchange. A separate history guard disables deletion propagation after loss of
an annotation index/history file. Corrupt files stop that book's exchange.
A valid, complete empty inventory still propagates intentional deletion of the
last item. To keep requests small, deletion detection is disabled for books with
more than 40 live entries; adds, edits and incoming changes still sync. Each
exchange uploads one changed entry and each session makes up to eight exchanges
per annotation kind per book; larger backlogs continue next session.

## Sideloads and the online catalogue

Each reading-sync session checks up to 16 indexed sideloaded EPUBs by KOReader
partial MD5, preserving its scan cursor across boots. Exact server matches join
progress, event, bookmark and highlight syncing without downloading another
copy. Matching is bounded by the 512-book file index. Detaching a sideload from
the managed list leaves the user's original file intact and remembers that
choice. A matched sideload may still appear in both Library source sections.

**Settings → BookOrbit → Browse BookOrbit online** offers all books, recently read, server-side
search and collections. Book pages contain 20 items and do not depend on the
1000-book offline cache. Collection lists stream through a bounded parser and
are displayed 20 at a time. Choose a book to download it. The browser’s **On device** list is available
without Wi-Fi and includes managed downloads outside the offline catalogue
cache. On touch devices, hold
the Library's refresh icon to open this browser; on button devices, focus the
tab bar and hold Up. Downloaded books remain accessible offline.

The online browser’s **Recently read** section requests EPUB pages with
`sort=recently_read&order=desc` and retains books with a recorded `lastReadAt`,
including finished books. Search keeps that ordering and history filter.
It uses the existing download flow and holds at most 20 entries in memory.
The server currently orders this route by reading-status update time, rather
than the progress timestamp returned as `lastReadAt`; the client preserves
that API order. Pages can contain fewer than 20 entries (or none) because
unread results are hidden; Next still follows the server’s page count.
This is a live online view, independent of the capped offline catalogue.

## Server-backed checks

See [the integration suite](../test/integration/README.md) for the pinned,
isolated Docker server and reproducible commands. The suite exercises the
production protocol/parser/policy against the server, including two-device
conflicts, upload retries, annotation edits/deletions, bookmark acknowledgements,
sideload matching, search and collection filters. Native fault-injection tests
cover queue/index recovery. ESP32 radio behavior, selection ergonomics in every
orientation and physical power-loss recovery still require device testing.

The foreground sync screen shows completed/remaining counts and a progress bar
for the current stage: cached catalogue entries, missing covers across the
catalogue, managed file records checked, and insights requests. Totals
are shown when known; matching local books has no percentage. Reading sync
also shows acknowledged page events and the remaining queue for the current
book, including events left beyond the 200-event session limit. Screen updates
are throttled between operations and never run while a mapper borrows the
framebuffer. A checked book may still need conflict resolution or have failed;
these counts measure work processed, not successful position changes.
