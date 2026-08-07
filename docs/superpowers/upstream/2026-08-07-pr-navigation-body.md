## The problem

Opening a folder in the web File Manager does a full page reload: every folder link and
breadcrumb segment is a plain `<a href="/files?path=...">`. That means clicking into a
subfolder re-downloads the entire Files page — about 215 KB of HTML/JS, ~50 KB gzipped —
just to show a different folder listing. The actual folder contents come from a separate,
already-efficient `/api/files` endpoint; the page shell around it never needed to be re-sent
at all for a navigation that only changes which folder is displayed.

## Related to #2560

#2560 (open since 2026-07-09) adds ETag/304 and `Cache-Control` to the page-HTML responses,
so that when the shell *does* need to be re-fetched, it's cheap. This PR takes the
complementary angle: it makes most folder navigation skip re-fetching the shell entirely.
Neither replaces the other — a first page load (or a load after a firmware update) still
needs the full shell, and that's where #2560's caching pays off; every folder click *after*
that first load is what this PR removes the network round trip for.

The two PRs touch disjoint files: #2560 changes `scripts/build_html.py` and
`src/network/CrossPointWebServer.cpp`; this one only touches
`src/network/html/FilesPage.html`. They should merge cleanly in either order, and the wins
compound — a cheaper-to-refetch shell that you also rarely need to refetch.

## What changed

**Client-side folder navigation.** Folder links and breadcrumb links are now intercepted
(via delegated click listeners on the file-table and breadcrumb containers, so newly rendered
rows are covered automatically) and routed through a `navigateTo()` function instead of a
full page load: it updates `currentPath`, the document title, and the URL (`history.pushState`),
then re-fetches just that folder's listing from `/api/files` and re-renders the table. A
`popstate` listener makes the browser Back/Forward buttons work the same way. Middle-click,
Ctrl/Cmd-click, and Shift-click are explicitly left alone (checked before intercepting) so
opening a folder in a new tab or copying its link still works exactly as before — the real
`href` is still in the markup.

**One-time init.** A few things that used to run inside `hydrate()` — and so used to run
only once per full-page load — would otherwise now re-run on every folder click, since
`hydrate()` is the function that does the re-fetch-and-render. Two of them needed to move to
a one-time setup instead:
- The modal-overlay dismiss-on-click-outside wiring. Those overlay elements live outside the
  file-table/breadcrumb containers that `hydrate()` rewrites, so they persist across every
  navigation — left in place, this would have added one more `click` listener per overlay per
  folder click, forever.
- The `/api/status` version fetch. It doesn't depend on which folder you're in, so re-running
  it per click would just be a wasted round trip on every navigation.

**A staleness guard.** Client-side navigation removes the "free abort" a full page reload used
to give: two folder clicks in quick succession (or a fast Back-button tap) can now have two
`/api/files` fetches in flight at once, and the server doesn't guarantee they resolve in the
order they were sent — a large folder scan takes measurably longer than a small one. Without a
guard, an older, slower response could land after a newer one and overwrite the listing with
stale content. `hydrate()` now tags each call with a generation counter and bails out (without
touching the DOM) if a newer call has started by the time an `await` resolves.

## Before / after

- Before: clicking into any folder re-downloads the full Files page shell (~215 KB HTML,
  ~50 KB gzipped over the wire), every time, in addition to the actual folder data.
- After: a folder click does one `/api/files` request for just that folder's entries — the
  page shell (HTML/JS/CSS) isn't touched at all for navigation within an already-loaded page.

## How to verify

1. Load the Files page, open your browser's Network tab, and click into a folder with a few
   items.
2. Confirm there's no `files` (HTML) request for that click — only a `GET /api/files?path=...`
   request, which is small and specific to that folder.
3. Click Back. The listing returns to the parent folder without a page reload, and the URL bar
   reflects it.
4. Middle-click (or Ctrl/Cmd-click) a folder — it opens in a new tab as before.
5. Click through a few folders quickly (or spam Back/Forward) and confirm the listing shown
   always matches the current URL — no flash of a stale folder's contents.

## What I have and haven't tested

The equivalent change is running on my own device (an X3), in a fork build I flashed today.
Before it, browsing was unpredictable: the same click into the same folder was sometimes
instant and sometimes took several seconds, with no pattern I could see from the outside —
which is what sent me looking in the first place. After it, folder browsing is immediate,
every time. That's real hardware evidence that the navigation behavior itself works and is
worth having.

What I have *not* done is run this specific port on hardware. It's built cleanly against the
real firmware target here (`pio run -e default`), but I haven't flashed this exact tree to a
device and watched it behave. Given how close it is to what's already running on my device,
I'd expect the same result, but that's an expectation, not something I've confirmed for this
PR — worth a maintainer check before merge.

## AI usage disclosure

This change was written with AI assistance (porting a fix that's been running on my own
device into this tree) and I have not reviewed it line by line myself. What I can vouch for
directly is the behavior: it's running on my device right now, and the difference in how
folder browsing feels is dramatic.
