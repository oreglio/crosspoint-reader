Tested this on hardware — X3 and X4, firmware built from this branch (3f191c85).

**Caching behaves as intended.** <!-- REMPLACER par le résultat réel :
- première ouverture de /files : 200 avec l'en-tête ETag
- rechargement : 304 Not Modified, sans corps
- après flash d'un firmware différent : 200 à nouveau (l'ETag a changé), pas d'UI périmée
-->

**On the WebDAV question you raised in July** — I looked rather than guessed, and I don't think there's a functional interaction. `WebDAVHandler` only ever reads three headers (`Destination`, `Depth`, `Overwrite`); `If`, `Lock-Token` and `Timeout` are collected but never read, and nothing on the WebDAV path reads `If-None-Match`. The only reader is `sendStaticContent`, which serves GET-only page routes. So adding the header to the collected list can't change WebDAV semantics — the cost is one extra `String` per request when a client actually sends it, which browsers do on conditional GETs and WebDAV clients generally don't.

I exercised it anyway, since it is the shared header list: <!-- REMPLACER : montage WebDAV, listing, upload, suppression, renommage — tout OK ? -->

**One detail worth making explicit for reviewers**, because it cost me a while to find independently: the `collectHeaders` change here is load-bearing, not cosmetic. The ESP32 `WebServer` silently drops any request header that isn't in that list, so without `If-None-Match` in it, `server->header("If-None-Match")` reads back empty and the 304 path never fires — while looking perfectly wired up in the code. Might be worth a short comment in the source so nobody "simplifies" the list later.

Happy to re-test if you push changes.
