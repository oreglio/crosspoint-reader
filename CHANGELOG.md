## [Unreleased]

### Added

- Raindrop Sync (beta): syncs the article shelf published by a personal CrossDrop
  companion server into `/Articles` as Markdown files. The Library leaves them
  alone — a reading list of hundreds of articles would bury the books — and they
  are read from the File Browser, where they stay.
  Enable it and paste the server URL + device token from the web portal
  settings; launch from the Home menu or Settings > System.

- A `Tasks` entry on the home menu: a to-do list that lives on the card and works entirely offline — adding, ticking, editing and reading tasks never needs a server. Open tasks are listed high priority first, then normal, then low; high-priority titles are set in bold, and long titles wrap over two lines instead of being cut short. `Confirm` ticks the selected task (or reopens a finished one, and its label says `reopen`), `Left` adds one (type the title, then pick a priority) and `Right` opens it. A ticked task moves straight into the finished ones while the selection stays on the task that took its place, so a list is ticked from top to bottom without moving the cursor. Finished tasks gather in an `N done` row at the bottom that `Confirm` expands or collapses. The list holds up to 120 tasks, and a new one is refused before the keyboard opens rather than after you have typed it. The entry is shown by default; the `Enable Tasks` switch in the web portal's settings hides it. Holding `Confirm` on a task (or a long press on touch devices) opens its menu: tick or reopen, view, edit, select, delete done tasks, keep the screen on (no sleep while you stay in Tasks, list or a task's page, until you leave), text size, delete and, last, sync. `Select` turns the list into a selection with square boxes: `Confirm` picks tasks, `Right` ticks them all and `Left` deletes them all after a confirmation; `Back` leaves it. `Delete done tasks (N)` clears every finished task at once, after a confirmation. The server's web page gains the same tools above its list — select, tick or delete several tasks, and clear the done ones — with its own confirmation dialog. `Settings > System > Tasks` sets the list's text size (`10`, `12`, `14`) and the room between its rows (`Compact`, `Comfortable`, `Spacious`). Within a priority, tasks follow the order you drag them into on the server's web page. Deleting asks first, removes the task at once and deletes it on the server at the next sync. A task's own page shows its title, its note a page at a time with `Up`/`Down`, and the length of a pomodoro. `Confirm` ticks it, `Left` edits the title and then the priority, and `Right` opens the pomodoro timer with the task's title above the ring. Notes are written on the server's web page, where a keyboard is at hand; the device only reads them. Tasks can sync with a self-hosted CrossDrop companion server, and only when you ask. `Settings > System > Tasks > Pair with server` shows a QR code and the same 26-character code in groups of four, to scan or type on the server's pairing page; the code is stored on the card, obfuscated, and not in the settings file. `change` replaces it after a confirmation, which unpairs the device until you pair it again. `Settings > System > Tasks > Sync tasks` (or `Sync tasks` in a task's menu, which comes back to the list afterwards) joins Wi-Fi, sends the changes made on the device, brings back the server's, and ends on a summary that names any task the server refused and why. If a sync fails, your pending changes are never lost: they stay queued, and the next sync picks up where the last stopped without resending those the server already accepted. An interrupted sync may already have applied part of the server's changes; the next one completes them. While changes made on the device are waiting to be sent, the right end of the list’s title bar carries a `↻`. The server address goes in the web portal's settings, next to the `Enable Tasks` switch.

- The pomodoro lengths list gains a `Chaining` row. `Automatic`, the default, starts the next step on the press that acknowledges the previous one. `Manual` makes that next step wait for its own press, so you can finish the work, notice it, and start the break when you are actually ready. The choice is remembered between sessions.
- The figure blinks once when a step begins. It used to drop a step within a second of starting, which read as "it is running"; now that it correctly holds, the blink says so deliberately.

- An update channel choice on the update screen, asked each time you check rather than stored in Settings. `Stable` behaves exactly as before; `Beta` also sees pre-release builds, so a single device can try one before everybody gets it. The screen will now also offer a release that is *older* than the one installed — it says so plainly and asks first — which is how a device gets itself off a bad build without a computer.

- A `Pomodoro` mode in the Countdown screen, chosen from a small menu alongside the existing `Target time`. It runs the classic 25-minute work and 5-minute break cycle, with a 15-minute break after every fourth, and waits for a button press between each step rather than moving on by itself — a step that has finished keeps counting `+mm`, so you can see how long it has been sitting done. Both modes now show a ring that drains rather than a bar that fills, with the time remaining written inside it.

- A `Countdown` entry on the home menu, for reading against a deadline. Pick the target hour, then the minutes, to the minute — front buttons step by one, side buttons by six hours or ten minutes, so the side pair drives the tens and the front pair the units — and the screen counts down to that time on the wall clock — remaining time large, a progress bar, and the elapsed time and target beneath it. A target at or before the current time is taken as tomorrow, so "it's 23:40, wake me at 06:00" works. On the X4, which has no clock chip of any kind, the screen asks what time it is now before asking for the end time, and counts from there — so the same "stop at 15:05" phrasing works on both devices. Once the target passes the screen keeps counting the overshoot as `+12m`. The screen stays on for the whole count but the processor drops to its low-power clock between the once-a-minute updates, so a long countdown costs far less battery than staying awake normally; the first button press brings it back to full speed. Requires a device with a working clock.

- A `Library` shelf on the home screen, listing every book on the card in one place regardless of the folders they sit in. Titles wrap over up to three lines instead of being cut short — each row only as tall as its own title needs — and rows carry the author beneath the title, with Left/Right turning pages while Up/Down move the selection. Sort from the tabs above the list: `Added`, `Titles` or `Author`.
- `Settings > System > Library` with a `Use book metadata` switch and a `Rebuild index` button. With metadata on, the shelf shows the title and author a book carries inside itself rather than whatever its filename happens to say — on a typical library that is a 23-character title in place of a 148-character filename, and correctly accented. Books never opened are read straight from the EPUB; books already opened cost only a cache read. One spelling is chosen per author across the whole library, so the same person does not appear three ways.
- Search the Library by title or author from the search action in the header — the magnifier is drawn on X3/X4 too, where the side `Up` button opens it from the sort strip, and `Left` opens it when the first book is selected and the button hint says `Search`. Typing the start of each word is enough — `wut hei` finds `Wuthering Heights` — which matters on a screen where every keypress redraws the whole page.
- Jump through a long shelf by folding it: `OK` on the `Titles` or `Author` tab collapses the list onto its own headings — every author by name, or every initial — and picking one unfolds the list there. `Back` returns to where you were. It pages like any other list, so a hundred authors are a few presses apart, and on the author tab it answers the question a grid of letters could not: you read the names themselves instead of guessing whether someone files under their first name or their last.
- Rebuilding recognises books that were renamed or moved and keeps their place in `Recently added`, rather than treating them as new arrivals.
- The shelf notices new books by itself: every way a book can arrive — nearby transfer, web upload, WebDAV, OPDS or Calibre download, USB transfer — marks the index stale, and the next visit to the Library rebuilds it with reconciliation, so the newcomer tops `Added` and every other book keeps its place. The first tab is now called `Added` rather than `Recent`, because the Home screen's `Recent Books` means recently *read* — this one means recently *arrived*.
- Mark a book as a favorite from its long-press menu. Favorite rows carry a star in place of the book icon, the star tab leads the strip and shows favorites only — with its own sort menu on a long press (`Added`, `Titles A-Z`, `Titles Z-A`, `Author`) — and the flag survives index rebuilds and moves to another folder, because it is stored in its own file keyed by file identity rather than in the rebuildable index. The shelf remembers its whole posture — view, both sorts and even the selected book — across sleep and power-off, in a tiny state file; backing out lands Home on the `Library` entry.
- A `Details` page in the same long-press menu shows the book's title, its author, and the on-disk filename, folder, size and format.
- The long-press menu can also `Delete` a book, confirmed first. The file, its reading cache, bookmarks, clippings, recents entry and favorites entry all go; the index is reconciled on the spot so the shelf never lists a ghost. Reading stats are deliberately kept — deleting a book does not rewrite history.
- Holding `Left` in the Library returns to the top of the list in one gesture, instead of turning back a page at a time.

- `File Browser Display` gains a `Full Name` option in `Settings > System > Files & Cache`, wrapping file names over enough lines that long names are no longer cut short with an ellipsis. Rows are taller, so fewer books are listed per screen than in `2 Lines`.
- Optimized EPUBs now keep their KOReader sync identity: the web optimizer embeds the original file's document hash, and Progress Sync uses it to pair the optimized copy with the original (e.g. the same book in KOReader on your phone). A "Preserve Sync Identity" toggle (default on) lives in the optimizer's Advanced Mode.
- Optimize on device: the File Manager can now optimize books that are already on the SD card — select books or folders (or use the per-book ⚡ action) and each one is downloaded, optimized in the browser, and swapped in place. Reading progress, favorites and the library cursor survive the swap; already-optimized books are detected and skipped; the original is never deleted before its optimized copy is safely on the card.

- A quick-toggle drawer over the reading page, opened by holding a side button or a front page-turn button — pick `Quick toggles` for either pair in `Settings > Controls`. It carries only the switches that take effect at once: font size, dark mode, anti-aliasing, guide reading, publisher page numbers, chapter page count, book progress percentage, and the touchscreen switch on devices that have one. Anything that reflows the book stays in the reader menu, so nothing behind this panel starts a long indexing pass mid-paragraph. `Back` closes it, writing the settings once for the whole visit rather than once per switch.
- The `Library` can now be reached from a long press rather than only from the home screen: as the `Menu` or `Back` long-press action, and from either button pair (`Settings > Controls`). The TXT and XTC readers answer the same gestures, so the setting means the same thing whatever you are reading.

### Changed

- The Library shelf now renders through the same FreeInkUI components as the rest of the interface, which is what brings touch to it: on touch devices, tap a book to open it, hold it for its menu, tap the `Added`/`Titles`/`Author` tabs to switch order, hold the active one to reverse it, hold the ★ tab for the favorites sort menu, use the header search icon, tap a letter in the A-Z grid to jump, and swipe to page the list. Button navigation, search, favorites, details, deletion and the remembered shelf posture all behave as before.
- The File Manager becomes cards on phone-sized screens: tap a card to select it, serif titles that wrap instead of crushing the table, and a download button on each card.
- Opening a folder in the File Manager no longer reloads the whole page. Only the folder's listing is fetched, so browsing lands in a moment instead of downloading the page again each time; `Back` and `Forward` still walk through the folders you visited, and a link opened in a new tab still works. Pages now carry a validator, so reopening the portal costs nothing when nothing has changed.
- The Library now takes a book's author from the book's own metadata rather
  than guessing it from the file name, and reads that metadata by default. A
  book that carries no author of its own joins the Unknown group instead of
  borrowing a name from its file name or its folder. Searching and sorting now
  work on Greek, Cyrillic and CJK libraries, which they never did before.
- The Library sort strip carries one tab per sort key — ★, `Added`, `Titles`
  and `Author` — rather than one per direction. Hold the tab you are already on
  to reverse it; the arrow on the active tab shows which way it runs.

### Removed

- The `Bitter` family and the 14 pt built-in reading sizes are no longer baked into the X3/X4 firmware, which is why the font picker offers fewer entries than before. That space is what pays for the rest of this release. SD-card fonts are untouched: a Bitter or a 14 pt family installed on the card reads exactly as it did.
- X3/X4 firmware now carries English and French only. A device set to any other language falls back to English on this update and cannot be set back to it from Settings — the other translations are simply not in the build.

### Fixed
- The update check and KOReader Sync work again on the X3 and X4. Since the upstream sync, the settings list, which a minimal network boot never shows, kept about 36 KB of RAM resident; with it, the TLS handshake with GitHub failed on allocation ("update failed"), and reopening a book after a KOReader Sync could not get its 32 KB decompression window ("re-optimize the EPUB", although the book was fine). The list is now built only while something uses it (a settings load or save, a settings screen, a web portal request) and freed right after, so reading and every network boot keep that memory. A save that could not build it fails and says so, rather than writing an incomplete settings file.
- KOReader Sync no longer fails with "re-optimize the EPUB" on the first page of a chapter that the optimizer split. That position names the chapter's body itself, with no paragraph yet, and it now maps to the first element the split contributes to the original.
- KOReader Sync finds a phone again after a book was optimized for the reader. Readest names a book only by the content of the file it holds, and an optimized book has two contents: the original the phone may keep, and the copy on the card. The reader wrote its position under the original's id only, so a phone holding the card's copy never saw it. It now writes under both ids, and looks under both in every sync mode, not only in Smart mode.
- Optimizing a book that was already optimized no longer breaks its KOReader Sync. The web optimizer used to take the first pass's split chapters for the original ones and dropped the table linking them back to the original book, so every later sync failed with "re-optimize the EPUB". It now carries that table over, chaining it through any chapter it splits again, and says in the upload log when a book lost its table earlier and can only be repaired from the original file.
- KOReader Sync positions in an optimized book no longer land one chapter early when the optimizer removed an empty chapter stub: the table that links the copy to the original now counts chapters in the original book, as the phone does.

- When two readers have synced the same book under different document ids — an optimized copy here, the original on a phone — the shelf no longer picks between them by percentage. Both records are mapped into this reader's own chapters and pages first, and the one genuinely further along wins. The comparison screen now also names the identity that found the record, beside the device that wrote it, so a record from the wrong reader can be told from a record from the right one.
- KOReader sync decides which way to sync from reading order rather than from percentages. The two engines paginate the same book differently, so a phone position a whole chapter ahead could report the smaller percentage and be ignored — progress simply never arrived, with nothing on screen to say why. The comparison now reads the chapter and page the remote position actually resolves to, and falls back to percentages only when neither side resolved. Books optimized on device keep syncing against their original file, unchanged.
- The Library strip sits flush under the header. It was reserving its space from the device safe area while the header was drawn from the screen edge, so a bezel inset's worth of empty band stood between them.
- The Library header reads `Library` rather than `Library · Title A-Z`. The direction was stated twice — the active tab already draws it as an arrow — and the longer title crowded the search action beside it.
- The sort arrow on the Library strip can be flipped on X3/X4. The active tab drew a direction it had no way to change there: the hold that reverses it was dispatched only by touch, so on a button reader the arrow was decoration.
- Opening or leaving the Library no longer risks closing an index handle before it has been opened.
- Library paging now uses the rows actually measured by FreeInkUI, so variable-height author headings cannot skip books and moving up across a page boundary lands on the previous page's final book.
- Books uploaded through the fast (WebSocket) upload path now appear in the Library without a manual index rebuild.
- Filenames that list the same author twice — the shape export tools leave behind, as in `Henry S_ Warren, Henry S_ Warren Jr` — no longer have that name rearranged into nonsense on the shelf. Authors written surname-first still read the right way round, so `Austen, Jane` shows as `Jane Austen`.

- The Countdown and Pomodoro figure is drawn again on X3/X4. Since v1.5.42 the ring appeared but its middle stayed empty and the label under it sat wrong: the 16 pt face those two screens are pinned to had been dropped from the build.
- Holding `Menu` to open the `Library` or the `File Browser` no longer opens whatever sits under the cursor the moment you let go. The release that ended the long press was landing in the screen that had just opened, which read it as a press of its own.
- The quick-toggle drawer no longer saves the current book's reading settings as the global defaults. On a book with its own font, size, spacing, margins or orientation, flipping one switch in the drawer wrote all of them over the defaults every other book starts from.
- The drawer and the Library no longer act on the release of the button that opened them: the hold that opened the drawer used to step one row on the way in, and the hold that opened the Library moved the selection.
- Opening the drawer no longer reloads the page underneath it. The page was already on the screen, so the reload only cost time and memory — and left the area behind the panel white on the occasions it failed.
- Choosing a built-in font family no longer changes the reading size along with it. Picking one while reading at 16 pt dropped you to 14 pt, a size this build does not carry.
- The font picker no longer lists a family that is not in the build.
- Side page turns in the TXT and XTC readers answer the press again, rather than the moment you let go, whenever the long-press action set for the side buttons does not exist in that reader — which includes `Chapter skip`, the setting they ship with, in the TXT reader. Waiting for the release is what lets a hold be told apart from a press; those two readers were paying that wait for gestures they do not have (`Chapter skip` and `Change font size` in TXT, `Change font size` and `Quick toggles` in both). Where the hold does mean something, nothing changes.
- Checking for updates no longer risks your recent books. `Check for updates` restarts the device into a lightweight network mode that does not read the recent-books file, and `Back` from that screen landed on a Home that showed `No open book` with a full shelf behind it — and, worse, saved that emptiness the moment you opened a book, leaving a single entry where the whole list had been. The KOReader sync credentials had the same trap: editing one field would have written blanks over the others.
- Firmware images now carry their own version and build time. Every release so far shipped the build information of an unrelated image, so a binary asked which build it was answered confidently and wrongly. Nothing on the device read it, but a device dump can now be traced back to the code it came from.
- The File Manager no longer keeps the previous folder's files on screen while the next one loads. On a large card that listing could sit there for a second or two beneath the new folder's name, and its delete, move and rename buttons — and any boxes you had ticked — still pointed at the folder you had just left. The spinner comes back instead, and the folder summary goes with it, so a folder that fails to load no longer shows the previous one's file count next to the error.
- Leaving a folder in the File Manager now closes whatever dialog is open and clears the failed-upload banner. Both belonged to the folder you left: confirming a delete acted on files you had already navigated away from, and `Retry All Failed Uploads` re-sent them into whichever folder you happened to be in.
- Going back in the File Manager returns you to where you were in the list rather than to the top, and where you scroll to after a Back or Forward is remembered too.
- The File Manager tab title returns to `Files` at the root instead of keeping the last folder's name.
- Folders whose name contains a `%` open correctly in the File Manager, by click and from a pasted link.
- Turn reading stats tracking on or off for the whole device or individual EPUB and XTC books, while keeping saved history and Time Left estimates.
- Assign separate short-press and long-press actions to the Left/Up and Right/Down side buttons; existing side-button layouts migrate to matching individual actions.
- Assign Library to power, long-press, button-chord, Home-button, or Quick Actions shortcuts to open the book list directly.
- Customize the top and bottom reader status bars separately, including item positions and progress bars, in EPUB, TXT, and XTC books. Each bar can be previewed where it appears while reading.
- View a selected book's reading stats from its Library or File Browser action menu.
- Library replaces Recent Books with a searchable book list, and adds various book metadata sort options.
- Reset a book's reader settings from the in-reader Settings tab.
- Assign actions to upward and downward slides along either screen edge on touch devices.
- TTF font support on ESP32-S3 devices. Whole-point sizes from 8pt to 22pt will be automatically available.
- In-reader menu for X3/X4/X4 Classic have been updated to a modified version of the in-reader menu for touch devices
- Chapter pages and book progress information is displayed in the frontlight drawer when in the reader for X4 Pro
- Add a Cover Grid Home theme on devices with PSRAM, showing the current book and six library covers.

### Changed

- Set Power short-press and long-press to Sleep, Wake, or Sleep/Wake separately; holding Power can always wake the device. Chord shortcuts and the home button can also now sleep the device.
- Brightness and warmth gestures now respond while you drag, with longer swipes making larger adjustments.
- Text drawing resolves clipping and screen rotation once per glyph, reducing work when painting menus and book pages.
- Library reuses its index on return visits and refreshes after file changes, instead of scanning the card every time.
- Home reads saved EPUB progress and chapter metadata without opening or indexing the book, and stops saved-item checks after the first file.
- Optional EPUB background work yields immediately when rendering is busy, keeping input polling responsive.
- SD-card fonts share identical character lookup tables across styles, reducing memory use and repeated card reads.
- EPUB reader menus now share five tabs across devices. Button devices gain live font and margin previews, Reading Stats, and in-book transfer options.
- Brightness and warmth gestures now adjust in one-point steps for finer control.
- The on-screen keyboard now uses wider outlined keys with clearer spacing on touch and button devices.
- Long status titles shorten faster when they do not fit the screen.
- Leaving an EPUB or TXT reader releases rebuildable font buffers for other screens.

### Fixed

- File Transfer choices no longer appear preselected when opened on a touch device.
- Saved clipping lists now show a scrollbar when more clippings are available below the visible rows.
- EPUB Safe Mode no longer pins inherited fonts and page layout as personal book settings.
- The X4 Pro Home button now steps back through dictionary lookup, chapter selection, and nested settings instead of jumping to Home.
- OPDS downloads now use the first listed author for filename templates when a catalog also lists translators or other contributors.
- Retain the CSS spacing supplied by empty inline spans.
- Improve stability when connecting to Wi-Fi for update checks and KOReader authentication on X4 Pro.
- Crash reports now identify the primary CPU core, show task names when available, preserve both cores' backtraces, and include the firmware ELF hash needed to decode them.
- Release clipping index memory after closing a book or clearing its clippings.
- Keep clipped text, exported excerpts, and chapter titles on complete characters when shortened.
- Keep clipping-selection button hints from covering book text.
- Changing a reader font with incremental indexing now returns after the current reading position is ready, instead of waiting for the whole chapter to be re-indexed.
- Release builds use the pinned PlatformIO core during nested ESP-IDF configuration.
- Adding the sleep moon to the last screen no longer flashes white in night mode.
- Waking the reader skips the intermediate loading icon refresh.
- Screenshot folder names keep complete non-English characters when shortened.
- Longer power-on instructions wrap on the finished update screen.
- Sticky now records periodic heap and PSRAM statistics over its ROM logging path.
- RTL EPUBs use reading-order swipe and tap directions.
- Korean text keeps natural syllable spacing when justified and wraps by word.
- Footnote choices can be selected directly on the reading page, with a list fallback for links without a visible target.
- Changing global font or page layout settings from the pull-down panel on touch devices now updates the open book when it inherits those settings.

## [v1.6.0] - 2026-09-21

### Added

- EPUBs with stable page numbers can jump directly to a specific stable page from the reader menu.
- Hidden folders can be created using the web file manager now when prefixed with a dot.
- Choose whole numbers, one decimal, or two decimals for the book progress percentage in status bar settings.
- Two-finger Screen Rotation can be turned off in Settings > Controls > Taps & Gestures on multi-touch devices.
- Go to % and Go to Stable Page use a numeric keypad for typing an exact destination, including decimal percentages. Touch devices use the keypad exclusively; button-only devices keep the slider by default and hold Confirm/Select to switch to the keypad.
- Files can be renamed from the File Browser action menu while keeping reading progress, bookmarks, clippings, and recent-book entries linked to the new name.
- Firmware builds can include only selected UI languages to reduce flash usage while preserving English fallback.

### Changed

- PNG, XTC, and image-dithering scratch buffers use fewer heap allocations to reduce fragmentation.
- The shared settings catalog keeps its initial allocation instead of retaining unused vector capacity.
- SPI SD-card transfers are batched through the ESP32 hardware FIFO for faster reads.
- SD-card font prewarming releases temporary lookup buffers before allocating large glyph bitmaps.
- UC8179 grayscale images use a slightly longer waveform for stronger midtone separation.
- EPUB image preparation writes extracted data in chunks and reuses two cached images on PSRAM readers.
- Font menus and the web portal use a persistent catalog that loads one family's details at a time, preventing crashes with larger font collections.
- Web portal pages reuse browser-cached content after checking for firmware updates.
- Rapid queued EPUB page turns defer text anti-aliasing and image loading until the final page, making intermediate turns faster.
- Grayscale sleep screen images use the panel's direct grayscale waveform where supported, which folds the base frame into the grayscale pass instead of refreshing the screen separately first.

### Fixed

- The web EPUB optimizer now accepts books that use standard Adobe or IDPF font obfuscation, while leaving DRM-protected books unchanged.
- Frontlight schedule time pickers now use the compact number keypad from Go To screens.
- X4 Classic's left/right tilt direction labels now match the physical page-turn direction.
- Touch keyboards no longer show button-only hold and navigation hints.
- The web settings page no longer offers the Up + Down shortcut on devices that cannot use it.
- OPDS Wi-Fi selection and search entry stay awake while the user is actively choosing or typing.
- USB Drive exits cleanly when a connected host is unplugged without ejecting first.
- EPUB ordered lists show numbers, respect marker-free styles, and retain their container indentation.
- EPUB chapter layout releases rebuildable font caches first, reducing low-memory failures on X3/X4.
- KOReader Sync uploads retain exact text-node positions, including zero offsets and UTF-8 text.
- Saved clipping highlights now retain Focus Reading's custom-font glyphs instead of showing replacement characters.
- EPUB dictionary lookup can select an individual part of a hyphenated word.
- Short Power-button frontlight and touchscreen shortcuts in EPUB books no longer run the configured long-press action.
- Silent restarts now preserve the frontlight state instead of applying wake or schedule settings.
- The Home button now returns from Status Bars to the previous menu instead of leaving the reader.
- OPDS book downloads can follow secure redirects without sharing catalog credentials with the download host.
- Larger EPUB stylesheets work on PSRAM readers, including rules that hide duplicate images.
- JPEG-heavy EPUBs can use PSRAM for decoding on supported readers, leaving internal memory available for reading.
- Importing CrossPoint settings preserves tap and swipe modes without carrying over a stale reader touchscreen lock.
- Saved clippings no longer highlight unrelated single words at page boundaries when matching text after a layout change.
- Quick Lock sleep now respects the configured short Power-button wake behavior.
- Quick Lock now clears when the device wakes after an automatic sleep timeout.
- EPUB content marked with the HTML hidden attribute no longer appears in the reader.
- EPUB paragraphs without source indentation no longer gain a synthetic first-line indent.
- End-of-book selection remains consistent during concurrent redraws.
- Image dithering reports low-memory failures instead of aborting during buffer allocation.
- The debugging monitor plots CrossInk heap and PSRAM logs separately; ZIP failures identify the affected EPUB entry.
- Many progressive JPEG images that store brightness and color in separate scans now render instead of appearing blank.
- PNG sleep overlays preserve four evenly spaced grayscale levels on supported displays.
- Exiting Calibre Wireless on X4 now returns Home with one clean screen refresh instead of repeated blank flashes.
- Manage Fonts no longer crashes after Wi-Fi connects on ESP32-S3 readers.
- Editing font settings from the top drawer's global settings within a book now applies those changes when no per-book font settings exist.
- Per-book reading stats now write to a backup file first.
- Paragraph-alignment previews remain available on text-heavy pages instead of disappearing when the preview sample is full.
- Quick Actions assignments stay visible in button-combo settings, and X4 Classic can use the Up + Down shortcut.
- Sync Progress from the reader menu opens KOReader setup when credentials have not been configured.
- Button-combo settings no longer offer Sleep because the same combo cannot wake the reader.
- EPUB variation selectors no longer appear as missing-glyph boxes after otherwise supported symbols.
- Cancelling Word Spacing on button readers no longer briefly changes the slider value.
- The File Browser now displays decomposed Hangul and accented filenames copied from macOS correctly.

## [v1.5.1] - 2026-09-10

### Added

- Xteink X4 Pro and X4 Classic support, including device-specific firmware and USB Drive access; X4 Pro also supports direct USB file transfers.
- The built-in EPUB optimizer can keep cover art in color while still resizing it to a reader-safe baseline JPEG.
- Custom BMP boot screens, selected in the File Browser or rotated from `/bootscreen` or `/.bootscreen`; sleep screens can also be selected from any folder.
- Quick Lock, assignable button combinations, and shortcuts for Previous Page and Nearby Position Sync. Quick Actions can also be assigned to Power + Up and X4 Pro Home-button gestures.
- Configurable touch page-turn gestures, pinch-to-resize text, two-finger rotation and swipe actions, and a tap-to-hide reader status bar.
- Selectable keyboard layouts, switchable from the keyboard's language key.
- Clippings from dictionary lookups on touch devices, plus selection of text inside EPUB tables.

- Hidden folders can be crated using the web file manager now when prefixed with a dot.

### Changed

- Touch EPUB readers use a half-height, five-tab menu. Sticky opens the menu with a swipe up and book details with a swipe down; X4 Pro frontlight controls include reading stats and reader shortcuts.
- Screen margins have separate Top/Bottom and Left/Right controls, adjustable up to 200 pixels.
- Night Mode applies system-wide on ESP32-S3 devices; frontlit readers can disable periodic full-screen refreshes.
- Waking keeps the sleep screen visible until the reader or Home is ready, unless a custom boot screen is enabled.
- Font choices show available point sizes, Download Fonts replaces the font-manager label, and Wi-Fi passwords are visible during entry.
- Reader controls, shortcut pickers, touch targets, and File Browser settings are easier to reach; Book Options is last in the button reader menu.
- EPUB indexing, image decoding, fonts, and reading-state updates use fewer resources; the web optimizer prefers natural boundaries when splitting chapters.

- Web portal pages reuse browser-cached content after checking for firmware updates.

### Fixed
- Favorites sorted by author now keep drawing when moving to the next page.
- Touch readers can now cancel a font download from the progress screen or its header Back button.
- Sleep screens now reuse a compact SD-card index for custom wallpaper folders, avoiding a full folder scan on every sleep while rebuilding safely after file changes.
- Touch taps and on-screen keyboard presses now route reliably while UI screens redraw.
- Long-pressing Up or Down in long popup lists now advances by a full page.
- EPUB table fixes now preserve final-column widths, give dense tables enough space for leading labels, and split oversized words instead of clipping them.
- Nearby Position Sync now leaves the sending device with a single Back action after sharing a position and tolerates repeated packets while the receiving reader prepares the location.
- Clearing an EPUB's reading cache now returns Home so the book can rebuild its cache safely when reopened.
- Large EPUB tables now use a bounded row-streaming grid on low-memory devices, preserving readable styled cells and falling back explicitly for unsupported table structures.
- Large EPUBs with thousands of chapters can now finish indexing on X3/X4 without running out of memory.
- Dictionary definition popups no longer leave an empty white button-hint block over the reader page.
- Recent Books and KOReader Sync settings now remain intact after returning from lightweight network screens.
- XTCH cover and thumbnail generation now stays within the available memory on X3/X4 after its cache is cleared.
- Change Font shortcuts now switch away from an active SD-card font instead of reindexing with the same font.
- Network connections no longer trigger repeated full-panel flashes.
- Dictionary word selection now follows the physical front-button direction in counter-clockwise landscape mode.
- End-of-book suggestions can now be opened by tapping them on touch devices.
- XTC and XTCH readers now ignore overlapping page turns while the display is updating, preventing corrupted pages after rapid swipes.
- XTC and XTCH readers no longer corrupt a page when turning or opening the menu during rendering.
- Dictionary font switches now retry after releasing the reader font when memory is tight.
- XTC table of contents now includes every available page entry, so large books can jump beyond the first 128 pages.
- Saved clipping highlights now remain accurate when a font or font-size change reflows a word across an inserted hyphen.
- Xteink readers wake faster by skipping redundant bootloader image validation after sleep.
- Large EPUB images keep the reader responsive during decoding.
- Full-height EPUB images no longer disappear when their container adds a top margin.
- EPUB page estimates now keep image-only and mixed image pages from being multiplied by XHTML byte density.
- Cancelling a chapter, footnote, location, or QR screen opened from the EPUB menu returns to that menu.
- EPUB and XTC readers retain less memory during ordinary reading by loading end-of-book suggestions only when needed.
- Long inherited dictionary-font names no longer overlap or extend beyond Font Options rows at Large UI size.
- KOReader Sync progress no longer remains interleaved with EPUB image pages after returning to the reader.
- Quick Lock sleep now respects the configured short Power-button wake behavior.
- Quick Lock now clears when the device wakes after an automatic sleep timeout.
- EPUB content marked with the HTML hidden attribute no longer appears in the reader.
- End-of-book selection remains consistent during concurrent redraws.
- Image dithering reports low-memory failures instead of aborting during buffer allocation.
- The debugging monitor plots CrossInk heap and PSRAM logs separately; ZIP failures identify the affected EPUB entry.

- Exiting Calibre Wireless on X4 now returns Home with one clean screen refresh instead of repeated blank flashes.
- Manage Fonts no longer crashes after Wi-Fi connects on ESP32-S3 readers.

- Clipping highlights stay aligned after font changes, retain multi-paragraph text, and remain readable in Dark Mode. Selection stays on its final page, and browsing saved clippings responds reliably.
- Dictionary lookup respects landscape controls and selected fonts, handles repeated lookups more reliably, and returns to the reader cleanly when dismissed.
- EPUB tables retain column widths and wrap long labels; mixed-direction text, Arabic/Persian shaping, ruby annotations, and footnote styling render correctly.
- EPUB contents links, split-chapter navigation, footnote resumes, and end-of-book exits preserve the intended reading position.
- Large EPUBs, image pages, and SD-font preparation recover more safely from limited memory and SD read errors.
- XTC/XTCH page turns no longer overlap, tables of contents show all entries, and covers retain their grayscale detail. TXT font-size controls and Home progress work reliably.
- Quick Resume, custom sleep images, transparent overlays, and X3/X4 wake refreshes avoid blank screens, grid artifacts, and lingering images.
- Long-press shortcuts no longer trigger an extra action on release; Quick Actions, Quick Lock, and Dark Mode shortcuts respond consistently.
- Touch scrolling, page gestures, font-download cancellation, and reader settings behave reliably across orientations and UI scales.
- KOReader Sync preserves orientation and settings, handles missing remote positions, and avoids repeated screen flashes during network transitions.
- Nearby sync, OPDS search, file listings, and image actions handle input and errors more reliably; Calibre Wireless shows the full IP address.
- S3 sleep, charger detection, and power-button wake behavior are more reliable. USB Drive recovers from storage failures, USB transfers avoid watchdog errors, and updates reject firmware for a different board.
- Book-specific settings stay separate from global defaults, and Recent Books and KOReader credentials survive network restarts.

### Removed

- The undocumented X4 Pro power-button double-click frontlight toggle.
- Built-in reader-font emoticons and hand gestures; SD-card fonts retain emoji fallback support.

## [v1.5.0] - 2026-08-08

### Added

- X4 Pro readers can lock the Home button while reading, with a Power-button shortcut to toggle it.
- End-of-book suggestions can now be opened directly by tapping their rows on touch devices.
- Quick Actions lets readers assign up to five favorite reader commands to one Power, Back, or Menu shortcut.

### Fixed

- EPUB tables now lay out a row at a time in both Incremental and Full Section indexing, keeping regular tables readable without whole-table buffering.
- Touch support for Seeed Studio Sticky
- Nearby File Transfer can send EPUB, TXT, XTC, XTCH, PNG, and BMP files directly between two CrossInk devices without a Wi-Fi network.
- Recent Books and image-file long-press actions can send files directly to a nearby CrossInk device.
- Dictionary lookup and lookup history
- EPUB books can use a dedicated SD-card dictionary font while keeping a different reader font.
- EPUB books can set a dedicated dictionary font size independently of the reader font size.
- Dictionary font and size defaults can be set globally from Settings > Reader > Font Options, with per-book choices still taking precedence.
- Reusable dictionary SD-font builder with IPA coverage and per-family ZIP packaging
- RTC-enabled devices can now choose the date format and numeric separator shown in headers from Settings > System > Device.
- The web EPUB optimizer now splits oversized chapters into memory-friendlier sections before sending them to the reader.
- Reader indexing can now use `Incremental` or `Full Section` mode globally or per book; changing modes keeps the current chapter readable and applies when the next chapter needs indexing.
- Look Up Word can now be assigned to short- and long-press Power button shortcuts.
- EPUB readers can now choose from five word-spacing levels, from normal through extra-wide.
- EPUB inline-image pages on X3 now use the grayscale-aware display base before the image grayscale overlay, reducing the moment where images appear too dark before settling.
- EPUB publisher small-caps styling now renders ASCII lowercase text as smaller capital letters without needing extra font files.
- When incremental EPUB indexing runs out of memory at the first unindexed page, the reader now silently restarts once and resumes the book with a fresh heap.

### Changed

- PSRAM-equipped readers now keep EPUB grayscale and image-cache working buffers in external memory, preserving more internal RAM for layout and reducing repeated SD reads on image pages.
- Reader font sizes now persist as actual point sizes, keeping the closest matching size when font families or installed files change.
- SD-card fonts now include the built-in reader fallback stack for common symbols, emoji, and selected CJK glyphs while retaining Noto Sans fallback coverage.
- Downloadable SD-card fonts are now rendered with the same darker anti-aliasing as the built-in reading fonts.
- Full-section EPUB indexing now prepares one-page chapters and direct jumps to a chapter's last page, while avoiding repeated checks after the next chapter is ready.
- EPUB grayscale rendering now reuses its 8 KB strip buffer across stable pages, reducing repeated heap allocation and release during long reading sessions.
- Reading progress is now saved in batches during ordinary page turns, immediately after layout changes, and when leaving a book, reducing repeated SD-card writes without carrying stale pagination into the next session.
- SD-card font discovery now waits until a custom font is selected or font settings are opened, reducing SD-card work during normal startup with built-in fonts.
- EPUB page turns using SD-card fonts now prepare the next page's glyphs while the reader is idle.
- Dictionary lookups now reuse open index files for stem matching, reducing repeated SD-card work after a miss.
- The web file manager now batches directory listings into fewer network packets, improving large-folder response time.
- Firmware releases now identify the supported device type: X3/X4 or Seeed Sticky.
- Image-heavy EPUB chapters now index by reading image headers first and extract each full image only when its page is shown.
- EPUB books with repeated byte-identical stylesheets now parse each unique stylesheet only once when building caches.
- SD-card fonts now reuse their page-sized glyph buffers, reducing heap fragmentation during long reading sessions.
- Firmware builds now prioritize usable heap over oversized system timer stacks and maximum WiFi throughput, leaving more memory for reading and network operations.
- Downloaded-font size range options now show their actual point-size ranges instead of firmware build names.
- KOReader Sync and authentication, OTA updates, and OPDS browsing now restart into a lightweight network mode that leaves reader and Home data unloaded, providing more contiguous memory for WiFi and secure connections.
- The web file manager can now delete non-empty folders recursively and, when hidden files are shown, remove hidden or system-managed SD card items after confirmation.
- SD-font, OPDS catalogs, and other unneeded settings now stay out of memory while reading unless their settings are open.
- EPUB books can now keep more saved clippings without loading every clipping's text into memory while reading.

### Removed

- The font download manager no longer offers a Download All action; fonts can still be downloaded individually or updated together.

### Fixed

- Book menu tab navigation, popup scrolling, customized Reading Stats hints, and short button presses after low-power mode now work reliably.
- Sleep screens now honor the current orientation, avoid X4 transition flashes, fall back to a valid wallpaper when needed, and handle low-memory image decoding without rebooting.
- Choosing Set Cover uses the selected image in place, and Home no longer repeatedly generates missing EPUB covers.
- Finished-book suggestions are now collected before an EPUB is moved to `/Read`.
- Manage Fonts now opens and scans large catalogs more safely on X3/X4, reports low-memory failures instead of restarting, and returns to Font Options when cancelled.
- Network screens refresh cleanly on X4; long errors wrap correctly; saved Wi-Fi networks and KOReader connections recover more reliably after restart or a missing address.
- Translated Wi-Fi and clock labels no longer truncate text or time values, and clock sync no longer risks a reboot while saving settings on memory-constrained X3/X4 devices.
- KOReader Sync no longer crashes during time setup, re-triggers while connecting, or loses precise EPUB positions; CrossPoint-only data stays on the official CrossPoint Sync server.
- Firmware updates reject images for the wrong chip family, and saved Wi-Fi settings safely handle concurrent access and corrupted values.
- EPUB opening, reflow, and background indexing now handle fragmented memory more safely, retry recoverable work, remain responsive to input and setting changes, and show useful errors instead of rebooting or silently returning Home.
- Low-memory EPUB grayscale and sleep rendering now fall back safely without leaving stale display content.
- Full-section indexing preserves more memory for large chapters and cancels speculative work on page turns, keeping the reader responsive.
- SD-card font and clipping work now release temporary data at the right time, preserving memory for reflow, dictionary use, covers, and thumbnails on X3/X4.
- EPUBs with book-specific built-in fonts no longer load an unnecessary global SD-card font, and custom fonts retain ligatures.
- EPUB styling choices apply before style caches load; CSS-heavy books use less temporary memory; and disabling Embedded Style consistently skips unused stylesheet work.
- EPUB layout now keeps CJK ruby and spaces, Russian paragraph continuations, Focus Reading, underline/strikethrough runs, and right-to-left text correct.
- EPUBs with flowing `<br>` elements, image-led or decorative chapter headings, unsupported images, and dense final pages now lay out without excess gaps, clipping, dropped images, or misleading low-memory warnings.
- EPUB footnote and cross-reference previews now show complete notes, including targets in the middle of a paragraph.
- Saved EPUB positions, clipping highlights, and selections now stay accurate after font, orientation, or indexing changes; selections also remain readable in dark mode and on memory-tight pages.
- Dictionary misses can switch dictionaries without leaving the reader, and dictionary read failures now report an error instead of a false “not found.”
- Reader popups, KOReader Wi-Fi labels, Lyra battery headers, and the sleep message now remain correctly oriented and positioned.
- Manual refreshes preserve EPUB and TXT text anti-aliasing; XTC and XTCH status bars show the configured time-left estimate.
- Watchdog panics with captured diagnostics open crash reporting, while reset-only events return normally; power-button wake timing no longer depends on SD-card startup.
- The web file manager and uploads now handle simulator/device ports and stalled connections safely; unsupported settings stay hidden, and the optimizer removes empty chapter stubs without breaking table-of-contents links.

## [v1.4.0.1] - 2026-07-28

### Added

- Updates to support Xteink device detection so the correct display panel driver is used.

## [v1.4.0] - 2026-07-10

### Added

- Dashboard UI theme for the Home screen, showing the current book cover and reading stats.
- Nearby Position Sync for sending or applying the current EPUB position between two CrossInk devices over ESP-NOW.
- Web EPUB optimizer support for CrossInk location metadata, so optimized EPUBs can keep better progress and stable page numbers.
- Reading Stats support for XTC and XTCH books, including reader menus, Home and sleep screen stats, mark finished, delete stats, and preserving stats when clearing book caches.
- Web file manager image previews, so PNG, JPEG, BMP, GIF, and WebP files can be viewed inline before downloading.

### Changed

- Large EPUBs, SD-card font-heavy books, and cover thumbnails now open, index, and generate more reliably under low-memory conditions.
- Home and sleep screens now load more cover and thumbnail data only when needed, reducing reader startup work and reusing cached cover data where possible.
- Built-in reader font choices have been reduced to Lexend Deca and Bitter, reducing firmware size while keeping fallback glyph coverage.

### Removed

- Teensy firmware builds are no longer produced for releases or release candidates.

### Fixed

- EPUB render-mode and Safe Mode toast messages now clear reliably, even when the reader is low on memory.
- EPUB Reading Stats no longer drops unsaved page-turn counts after viewing the stats screen mid-session.
- KOSync is more reliable with many SD-card fonts installed, reducing low-memory failures during secure sync requests and uploads.
- Web file manager actions now handle filenames with special characters safely and reject unsafe rename characters before saving.
- Auto Turn interval settings and related action prompts opened from long-press shortcuts now stay open after releasing the shortcut button.
- EPUB footnote previews no longer show clipped status-bar labels or misleading reader progress indicators, and clipping selection now works from footnote previews.
- Font selection no longer reopens the font preview after choosing a font.
- EPUB chapters with stale publisher style data now rebuild it instead of opening without the book's styling.
- Large SD-card font EPUBs no longer overlap characters after font or line-spacing changes, and clipping selection can fall back to a built-in UI font when needed.
- EPUB cover and thumbnail generation is more reliable with custom SD-card fonts selected and optimized books under low-memory conditions.
- Web EPUB optimizer now preserves more PNG and SVG artwork on-device, including transparent PNGs, dividers, and images in malformed or XML-declared chapters.
- Unsupported SVG images in EPUB chapters are now skipped silently instead of triggering low-memory image warnings.
- Nearby Position Sync now silently restarts back into the reader after using ESP-NOW, matching other WiFi sync flows and reducing post-sync memory fragmentation.
- EPUB page cache loading now uses fewer small heap allocations, reducing fragmentation-related reader failures.
- EPUB grayscale page turns on X3 now use the grayscale-aware display base, reducing the moment where new text appears too dark before the anti-aliased overlay finishes.
- EPUB chapters with many inline anchors, footnote links, malformed XHTML, large publisher styles, or SD-card fonts are less likely to fail or get stuck on the indexing screen.
- EPUB opening and image rendering now recover from more low-memory conditions instead of rebooting, including landscape image pages and books that need lighter render modes.
- EPUB clipping selection now follows right-to-left line order when selecting Hebrew and other RTL text.
- Lyra Carousel no longer shows a blank carousel after returning from WiFi-related File Transfer screens and moving between the menu row and book row.
- Generated SD-card font packages now include the same core glyph coverage as built-in reader fonts.
- Manage Fonts no longer crashes while loading or reloading large SD-card font lists.
- Minimal Home no longer swaps to another recent book when returning from Settings when Back button is mapped to the first button.
- Cancelling a font download now stops on the first Cancel button press instead of needing several presses.
- The `Inverted` sleep cover filter now keeps book covers unchanged on Minimal and Dashboard sleep screens while switching the background to white.
- Rare EPUB open or thumbnail crashes during ZIP decompression are fixed.

## [v1.3.4] - 2026-06-24

### Added

- File Browser now indexes large SD-card folders so directories with many books can be browsed without loading every filename into memory at once.
- EPUB text clipping with saved highlights, clipping lists, and Kindle-style `/My Clippings.txt` export.
- `Create Clipping` is now available as a reader shortcut for short/long Power, long-press Menu, and long-press Back actions.
- Per-book EPUB options for font, layout, styling, reading aids, and render modes, including `CrossInk Default`, `Balanced`, and `Light` modes for difficult books.
- Arena allocator (`lib/Memory/Arena.h`) for burst-then-discard allocation patterns - reduces heap fragmentation during EPUB parsing and page layout over long reading sessions.
- Optimized EPUBs now store location metadata at `META-INF/x-locations.json`.
- X3 SD-card writes now use the RTC for file timestamps when the clock is available.

### Changed

- The EPUB reader menu now splits the growing menu into 3 screens, labels per-book settings as `Book Options`, and avoids showing duplicate `Orientation` controls.
- The `Inverted` sleep cover filter now flips Minimal and Reading Stats sleep screens to black text on a white background.

### Fixed

- Quick Resume no longer shows a blank page after EPUB next-chapter indexing.
- Calibre Wireless transfer status no longer stacks the last received-file message on top of the upload percentage.
- X3 Tilt Direction now labels left/right choices as `Left-Right` and `Right-Left`, with existing left/right preferences migrated to keep the same physical tilt behavior.
- EPUB layout now honors publisher page-break CSS, avoids stretching justified spaces before closing punctuation, and keeps large CSS rule sets in a smaller disk-backed lookup cache.
- EPUB first-open conversion now uses more compact OPF manifest lookups and streams cover-wrapper parsing to avoid large temporary heap buffers on books with huge manifests.
- EPUB chapters that run out of memory now retry with `Balanced`, `Light`, and final `Safe Mode` rendering before showing an error, apply the same fallbacks during next-chapter pre-indexing, and let book action menus reset a book's reader settings if Safe Mode still cannot open it.
- EPUB reader font-size changes now restore the current chapter position by content instead of jumping far backward after re-indexing.
- Reading Stats now use the reader's last live book time-left estimate instead of showing a separate fallback estimate.
- Per-book reading stats now migrate compatible legacy `stats.bin` files into the `stats_v5.bin` flow instead of resetting when only the old filename exists.
- Lyra Carousel Home menu rendering now avoids extra label allocations that could crash builds under low memory.
- Lyra Carousel Home cover refresh no longer risks a reboot when memory is tight after returning to or selecting a recent book.
- EPUB image-heavy chapters no longer risk a reboot while saving their reading cache under low memory.
- TXT readers now stay open when pressing a page-turn button at the end of the file.
- Long-press reader shortcuts that open another screen no longer close or confirm it again when releasing the shortcut button.
- RoundedRaff's header battery icon and percentage now sit lower to avoid clipping at the top edge.
- Lyra Carousel now keeps the Home header current when rendering the menu or restoring cached carousel frames, preventing stale battery and clock values while navigating between books.
- Web file manager multi-delete now handles larger selections without failing after a small batch.
- Portuguese EPUBs now use Portuguese hyphenation rules instead of leaving long words unhyphenated when Hyphenation is enabled.
- Progressive JPEG EPUB covers now render more smoothly in generated cover and thumbnail BMP assets.
- EPUB section layout now flushes long text runs earlier when Focus Reading or Guide Dots are enabled, reducing low-memory failures on difficult books.
- Footnotes in EPUBs with very large shared notes sections no longer cause long stalls when opened.
- Firmware updates now follow GitHub asset redirects before streaming the install.
- Tiled grayscale rendering now serializes display transfers on the shared SPI bus to avoid display glitches during SD activity.

## [v1.3.3] - 2026-06-13

### Added

- `File Browser Display` in `Settings > System > Files & Cache` for choosing one-line or two-line file browser rows across all themes, while preserving Minimal users' existing two-line display on upgrade.
- `Hide File Extension` in `Settings > System > Files & Cache` for expanding file-browser filenames by hiding the right-side extension label.
- Device Name in Settings > System > Device for customizing the KOReader Sync and Nearby Stats Sync device label.
- Additional shortcut options and new ability to add custom shortcuts for Long-press Back Action.
- Delete Reading Stats actions in the EPUB reader and book action menus for clearing one book's stats without deleting its cache.

### Changed

- CrossInk settings now save to `/.crosspoint/crossink-settings.json`, with a one-time fallback migration from `/.crosspoint/settings.json`, so switching between firmware builds is less likely to reset preferences.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.

### Fixed

- RoundedRaff's date shown in settings now sits lower on X3 devices instead of overlapping the battery.
- Clear Bookmark List now asks for confirmation before deleting a book's bookmarks.
- Clear Reading Cache now preserves per-book reading stats while continuing to leave all-time reading stats untouched.
- Moving finished EPUBs to `/Read` now consistently preserves reading progress, per-book stats, bookmarks, and resume state.
- Book settings option lists now return to the submenu they were opened from when pressing Back.
- Lyra Carousel now refreshes its cached Home icon row after OPDS, Reading Stats, or Bookmarks icons appear or disappear.
- KOReader Sync failure screens now wrap long error messages and shut down WiFi cleanly before returning to the book.
- Sleep Screen > Cover now generates the current book cover on demand instead of falling back to the dark sleep screen when the setting is changed after opening a book.
- File Browser now previews PNG images instead of trying to open them as EPUBs, and hides common macOS and Windows metadata files.
- File Browser now refreshes immediately after falling back to the root folder from a stale saved path.
- File Browser now stops loading oversized folders before low memory can crash the device and shows a memory error instead.
- TXT reader long-press Power page turns now work when Long Power Button is set to Page Turn.
- SD-card font read failures no longer risk a reboot while cleaning up the failed file read.
- Page Overlay sleep screens no longer force EPUB chapters to re-index after waking.
- Page Overlay sleep screens now use the current screen as the overlay background outside the reader instead of trying to rebuild a stale book page.

## [v1.3.2] - 2026-06-10

### Added

- Current date in the top-right Settings header on X3 devices.
- Dark Reader Mode for EPUB and TXT reading screens, plus shortcut actions for the power button and front-button long press.
- File Browser long-press folder action for choosing a custom sleep-image folder instead of only `/.sleep` or `/sleep`.
- Expanded X3 Reading Stats, including streaks, time charts, editable dates, all-time backups, reset controls, an idle-time threshold, and the `Minimal Stats` sleep screen.
- `Reset Reading Pace` in the EPUB reader menu when Time Left is enabled, for clearing only the time-left pace estimate while keeping book reading stats.

### Changed

- Display, Reader, and Controls settings now open list menus instead of cycling through options one by one.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.
- Reading time and time-left pace tracking now ignore page intervals longer than the configured idle-time threshold.
- Web portal pages now use shared templates, stylesheet, and logo assets, reducing on-device page size and improving browser caching.
- Already-cached EPUBs now open directly to the first page without an extra book-loading popup refresh.
- Reader font-size choices now show point sizes like `10 pt` instead of names like `Tiny`.

### Fixed

- Inverted reader menus now honor orientation-aware side-button navigation.
- EPUB book time-left estimates now wait for more session pace samples and use a progress-based floor after pace data exists, reducing swings from unusually short or long pages.
- Deleting an EPUB book cache now preserves that book's reading stats and pace data.
- X3 clock settings now have clearer UTC offset editing, and `Sync Date/Time` can use saved WiFi networks automatically.
- Home, Lyra Carousel, WiFi setup, and SD-card font flows now release memory more aggressively to avoid freezes or crashes on constrained builds.
- Vietnamese settings labels no longer show replacement diamonds after generated translation offsets shifted.
- KOReader Sync now lands correctly at chapter starts and shows more specific connection guidance.
- EPUB bookmarks saved under the old unstable path hash now show up again, including for books moved to `/Read`.
- SD-card font downloads now use versioned direct S3-hosted HTTP endpoints with CRC validation, avoiding GitHub release redirects and ESP32-C3 TLS stalls when loading the font catalog.
- EPUB text blocks now keep the book's alignment style when an inline image appears before the text.

## [v1.3.1] - 2026-05-28

### Added

- EPUB reading-position improvements, including bookmark anchors, bookmark preview snippets, and optional chapter/book time-left estimates.
- Nearby Reading Stats sync with separate totals for this device and all synced CrossInk readers.
- Per-server OPDS filename settings so downloaded books can use either Author - Title or Title - Author.
- EPUB render heap diagnostics that include the largest allocatable block, not just total free heap.

### Changed

- Moved the X3 reader clock into a new top-centered status bar and moved clock settings to Settings > System > Device.
- Reworked Display, Reader, Controls, in-reader options, and larger System settings groups so related options open as submenus.
- Improved OPDS and font download responsiveness by reducing progress-update overhead and temporarily disabling WiFi power saving during transfers.
- Book selection now shows a loading popup before EPUB indexing or cache loading begins.
- Delayed the automatic finished-book prompt until the reader leaves the chapter where they reach 99%.

### Fixed

- WiFi settings screen now keeps the displayed MAC address consistent with the router-visible WiFi address.
- Reader UI issues with inverted menu button hints, Lyra Carousel popups, and Auto Page Turn interval persistence.
- Web uploads and KOReader Sync progress saves now preserve progress, stats, settings, and valid resume data for refreshed book files.
- OPDS low-memory handling now shows a specific parser-buffer memory message and releases SD-card fonts before catalog loading.
- EPUB cache, CSS, table, SD-card font, and allocation failure paths now recover, retry, or stop cleanly under low memory instead of opening unstyled pages, failing unnecessarily, or risking a reboot.
- EPUB text with invisible word-joiner characters no longer shows replacement diamonds for missing font glyphs.
- Clarified the low-memory EPUB image warning so it says some or all images may be missing.

## [v1.3.0] - 2026-05-21

### Added

- Back/Cancel support while downloading books from OPDS catalogs.
- Recent Books long-press menu in both List and Grid views with delete, cache delete, completion, and remove-from-recents actions.
- Minimal sleep screen option that shows the current book cover and reading progress on a dark background.
- More detailed WiFi connection debug logs for scans, selected networks, status changes, disconnect reasons, and timeouts.
- 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.
- In-reader confirmation message when a shortcut turns tilt-to-turn on or off.

### Fixed

- WiFi and OPDS connection-flow edge cases: manual Settings connections now show the connected status before continuing, copied or corrupted saved-password files are rejected before use, OPDS retries show loading before requests, and large OPDS feeds fail safely under low memory instead of rebooting.
- Reader and Home UI polish issues, including landscape status-bar settings, missing Vietnamese labels, File Browser and Lyra Carousel icon alignment, cover thumbnail artifacts, and duplicate Home progress/stat loading.
- EPUB cache and low-memory handling now use stable cache folder keys, migrate older cache folders where possible, rebuild stale section caches, lay out very long text blocks earlier, stream table fallback content when heap is tight, and clarify the warning text.
- Sleep-entry, network, and SD-card font download reliability improvements: cached sleep-screen assets are reused, OPDS pages idle normally after load, the X3 tilt sensor sleeps outside the reader, WiFi power saving is disabled during transfers, WebDAV stack usage is lower, longer stalls are tolerated, interrupted font files are retried, and active reader fonts are freed when needed.
- Remaining reader service edge cases, including an XTC chapter selector crash on memory-constrained builds, SD-card font size selection, SD-card font-size shortcuts skipping manually installed sizes, and KOReader Sync login compatibility with self-hosted servers that return valid JSON on success.

### Changed

- Modified upstream "page-as-sleep" behavior into a new `Sleep Screen > Quick Resume` option, which also keeps `Quick Resume on Timeout` on, and renamed the timeout-only toggle.
- Improved reader and browser menu behavior by moving the Footnotes shortcut above Select Chapter, wrapping long book titles in action menus, and reducing progress-screen repaint work during OPDS and SD font downloads.

## [v1.2.11.1] - 2026-05-15

### Changed

- Removed Medium font size from `xlarge` build to get it below the size limit

### Fixed

- Lyra Carousel is now included by activating the build flag `DCROSSINK_ENABLE_LYRA_CAROUSEL=1`

---

## [v1.2.11] - 2026-05-14

### Added

- New personal theme: "Minimal"
- Custom sleep timer picker so `Time to Sleep` can be set from 1 to 30 minutes instead of cycling fixed presets.
- In-reader Controls shortcut for customizing buttons without leaving the book.
- Bookmark cleanup shortcuts: hold Select on a bookmark to delete it, or hold Open on a book in Bookmarks to clear that book's bookmark list.
- Confirmation message after deleting a book's cache from the reader or File Browser.
- File Browser long-press action for deleting an EPUB or XTC book's cache.
- Downloaded-font size range setting so SD-card fonts can use compact, default, or large point-size sets.
- File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed

- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.
- Split SD-card font descriptions and supported languages into separate lines in the font download screen.

### Fixed

- Inline EPUB images no longer disappear in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- SD-card font picker no longer reopens immediately after selecting a font from Settings > Reader > Font Family.
- In-reader font-size changes now work for SD-card fonts.
- In-reader SD-card font changes now rebuild the current EPUB page layout consistently.

## [v1.2.10] - 2026-05-11

### Added

- `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid.
- More flexible reader controls, including orientation-aware front/side button settings, nav-only or all-button front inversion, tilt page turn shortcuts, and side-button long-press rotation actions.
- Per-session auto page turn interval picker with values from 5 to 120 seconds.
- File Browser Home/Back long-press action for toggling hidden files and folders.
- EPUB rendering and diagnostics improvements, including visible `<hr>` separators and heap logs around section rebuilds, image extraction, page serialization, and sleep-cache rebuilds.
- Reader font coverage for block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation (PR #104).
- Simulator tools for testing sleep/wake behavior and smoke-testing common screens and EPUB reader menus.

### Changed

- Reduced Controls settings section spacing so the grouped controls fit better on X3 screens.
- Made front reader long-press actions trigger when the hold delay is reached while normal page turns still trigger on release.
- Used the fast EPUB spine/TOC indexing path for books with 300+ spine entries so heavily split books build `book.bin` faster on first open.
- Allowed the web file manager and WebDAV to browse dot-prefixed hidden files when hidden files are enabled, matching the device file browser.

### Fixed

- Reader button and shortcut behavior, including X3 power-button wake filtering, folder delete long-press timing, and WiFi scan/connect screens that could not be exited while work was in progress.
- RoundedRaff home-menu, keyboard, and button-hint rendering issues so Settings remains reachable and compact labels no longer overlap or disappear.
- Font and glyph handling now reduces persistent SD-card font advance-cache memory, releases optional font caches before image extraction only when heap is tight, and shows a visible replacement symbol when compact UI fonts lack `U+FFFD`.
- KOReader Sync authentication diagnostics and an in-reader sync crash, including clearer handling when a server or proxy returns non-JSON content.
- EPUB text rendering for redactions, whitespace-only XHTML text nodes, simple black CSS span backgrounds, list bullets in `<li><p>...</p></li>` items, and very long base64-like text runs.
- EPUB image, thumbnail, and section-rebuild stability so image-heavy chapters use less temporary memory, scale images more reliably, avoid stale dimensions, and suppress optional image work earlier under heap pressure.
- EPUB low-memory and cache safety now skips optional next-chapter indexing and sleep-page cache rebuilds when heap is tight, fails safely with a malformed-book warning and Home exit path, rebuilds incompatible fork-written caches, and handles low-memory CSS parsing, truncated SD writes, invalid serialized strings, and failed temp-cache promotion.
- Home no longer crashes after clearing reading cache when the source EPUB cache is missing.
- Reader prewarm behavior now skips image decoding, keeps mixed-style font glyphs cached together, and avoids section rebuilds for render-quality-only option changes.
- Concurrent render/storage crashes are avoided by serializing `GfxRenderer` scratch-buffer access, shared SPI bus access, and failed SPI lock cleanup.
- Recent Books, EPUB/XTC thumbnail caches, deleted-folder metadata, and XTC cover scaling now keep cached book data in sync and grid covers fill their slots correctly.
- Simulator build configuration now lets SDL2 and simulator-provided network/OTA shims compile cleanly.

---

## [v1.2.9.1] - 2026-05-03

### Changed

- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed

- Power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions.
- Potential crash when using `Go to %` in EPUBs.
- Potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid.
