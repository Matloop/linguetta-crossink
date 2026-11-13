# Linguetta on CrossInk

This branch (`linguetta`) is CrossInk (uxjulia/CrossInk, tag v1.6.1, itself a CrossPoint fork) plus the Linguetta
vocabulary client for the Xteink X4 Pro. It adds blend-link taps and Word Lens glosses in the reader,
flashcards with FSRS, and pairing/sync with the Linguetta server (`/api/v1/sync`, protocol 1).

No CrossNext code is used. The `#lb:` link format follows CrossNext's `docs/blend-format.md`; all
code here was written for this branch or is upstream CrossPoint.

## Layout

| Path | What |
| --- | --- |
| `src/linguetta/core/` | Platform-free logic: FSRS-7, op builders, JSONL store, local view (bank + outbox), blend links, TSV decks, history, sync client, Word Lens index and matching (`WordLens`). Built into the host tests as-is. |
| `src/linguetta/device/` | ESP32 glue: `HalFs` (SD), `Service` (state, trusted clock, view cache), `SyncHttp` (upstream `freeink::SecureHttpClient`). |
| `src/linguetta/reader/` | Blend footer over the reader page (save / seen), Word Lens drawing and taps (`WordLensReader`), page words in reading order (`PageWords.h`). |
| `src/linguetta/flashcards/` | Deck list, review, session summary, history, Sincronizar. |
| `src/linguetta/ui/` | `Screen`: base of the flashcard screens, a FreeInkUI app like CrossInk's own activities (theme tokens, fonts from "uiScale", `TouchHeaderBackButton` / `GUI.drawHeader`, GUI button hints, FreeInkUI list, button, metric card, progress bar, sheet). `Ui.h`: the flashcard word and IPA fonts, the reader highlight. |
| `test/linguetta/` | Host tests (GoogleTest) and the contract fixtures copied from the server repo. |

SD card: `/.linguetta/{bank/<language>.jsonl, outbox.jsonl, state.json, rejected.jsonl, history.jsonl, settings.json, wordlens.json, imports.json, decks/, cache/}` and TSV/CSV decks in `/flashcards`.

`/.linguetta/settings.json` (optional): `{"server": "https://…", "deviceName": "…", "language": "it"}`.
The server defaults to `https://linguetta-app.vercel.app` (the contract fixtures' server). If the server changes, the device is unpaired.
`language` only breaks ties for blend ops (see below).

## Upstream seams

These are the only CrossInk files touched. Everything else lives under `src/linguetta/` (picked up by
the `src` glob) and `test/linguetta/`. Every change is marked `Linguetta` so a CrossInk update is a rebase.

| File | Change |
| --- | --- |
| `src/activities/ActivityManager.h/.cpp` | `HomeMenuItem::FLASHCARDS`, `goToFlashcards()`, and "back to Home" focuses Flashcards. |
| `src/activities/home/HomeActivity.h/.cpp` | A "Flashcards" entry, first in the list home and the minimal home (not added to the cover-grid tab bar). |
| `src/components/themes/BaseTheme.h`, `src/components/themes/lyra/LyraTheme.cpp`, `src/components/icons/linguettaIcons.h` | `UIIcon::Flashcards` and its 32 px icon (Lyra draws the home menu icons). |
| `src/CrossPointSettings.h/.cpp` | UI language defaults to pt-BR (only when settings.json has no `language`); `readerRenderSpec` line spacing goes through `wordlens::lineCompression()` (160% floor while Word Lens is on). |
| `src/main.cpp` | `registerDisplayFont()` registers the flashcard word (28 pt) and IPA fonts. |
| `src/activities/reader/EpubReaderActivity.h/.cpp` | CrossInk has no page links, only footnote entries, so `linguettaSplitBlend` moves `#lb:` entries out of the page footnotes into `linguettaBlendLinks`; `linguettaTouch` (after the footnote tap) hit-tests blend links and glossed words and opens the footer. `onEnter`/`onExit` call `wordlens::openBook`/`closeBook`; `renderContents` draws the glosses (scan pass and every composed buffer); the page margins used for the last drawn page are kept for the taps. |
| `src/activities/reader/EpubReaderMenuModel.h`, `EpubReaderDrawerActivity.cpp` | Drawer "More" row `WordLens` (level value, popup shown by the reader) when the device has a study language. |
| `lib/Epub/Epub/FootnoteEntry.h`, `Section.cpp` | `FOOTNOTE_HREF_LEN` 96 -> 256 so a `#lb:` href fits; section cache version bumped. |
| `platformio.ini`, `scripts/simulator_c17.py`, `test/CMakeLists.txt`, `test/library_builder/stubs/HalStorage.h` | Simulator builds on GCC 15 (C17 for QRCode, `-Wno-narrowing`, `-lssl -lcrypto`); `add_subdirectory(linguetta)`; test stub overload. |

CrossInk differences handled in `src/linguetta/`: `TextBlock` keeps no source offsets, so `PageWords.h` derives
paragraph starts and glued words from the line layout; no `replaceFile`, so `HalFs::replace` renames through a
`.bak`; no `TrustedTime`, so the clock is `gettimeofday` plus `halClock.syncFromNTP()`; no `fillRectDither`
ink-preserving mode, so `ui::shade`; Noto Sans 12-18 pt are gone.

## Tests

```sh
cmake -S test/linguetta -B build/linguetta && cmake --build build/linguetta -j && ctest --test-dir build/linguetta
pio run -e x4-pro
```

## Decisions and deviations

- **Wi-Fi exit.** Upstream calls `silentRestart()` after a Wi-Fi session to defragment the heap. On
  the touch board a soft reset cycles the touch/frontlight rails (see `silentRestartToJoinNetwork`),
  and the X4 Pro has PSRAM, so `SyncActivity` just disconnects and turns the radio off. The user
  stays in Flashcards.
- **TLS.** Uses `setInsecure()` (no CA bundle), the same as every upstream SecureNet consumer.
- **Clock.** Ops are recorded only when the clock is trusted (≥ 2026). Sync runs NTP first
  (`halClock.syncFromNTP()`). If NTP fails, `deviceNow` uses the RTC. `serverNow` is not used to
  set the clock.
- **Push.** Batches of up to 200 ops and 256 KB (the contract allows 512 KB). A 413 splits the batch
  in half. Results must match the sent clientIds in order, or nothing is resolved. An unknown
  result status leaves the op in the outbox.
- **One synced deck per study language.** Every sync downloads a snapshot for each language in
  `account.studyLanguages`, each with its own pages and cursor, into `bank/<language>.jsonl` (the
  contract's `bank.jsonl` format, one file per language). All languages download into `.tmp`
  files first; only when every one succeeded are the banks swapped in, the outbox rewritten and the
  cursors saved. One failure leaves every bank, the outbox and the cursors as they were. A bank of a
  language the account no longer studies is deleted after a successful sync.
- **Cursors.** `state.json` keeps `cursors` (`{"it": "v1…", "en": "v1…"}`, device-only field).
  `language`/`cursor` stay as in the contract and mirror the first study language. A `state.json`
  without `cursors` seeds it from `language`/`cursor`.
- **Old single bank.** A `/.linguetta/bank.jsonl` from earlier builds is moved to
  `bank/<its header language>.jsonl` (or deleted if that file exists or it has no header) when the
  service starts, and every cursor is forgotten, so the next sync is full.
- **Deck list (server decks).** Per study language with `header.decks`: a section "Decks ·
  Italiano", "Estudar tudo" (thick border, "N para hoje · X novas de Y", X = new cards in the
  queue, Y = newFlashcardsPerDay + extras), then one row per deck in header order ("N cartões ·
  M para hoje"), subdecks indented under their parent. A deck session includes its subdecks. A
  bank from before decks keeps one row per language. Then the local TSV/CSV decks not imported yet.
  A review uses its deck's language.
- **Queue (contract section 8).** Due cards (state > 0, due now) by due date, then new cards with a
  `queueRank` in rank order, then offline saves (no rank) by creation, cut to the local allowance:
  `today.newAllowance` + the day's `extraNew` ops − new cards reviewed offline today (after
  `nextDayStart`: `newFlashcardsPerDay`). New with `queueRank: null` does not start today. Only
  status `ativo`; `leechWaiting`, a `deckId` outside `header.decks` and siblings (state 0 or 2 with
  `siblingReviewedAt` ≥ today's start, `buryRelated`) stay out. FSRS uses
  `settings.desiredRetention`. Local TSV decks use the account's new-per-day and retention (10 and
  0.9 without a bank). A bank without `newAllowance` uses the old newPerDay − newDoneToday rule.
- **Fichas.** `ficha` lines are review-only cards (front, then front + back; "LACUNA" hint for a
  cloze). They are not in the lemma index, so Word Lens and saves never see them.
- **Teimosas.** A card the server marks `leechWaiting` is not reviewed. Offline, a review that
  takes `lapses` to `settings.leech.lapses` (action `wait` or `suspend`) marks it waiting locally:
  the session shows "Teimosa: errou N vezes. Vai esperar até você encontrá-la num livro ou série."
  and drops it; the summary lists the teimosas. A `save` with a sentence clears the local flag.
  Word Lens does not mark teimosas (deferred).
- **"+5 novas hoje".** When the allowance is spent and more ranked new cards exist in the scope,
  the summary (or the empty session) offers it: an `extraNew` op (n = 5) that raises the local
  allowance at once and restarts the session.
- **CSV/TSV → deckImport.** On each sync, before the push, every file in `/flashcards` never sent
  becomes one `deckImport` op (rows parsed like the local deck, duplicates dropped, cut to 2000 code
  points; hash = SHA-256 of `front TAB back LF` per row). More than one op's worth (500 rows or
  ~12 KB) is split into "Name 1", "Name 2"… with hashes `<hash>.1`, `<hash>.2`. `/.linguetta/imports.json`
  records each file (queued / imported / rejected); a file is sent once by name, so later edits to
  it are not sent again. Imported files are hidden from the deck list (they come back as server
  decks in the snapshot); the local review log of that file is not migrated. Language: the first
  study language.
- **Language of blend ops.** `#lb:` links carry no language and blend EPUBs declare the base
  language, so `save`/`seen` pick (`pickBlendLanguage` in `core/Store.cpp`): the book's
  `dc:language` if it is a study language other than the native one; else the study language whose
  deck already has the lemma; else `settings.json` `language` if it is a study language; else the
  first study language that is not the native one; else the first study language; else `it`. The
  outbox stays one file (each op carries its language).
- **Snapshot.** A full snapshot goes to `bank/<language>.jsonl.tmp`, then rename. A delta goes to
  `bank/<language>.jsonl.delta.tmp`, then merges by id. If an id is both an item and a del in one delta, the del
  wins. Unknown line types are ignored. A 410 restarts the snapshot once.
- **Backoff.** 30 s / 2 min / 10 min after 5xx, and Retry-After (header, else body) after a 429.
  Stored in `state.json` as `failures` and `retryAtMs` (device-only fields). A manual "Tentar de
  novo" still respects it.
- **`lastSyncAt`** is updated only on success.
- **Other account.** If the approved account differs from the stored one, the device asks before
  deleting bank and outbox. Cancelling throws away the new token on the device; the server's token
  stays until it is revoked in Integrações.
- **Word Lens.** Our own code; no CrossLingua or `.lens` packs. While reading a book whose OPF
  `dc:language` primary subtag is a study language, the bank sense of each matching item is drawn
  in the gap above the word (centered, SUP half-size font, cut with … to the room between the
  neighbours). Matching is on lowercased lemma, snapshot `forms` and encounter forms, up to 4 words,
  longest first, never across punctuation or paragraphs; an elided article (`l'acqua`) also tries
  the part after the apostrophe. Words inside `#lb:` links are glossed too. Items come from the
  local view (bank + outbox saves); Conhecido never shows; new, learning and relearning cards always
  show; review cards by level: Muita all, Média stability < 30 d (default), Pouca < 7 d, Desligado
  none. The gloss is the item sense, else the newest encounter sense, else no gloss.
  The level is global, in `/.linguetta/wordlens.json` with the day's seen set, and set from the
  reader's More panel or list menu. While on, line spacing is at least 160%; the gloss is drawn at
  render time, so the section cache format is unchanged (switching Off ↔ on rebuilds the section).
  A tap on a glossed word opens the blend footer for the bank item (sense, next review) with
  "Salvar encontro" (a `save` op with the real sentence, work and position). Each displayed page
  records a `seen` op per glossed item at most once per item, book and day (trusted clock only).
  The index is rebuilt when the language, level or local data changes (`Service::generation()`,
  bumped by any non-`seen` op and by sync).
- **Not implemented.** Unpair from the device.

## Simulator (desktop, SDL2)

`[env:x4-pro-simulator]` builds the firmware natively with
[crossink-simulator](https://github.com/uxjulia/crossink-simulator) (Fedora: `sudo dnf install SDL2-devel openssl-devel`).
Sync (`SyncHttp`) is stubbed there and always reports a network error.

```sh
pio run -e x4-pro-simulator
CROSSPOINT_SIM_SD=/path/to/fake-sd .pio/build/x4-pro-simulator/program        # window
```

Headless, scripted (input times are ms since start, not cumulative; the X4 Pro screen is 480x800 portrait;
screenshots are `ms:path` pairs separated by `;`):

```sh
SDL_VIDEODRIVER=offscreen CROSSPOINT_SIM_SD=$SD \
CROSSPOINT_SIM_SCREENSHOTS='3000:/tmp/decks.bmp' \
CROSSPOINT_SIM_INPUT_SCRIPT='2500:TAP:140,340;6000:QUIT' \
  .pio/build/x4-pro-simulator/program
```

The fake SD is case-sensitive on Linux: decks go in `flashcards/`, not `FLASHCARDS/`.
`scripts/simulator_c17.py` pins C to gnu17 (GCC 15's C23 default breaks QRCode).
The stock smoke test also passes: `python3 scripts/run_simulator_smoke_test.py --env x4-pro-simulator`.

## Screens

All flashcard screens (`DeckListActivity`, `ReviewActivity`, `HistoryActivity`, `SyncActivity`) and the
blend/lens footer (a FreeInkUI `sheet`) derive from `ui::Screen` and use only CrossInk components, so fonts,
row heights, header, button hints, selection style and colours follow the user's "uiScale" and UI theme.
Only the flashcard word (28 pt) and the IPA line use Linguetta's own fonts.

- List screens (decks, history, one session) return `listCount()`: the direction buttons move the
  selection, swipes scroll, Confirm and taps call `onRow()`. Section headings are `ListItem::sectionHeading`.
- Button screens (review, sync) draw `button()`s; the direction buttons move the focus, Confirm or a tap
  calls `onAction(id)`. The main action of a screen is the inverted button.
- The "N hoje" count and "Faltam N" are the header's right label.
