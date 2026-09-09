# libxlio / TLS offload

Language for how kernel TLS and XLIO's own TLS offload relate, and where each one's state lives — written up after debugging why nghttpx+XLIO zero-copy RX (`shrpx_http2_session.cc`'s `read_tls_zcopy()`) fell back to `SSL_read` with `ENOTSUP` even though the TLS handshake completed fine.

## Language

**kTLS (kernel TLS)**:
Linux's own `net/tls` record-layer offload, entered via `setsockopt(SOL_TLS, TLS_TX/TLS_RX, ...)`. OpenSSL issues this call itself when built `enable-ktls` and `SSL_OP_ENABLE_KTLS` is set (nghttpx: `--tls-ktls` / `tlsconf.ktls`). Success is visible system-wide in `/proc/net/tls_stat` (`TlsTxSw`/`TlsRxSw` for software crypto, `TlsTxDevice`/`TlsRxDevice` for NIC inline-crypto offload). Entirely independent of XLIO — verified directly with plain `openssl s_client`/`s_server` and with plain `nghttpx`/`nghttpd` (no `--xlio`), both of which activate kTLS RX and TX correctly regardless of which NIC is present.
_Avoid_: reading `/proc/net/tls_stat` as a signal about XLIO's own TLS offload — see next entry.

**XLIO uTLS (`XLIO_UTLS_TX`/`XLIO_UTLS_RX`)**:
XLIO's *own* userspace TLS offload, unrelated to kTLS except that it's triggered by intercepting the exact same `setsockopt(SOL_TLS, TLS_TX/TLS_RX, ...)` call OpenSSL makes (`sockinfo_ulp.cpp`, `sockinfo_tcp_ops_tls::setsockopt`). When XLIO owns the socket, that call is handled entirely inside XLIO's ring/NIC datapath and never reaches the real kernel — so `/proc/net/tls_stat` staying flat does **not** mean XLIO's offload failed; it can just as easily mean XLIO consumed the call itself (success or failure, kernel counters don't move either way).
_Avoid_: treating `TlsTxSw`/`TlsRxSw` growth as proof XLIO's uTLS is working — on an XLIO-managed socket it's proof the opposite happened (XLIO didn't intercept, kernel did the offload).

**UTLS_RX socket**:
XLIO's per-socket state after a successful `TLS_RX` offload setsockopt: incoming RX buffers get a `tls_type` stamp (from `sockinfo_tcp.cpp`'s `recv_zc_impl`, `head->rx.tls_type`) set by the NIC hardware path. `tls_type == 0` means this socket is *not* a UTLS_RX socket (plain ciphertext, or software-kTLS plaintext that went through the kernel rather than XLIO's ring) — `xlio_recv_zc_fd()` returns `ENOTSUP` in that case, and shrpx's `read_tls_zcopy()` (`shrpx_http2_session.cc`) falls back to `SSL_read` permanently for that connection. This detection is reactive by design: nothing checks UTLS_RX status before the read path switches over — the code comment in `shrpx_http2_session.cc` explicitly says the ENOTSUP handler *is* the detection mechanism.
_Avoid_: assuming the "XLIO zero-copy RX: enabled" log line at handshake time means the socket is actually UTLS_RX-capable — it only means the `xlio_recv_zc_fd` symbol was found via `dlsym`, not that this specific socket got hardware TLS RX offload.

**Ring HCA TLS capability gate**:
`sockinfo_tcp::is_utls_supported(direction)` requires **both** the env var (`XLIO_UTLS_TX`/`XLIO_UTLS_RX`, default TX=on/RX=off) **and** `ring->tls_tx_supported()`/`tls_rx_supported()` to be true. The ring booleans (`m_tls.tls_tx`/`m_tls.tls_rx` in `ring_simple.cpp`) are populated once at ring construction from `dpcp::adapter_hca_capabilities` (`caps.tls_tx`/`caps.tls_rx`), which in turn reflects the NIC port's actual driver/firmware TLS offload state.
_Avoid_: assuming `XLIO_UTLS_RX=1` in the env is sufficient — it's necessary but not sufficient; the ring capability bit gates it independently, and that bit is a real hardware/driver fact, not something XLIO or an env var can force on.

**Root cause found (2026-09-07)**: the ConnectX-7 ports on this box have TLS hardware offload disabled at the driver level, confirmed directly (bypassing XLIO/dpcp entirely) with:
```
ethtool -k enpCX7_1 | grep tls
tls-hw-tx-offload: off [fixed]
tls-hw-rx-offload: off [fixed]
tls-hw-record: off [fixed]
```
`[fixed]` means the driver reports this as not even toggleable — `ring.tls_tx_supported`/`tls_rx_supported` (and the `dpcp::adapter_hca_capabilities` query behind them) were correctly reporting `false`; there was no bug in XLIO's capability check or in the `is_utls_supported()` gating logic. **Fix**: use a NIC/port that has TLS offload enabled instead (check with the same `ethtool -k <iface> | grep tls` one-liner before assuming any port will do). The user has since moved traffic to such a card and XLIO uTLS now engages — a *different*, not-yet-characterized error now surfaces past that point (open item, not yet investigated in this doc).

**probnik `"zc-trace"` tag**:
The `PROBNIK_LOG(level, "zc-trace", fmt, ...)` convention (from `subprojects/probnik`) used to trace the zero-copy RX / TLS-offload decision path. As of this investigation it covers: `is_utls_supported()`'s per-direction decision (`sockinfo_tcp.cpp`), and the full `TLS_RX` `setsockopt` path in `sockinfo_ulp.cpp` (pre-checks, `g_tls_api` null check, TX/RX ring context mismatch, TIR creation from TX-cache vs fresh RX ring, `tls_context_setup_rx` return code, SQ credit exhaustion, final active/failed outcome) — added specifically to find where XLIO's RX offload setup breaks down, in addition to the pre-existing `recv_zc_impl` trace (`tls_type=0 -> ENOTSUP`) that was the only one before this session.
_Avoid_: relying on `/proc/net/tls_stat` or the shrpx INFO log alone to debug this path — grep the probnik `zc-trace` output instead; it is the only place both kTLS and XLIO-uTLS decisions are visible in one stream. Fastest first check before diving into logs at all, though: `ethtool -k <iface> | grep tls` on whichever interface is actually carrying the traffic.

**`XLIO_INTERFACE` config/hardware mismatch**:
`configs/xlio/shared-nghttpx` sets `XLIO_INTERFACE=ethBF3_0`, which does not match any interface present on this dev box (`ip addr` shows `enpCX7_0`, `enpCX7_1`, `enpBF2_0` — ConnectX-7 + BlueField-2, not BlueField-3). Looks like a leftover from a different test rig; not yet confirmed whether it actually affects which ring gets used, but it is a genuine, fixable config bug independent of the TLS capability investigation above.
_Avoid_: assuming the vars file matches the current box without checking `ip addr` first when moving between machines.

## Zero-copy mechanisms (three distinct things, one overloaded word)

"Zero-copy" in this codebase names three independent mechanisms. Bugs get misdiagnosed when a fix targets the wrong one — name the mechanism, not just "zero-copy," when describing a bug or a fix here.

**RX zero-copy**:
`xlio_recv_zc_fd()`/`recv_zc_impl` exposing XLIO's DMA receive buffers directly to nghttp2, eliminating the copy inside `SSL_read`. Gated by UTLS_RX socket state (see "UTLS_RX socket" above). Gives nghttp2 a pointer into XLIO's buffer, nothing more — it does not by itself get that data anywhere else.
_Avoid_: calling any bug "an RX zero-copy bug" when it's actually in how the received buffer is *relayed* or *sent* afterward — see the next two entries.

**ZC body relay**:
`Downstream::zc_body_queue_`/`push_zc_body()`/`pop_zc_body()`/`zc_body_empty()` (`shrpx_downstream.h`, populated in `shrpx_http2_session.cc`, consumed in `shrpx_http2_upstream.cc`'s `send_data_callback`). The mechanism that carries an RX-zero-copied backend response body across the application — from the backend `Http2Session`'s RX buffer straight into a client-facing HTTP/2 DATA frame — without ever landing in `Downstream::get_response_buf()`. This is the layer where RX zero-copy (backend leg) and Ultra API TX (frontend leg) have to agree with each other; when they don't, you get the bug described under "Bug: ZC body relay / Ultra API TX disagree" below.

**Ultra API TX**:
`XlioAdapter::has_ultra_tx()`/`socket_from_fd()`/`sendv_inline()`/`sendv_zc()` (`shrpx_xlio.h`) — a separate XLIO socket handle (`conn_.xlio_sock`, obtained via `xlio_socket_from_fd()`) used to transmit via the Ultra API instead of a plain `write()`/`SSL_write()`. Currently acquired unconditionally on the frontend after handshake (`shrpx_client_handler.cc`) and used for: (a) `write_clear`'s cleartext INLINE sends, (b) `write_tls`'s "Level 1 experiment" TLS-TX path (`shrpx_connection.cc:531-570`), and (c) the ZC body relay's `sendv_zc()` payload sends. Each of (a)/(b)/(c) is a separate consumer with separate correctness requirements — see next section.

## Bug: ZC body relay / Ultra API TX disagree on eligibility, and on protection domain

Two separate reasons the ZC body relay's send can silently fail or corrupt a frame, both now guarded in `shrpx_http2_upstream.cc`'s `send_data_callback`:

1. **`data_read_cb` vs `send_data_callback` disagree on ZC-eligibility** (found 2026-09-07): `data_read_cb` (`shrpx_http2_session.cc:1825`) decides a chunk is available purely from `!downstream->zc_body_empty()`, with no visibility into whether the frontend connection has a usable Ultra TX socket. `send_data_callback` is where that's actually checked (`csock && xa.has_ultra_tx()`) — when it's false, nghttp2 has already committed to a DATA frame length that the copy-path fallback then can't fill (it drains `body`, not `zc_body_queue_`), producing a truncated/corrupted frame.
2. **Same-protection-domain requirement, previously unchecked** (found and fixed 2026-09-07): `send_data_callback` hands `ref.buf` — an RX buffer registered under the *backend* connection's XLIO protection domain — directly to `sendv_zc()` on `csock`, the *frontend* socket. Per `xlio_socket_get_pd()`'s doc ("this can be used for registering memory regions for zero-copy"), this only works if both sockets share a PD, i.e. are on the same XLIO ring/NIC port. Nothing checked this. Added a guard: `send_data_callback` now calls `xa.socket_from_fd()` on the backend `Http2Session`'s fd (new `Http2Session::get_fd()` accessor) purely to read its PD via the new `XlioAdapter::get_pd()`, and compares it against the frontend socket's PD before attempting the relay; on mismatch it falls into the same MISMATCH-logged copy-path branch as reason 1.

_Avoid_: assuming a same-PD topology is guaranteed by config — nothing in XLIO enforces it, and `XLIO_INTERFACE` (see below) never actually pinned it either.

**Confirmed live (2026-09-07): `same_pd` really is false in the current topology, and the "fall through to copy path" fallback was itself broken — fixed by moving the eligibility check earlier**

First real data point (after the Level-1 TLS-TX fix above unblocked requests from completing): `csock=1 has_ultra_tx=1 same_pd=0 backend_pd=(nil)`. So frontend and backend genuinely aren't on the same protection domain right now (or the backend fd isn't convertible to an Ultra handle at all — `backend_pd` came back null even trying) — a real, live instance of the topology gap, not a hypothetical.

But hitting that path exposed a second bug: the guard correctly refused to `sendv_zc()`, fell into the documented "copy path," and the client's DATA frame still never arrived — hang, not corruption this time, matching reason 1 above almost exactly (`data_read_cb` had already committed nghttp2 to a 1024-byte frame via `zc_queue=1024 body=0`; the copy-path fallback drains `body`, which is empty). This is the same root problem as reason 1, just triggered by the PD check instead of the `csock`/`has_ultra_tx` check — confirms the two reasons above are really one bug: **the decision to queue into `zc_body_queue_` and the decision to actually send it that way must be the same decision**, not two checks running at two different times that can disagree.

_Fix applied_: added `zc_relay_eligible(Http2Upstream*, Http2Session*)` (`shrpx_http2_upstream.cc`, anonymous namespace, checks `csock`/`has_ultra_tx`/same-PD in one place) and moved the check to where the decision actually originates — `on_downstream_body()`'s `try_claim_zc_buf()` call is now skipped entirely (falling through to the existing, already-correct copy-body-append path) whenever `zc_relay_eligible()` is false, so `zc_body_queue_` is simply never populated with bytes the frontend can't relay. `send_data_callback()` now calls the same helper as a defense-in-depth check rather than reimplementing the PD/eligibility logic inline. Builds clean, both variants.
**Confirmed live (2026-09-07): `same_pd` false was a real topology fact, not a code bug — traced to the frontend listening on the wrong physical port**

`zc_relay_eligible` breakdown logging (added to distinguish the three possible causes) showed `backend_sock=1 backend_pd=0x...700 csock=1 csock_pd=0x...640` — both handles valid, both PDs non-null, but genuinely different pointers. Cross-referencing this box's actual topology (`ip addr`, `ip route`, `rdma link show`, `ethtool -i`) found: `enpCX7_0`/`enpCX7_1` (ConnectX-7, PCI `8a:00.0`/`.1`) map to `mlx5_0`/`mlx5_1`, and `enpBF2_0`/`enpBF2_1` (BlueField-2, PCI `99:00.0`/`.1`) map to `mlx5_2`/`mlx5_3` — **four separate RDMA devices**, each with its own PD; not just separate across the two physical cards, but separate between the two ports of the same card too. The client's request showed `:authority: 10.7.0.1:8443` — the frontend LB listener was on `enpCX7_0`, the exact port already confirmed (earlier in this doc) to have TLS hardware offload `[fixed] off` — while the backend connection's `is_utls_supported` trace showed `tls_rx/tx_supported=1`, meaning it was on one of the BlueField-2 ports. Two different physical HCAs: `sendv_zc()` could never have worked here regardless of any software fix. Also confirms the config file's own topology comment (`client 1.1.1.2 → LB 1.1.1.1 → backend 1.1.1.4`) describes IPs that don't exist on this box at all (`ip route get 1.1.1.4` resolves via the plain management NIC `eno8303`, not any XLIO-capable interface) — yet another stale/fictional leftover, unrelated to the real `10.7.x`/`10.2.x` topology actually in use.

_Resolution_: moving the frontend listener onto the same BlueField-2 port the backend already uses (confirmed: traffic now via `mlx5_2`) makes `same_pd` achievable. Not a code fix — a deployment/topology fix. Worth updating the config file's topology comment to the real addresses once settled, so the next person doesn't chase the fictional `1.1.1.x` addresses again.

**Second bug found immediately after fixing topology (2026-09-07): send-time eligibility recheck raced against backend connection-pool detachment, always reporting ineligible for already-correctly-queued chunks**

With frontend and backend finally on the same port, a 10K response showed `ZC body queued: len=10091`/`len=149` (`zc_relay_eligible` true at queue time, in `on_downstream_body`) — but every subsequent `send_data_callback` for that same data logged `INELIGIBLE-AT-SEND`, falling to the copy path every time and hanging/corrupting exactly like the pre-fix bug. Root cause: nghttp2 detaches the per-request `DownstreamConnection` back into the backend connection pool as soon as the backend response is fully read (`Detaching from DOWNSTREAM` / `DCONN Deleted` in the log), **before** `send_data_callback` finishes emitting all the DATA frames for that response to the client. `send_data_callback`'s defense-in-depth recheck walked `downstream->get_downstream_connection()` to re-derive the backend and its PD — which is null after detachment — so it always failed, even though every byte in `zc_body_queue_` had already been correctly vetted once, before detachment could happen.

_Fix applied_: `send_data_callback` no longer re-derives backend/PD eligibility at all. It only checks that the *frontend* socket (`csock`/`has_ultra_tx()`) is still usable — the one thing that actually can change between queue and send time. Backend-side eligibility (PD match) is checked exactly once, at `on_downstream_body`, before anything is queued; nothing after that point can invalidate an already-queued chunk from the backend side. `zc_relay_eligible()` remains the single source of truth, but is now only called from the one call site where the backend reference is guaranteed live. Builds clean, both variants.
_Not yet verified live_: this fix hasn't been exercised on hardware yet — next step is confirming a 10K (and larger) response completes with real `send_zc_frames`/`send_zc_bytes` growing in `dump_stats`, not `copy_queued_bytes`.

**`XLIO_INTERFACE` is not a real XLIO config variable — removed (2026-09-07)**: it doesn't appear anywhere in libxlio's source or its config schema; it never took effect. `configs/xlio/shared-nghttpx` and the `-multiworker-WIP` sibling both set it, matching neither this box's real interfaces (`enpCX7_0`/`enpCX7_1`/`enpBF2_0`) nor anything XLIO reads — it was dead config from the start, not just stale for this box. Removed from both files. XLIO picks up which NIC to accelerate via normal socket binding/routing, not an env var — so the same-PD requirement above has to be satisfied by making sure both the frontend listen address and the backend connect address actually route over the same physical interface, not by setting a variable.

## Correction (2026-09-07): the "acquiring the Ultra handle disturbs a live read" theory is likely wrong — the real suspect is the very next *write*

The investigation history below (kept for the record) settled on "calling `xa.socket_from_fd()` on a live fd disturbs a subsequent `SSL_read()`" as the cause of the frontend `tlsv1 alert protocol version`. Re-reading the code surfaced a direct contradiction the original investigation didn't have in view yet:

- `shrpx_client_handler.cc:251-254`'s comment claims that for TLS connections, `conn_.xlio_sock` is set but *unused* — `write_tls` "still uses `SSL_write`."
- But `shrpx_connection.cc:531-570` (`write_tls`), labeled **"Ultra API TLS TX path (Level 1 experiment)"**, routes every post-handshake write through `xa.sendv_inline(xlio_sock, ...)` whenever the handle is non-null — directly contradicting the comment above it in a different file.

The trace evidence (`SSL_read: ... tlsv1 alert protocol version`, `role=upstream`) is us **receiving an alert the client sent us** — consistent with the very next write after handle acquisition (shrpx's first post-handshake write, the HTTP/2 SETTINGS frame) going out through this Level 1 experiment path malformed, and the client's TLS stack alerting back. That's a **write-framing bug in the Level-1-experiment TX path**, not a "the read path gets disturbed by acquisition itself" bug — those two stale, contradictory comments are themselves an example of the "leftover from a previous zero-copy layer" pattern this file exists to untangle.

_Decision (2026-09-07)_: Ultra API TX for TLS (the Level 1 experiment) is out of scope for "get zero-copy L7LB working again." Plan is to disable/remove it rather than debug XLIO's `sendv_inline` TLS-framing path — TLS writes stay on plain `SSL_write`, only RX zero-copy + ZC body relay need to work for now.

**Confirmed (2026-09-07, decisive live test): the Level-1 write path is the cause, not handle acquisition — fixed by removing it**

Added `tls-resume` `CALL`/`RETURN` tracing around the `sendv_inline()` call itself (the one thing missing from the earlier instrumentation) and re-ran with the Ultra handle genuinely acquired (previous run was confounded — see "diagnostic switch" note below) and the Level-1 branch still live. Two consecutive runs nailed the sequence:

1. Handshake completes, handle acquired (`xlio_sock=1`).
2. `write_tls Level-1 CALL ... len=33` → `RETURN rc=0` — shrpx's own post-handshake HTTP/2 SETTINGS frame goes out via `sendv_inline()`, reported success.
3. Client's HEADERS read succeeds right after (`rv=116`) — this is *not* evidence the write was fine, just that the client had already sent its own HEADERS independently over the duplex connection before it could have received/parsed shrpx's bad output.
4. **No further write_tls call of any kind** happens on this fd before the failure.
5. A 24-byte record arrives from the client; `read_tls` decodes it and fails: `SSL_read: error:0A00042E:SSL routines::tlsv1 alert protocol version`.

With zero writes between steps 2 and 5, the alert can only be the client's own reaction to what shrpx sent in step 2. Conclusion: `sendv_inline()`'s assumption that XLIO transparently detects UTLS_TX and applies TLS record framing/encryption in hardware did not hold for this socket — the 33 bytes went out as raw, unencrypted, unframed application bytes. The client's TLS stack, parsing those bytes as a TLS record, found a bogus version field and correctly sent back a fatal `protocol_version` alert before closing. shrpx's "spurious" alert is that alert, arriving one round-trip after the bad write — not a read-path bug at all.

_Fix applied_: removed the Level-1 experiment branch from `Connection::write_tls()` (`shrpx_connection.cc`) entirely. TLS writes now unconditionally use `SSL_write()`; `xlio_sock` is still acquired and still used for `write_clear` (h2c) and the ZC body relay's `sendv_zc()` sends, which are unaffected. Builds clean (`./scripts/compile.sh --nghttp2` and `ZC_BODY=1 ./scripts/compile.sh --nghttp2`).
_Not yet confirmed_: whether the underlying assumption (`sendv_inline` auto-detecting UTLS_TX) is simply wrong in general, or only failed because UTLS_TX was never actually engaged for this specific frontend socket (e.g. kTLS `setsockopt` never issued, or `--tls-ktls` not passed to nghttpx for this run) — doesn't matter for the current fix, but matters if TLS-TX zero-copy is revisited later; whoever does that should confirm real UTLS_TX engagement (via the `is_utls_supported` zc-trace line, keyed to the frontend fd) before trusting `sendv_inline` to add framing.
_Avoid_: reading the "second live run" / "Confirmed... the switch fixes the alert" entries below as proof the *acquisition* itself was the cause — the A/B switch used there (`XLIO_SKIP_TLS_ULTRA_HANDLE=1`) disabled both acquisition and the Level-1 write path together, so it couldn't distinguish between the two. This entry supersedes that conclusion: acquisition is fine, the write path was the actual bug, and it's now removed rather than gated.

**Diagnostic-switch bug found along the way (2026-09-07)**: `XLIO_SKIP_TLS_ULTRA_HANDLE`'s check (`shrpx_client_handler.cc`, now moot since the switch's only purpose — gating the now-removed Level-1 path — no longer exists, but worth remembering the pattern) was `std::getenv(...) != nullptr` — presence-based, not value-based. Setting `XLIO_SKIP_TLS_ULTRA_HANDLE=0` intending "off" still skipped acquisition, because the var was still *set*. First re-run attempt was confounded by this until the var was unset entirely rather than set to `0`. Worth remembering for any future ad-hoc env-var diagnostic switches in this codebase: match the naming convention's implied semantics (`=0`/unset means off) or don't use `0`/`1` values at all.

## Open items for the next agent

- ~~`XLIO_INTERFACE=ethBF3_0` in `configs/xlio/shared-nghttpx` still doesn't match this box's interfaces~~ — resolved 2026-09-07: it wasn't a real XLIO variable at all, removed from both config files (see "`XLIO_INTERFACE` is not a real XLIO config variable" above). Still need to confirm both frontend and backend addresses actually route over the same interface on this box, by ordinary routing rather than an env var.
- Run the experiment: does `same_pd` in the new guard (`send_data_callback`) actually come back true on this box's current topology? Tells us whether the PD mismatch was ever live here or the tests only hit the eligibility-check bug.
- Test the write-framing theory (previous section) before touching libxlio: force `write_tls`'s Level-1-experiment branch off while leaving the Ultra handle acquisition on, see if the alert still reproduces.

**"Different error" from the item above, now characterized (2026-09-07): single request hangs forever, not an RX bug**

Traced via `zc-trace` on a request that never completes. Sequence on the downstream (backend) HTTP/2 session:

1. Ready list gets 3 records: two TLS 1.3 `NewSessionTicket`s (`tls_type=22`) then one 21-byte application-data record (`tls_type=23`).
2. `recv_zc_impl` (`sockinfo_tcp.cpp:2641`) only inspects the **head** of `m_rx_pkt_ready_list`. Head is a handshake record, so it returns `ENODATA` for the *entire* batch — it never looks past the head to notice the app-data record queued right behind it.
3. `read_tls_zcopy()`'s `ENODATA` branch (`shrpx_http2_session.cc:2246`) is designed for exactly this: falls back to `read_tls()`/`SSL_read`, which drains all 3 records off the same list (`m_rcvbuff_current` 135 → 0) and feeds them to nghttp2 normally. This part is working as intended, not a bug.
4. One more packet arrives; the offload engine has to `RESYNC` (`sockinfo_ulp.cpp` `RESYNC PSV`/`RESYNC reprogram`, matched via `hw_resync_tcp_sn`) because the previous records were drained through the plain-recv/`SSL_read` path instead of the UTLS_RX zero-copy path, so the hardware's expected-record-number tracking fell behind. Resync succeeds and correctly decrypts a 9-byte app-data record (`recno=3`) — this is almost certainly the backend's initial (empty) `SETTINGS` frame, hence `zc_claim_count_=0` ("no DATA, release_zc") is expected, not an error.
5. After that: `m_rx_pkt_ready_list` stays permanently empty, `recv_zc` returns `EAGAIN` forever, and the idle-connection timer eventually fires (`shrpx_http2_session.cc:88`, "connection check required"). The backend never sends anything else.

_Conclusion_: the RX zero-copy path (ENODATA fallback, resync, ZC delivery) is behaving correctly through all of this — every byte the backend actually sent gets accounted for. The hang means the backend is never sending its actual response, which points at the **TX side** of this downstream connection, or (see below) at a request never being formed in the first place because the connect/handshake itself failed.
_Avoid_: assuming the `ENODATA`/resync trace lines themselves indicate a bug — they're the designed fallback and diagnostic path, not evidence of failure on their own; the actual signal is the RX list going permanently empty while the request never completes.

**Actual root cause found (2026-09-07, same day, supersedes the "trace TX side" next-step above): spurious backend TLS alert during session-resumption connect, not a TX bug**

Real culprit surfaced in a plain shrpx log (no zc-trace needed) for a failing request:

```
INFO (shrpx_http2_session.cc:448) Connecting to downstream server
INFO (shrpx_connection.cc:740) SSL_read: error:0A00042E:SSL routines::tlsv1 alert protocol version
INFO (shrpx_client_handler.cc:606) [CLIENT_HANDLER:...] Deleting
...(client handler / downstream / dconn all torn down)...
INFO (shrpx_http2_session.cc:1999) [DHTTP2:0x...] Connection established
INFO (shrpx_connection.cc:398) SSL/TLS handshake completed
```

The tell: the *same* `Http2Session` object that produced the `protocol_version` alert goes on to finish its TLS handshake successfully right after. A real fatal alert from the backend would kill the whole TCP connection — it wouldn't finish handshaking next. So the alert only kills the one request/downstream attached at connect time (cascades through `on_downstream_reset` → client handler deleted), while the connection itself recovers.

This lines up with [shrpx_http2_session.cc:474](../nghttp2/src/shrpx_http2_session.cc#L474):
```cpp
auto tls_session = tls::reuse_tls_session(addr_->tls_session_cache);
if (tls_session) { SSL_set_session(conn_.tls.ssl, tls_session); ... }
```
`addr_->tls_session_cache` is exactly the cache populated by the `NewSessionTicket` records seen earlier (the "Update client cache entry timestamp" / "Client session cache entry is still fresh" lines in `shrpx_tls.cc`'s `try_cache_tls_session`/`reuse_tls_session`, `shrpx_tls.cc:2406-2431`). Working hypothesis: a new backend connect attempts 1-RTT resumption using a ticket cached from an earlier connection; the backend rejects it with `protocol_version`, which only makes sense if the cached session's parameters are stale/mismatched by the time it's replayed — plausibly because those tickets are consumed via the `ENODATA`→`SSL_read` fallback path (see above) rather than a plain non-XLIO recv, and something in that path corrupts what ends up cached.
_Next step_: temporarily stub `reuse_tls_session()` (or clear `addr_->tls_session_cache` before use) for XLIO-backed downstream connections. If the alert disappears, the bug is confirmed to be in how the cached `SSL_SESSION`/ticket interacts with the XLIO uTLS RX path, not in TX or in the RX zero-copy delivery mechanics documented above (those were a red herring for this particular failure).
_Avoid_: chasing the RX zero-copy / TX-side hypothesis above for this failure mode — that trace was from a *different*, successfully-established connection; this alert happens at connect/handshake time on a *new* connection attempting session resumption, before any request-level TX/RX is relevant.

**Instrumentation added (2026-09-07) to test the session-resumption hypothesis — `tls-resume` probnik tag**

Added `PROBNIK_LOG(..., "tls-resume", ...)` calls (separate tag from `zc-trace`, enable independently) at every point in the session-cache round trip, to nail down exactly where the cached session diverges from what the backend will accept:

- `shrpx_tls.cc` `tls_session_client_new_cb`: logs `conn_proto` (live connection's negotiated protocol via `SSL_get_version`) vs `session_proto` (the new ticket's own protocol via `SSL_SESSION_get_protocol_version`, printed as hex — e.g. `0x304`=TLS1.3) and the cache pointer, every time the backend hands us a new ticket.
- `shrpx_tls.cc` `try_cache_tls_session`: logs whether the write was skipped (still "fresh", <1min old) or applied, plus `session_proto` and the serialized byte length, keyed by `cache` pointer — lets you correlate which `DownstreamAddr`'s cache got which ticket.
- `shrpx_tls.cc` `reuse_tls_session`: logs cache-empty (no resumption attempted), `d2i_SSL_SESSION` deserialization failure (cache corrupted), or success with the deserialized `session_proto` — this is the first place a mismatch between what was *stored* and what comes back *out* would show up.
- `shrpx_http2_session.cc` `initiate_connection` (~line 474, where `SSL_set_session` is called): logs the about-to-be-resumed `session_proto` against `ctx_min`/`ctx_max` (`get_config()->tls.min_proto_version`/`max_proto_version` — the downstream SSL_CTX's configured allowed range). **If `session_proto` ever falls outside `[ctx_min, ctx_max]`, that's the bug**: a ticket cached under a wider/older config being replayed against a now-narrower one, or a stale/wrong-protocol session slipping into the cache.
- `shrpx_connection.cc`, both `SSL_ERROR_SSL` branches (`tls_handshake()`'s `SSL_do_handshake` failure, and `read_tls()`'s `SSL_read` failure — the alert in the user's log came from the latter): logs `role` (downstream vs upstream, via whether `tls.client_session_cache` is set), `in_init`/`session_reused`/`had_session`/`session_proto` at the moment of failure, `initial_handshake_done`, and whether `xlio_sock` is set on this connection. This pins down (a) which connection role hit the alert, (b) whether a session had actually been attached via `SSL_set_session` before the failure, (c) its protocol version, and (d) whether `SSL_read` fired before the handshake had actually completed (`in_init=1`) — which would point at XLIO delivering a premature/spurious readable event rather than a genuine protocol mismatch.

Builds clean via `./scripts/compile.sh --nghttp2` (verified 2026-09-07).

**Correction (2026-09-07, same day, from first live run of the instrumentation): the alert is on the UPSTREAM (frontend, client→shrpx) connection, not backend session resumption**

Ran with `tls-resume` enabled and captured a live failing request:
```
reuse_tls_session cache=... EMPTY, no resumption attempted
no cached session, full handshake fd=-1 addr=...           ← downstream connect, clean, cache empty
...
[zc-trace] ring_slave.cpp:680: src_port=46806 dst_port=8443 seq=... sz=24 cum=743 tls_dec=0   ← NEW packet on the frontend listener (port 8443)
SSL_read: error:0A00042E:SSL routines::tlsv1 alert protocol version
read_tls SSL_ERROR_SSL fd=53 role=upstream in_init=1 session_reused=0 had_session=1
  session_proto=0x304 initial_handshake_done=1 xlio_sock=1 req_len=16384
```
This **rules out** the backend session-resumption hypothesis above: `reuse_tls_session` reports the cache was empty for this request's downstream connect (no resumption attempted at all on that side), so the earlier `SSL_SESSION`/ticket theory is dead for this failure instance.

The real anomaly: `role=upstream` — this is the **client-facing** connection (fd=53, the same fd tagged `CLIENT_HANDLER` in the plain log), not a backend `DHTTP2` session. And it shows a direct contradiction: `initial_handshake_done=1` (shrpx's own bookkeeping says this connection's TLS handshake completed successfully once already, at `TLS1.3`/`0x304`) but `in_init=1` at the moment of this `SSL_read` call (OpenSSL itself reports the connection is *back* in handshake/init state). A real, fully-completed TLS 1.3 connection should never re-enter `SSL_in_init()==true` on a plain data read. Two live hypotheses, not yet distinguished:
1. **XLIO fd/socket state reuse**: fd 53 was recently used by a *different*, already-closed connection, and XLIO's own per-fd ring/TLS bookkeeping (independent of shrpx's C++ `Connection` object, which genuinely is freshly constructed per accept — verified no pooling/freelist exists for `ClientHandler`/`Connection` on the frontend path) wasn't fully torn down before the fd number was reissued to this new client, so bytes or state from the old connection bleed into the new one.
2. **Genuine spurious `SSL_in_init()` flip** post-handshake (e.g. a real but garbled/misframed byte sequence — `tls_dec=0` on this connection means XLIO's hardware TLS decrypt path never engaged here at all, so these are plain bytes going straight to `SSL_read`) causing OpenSSL's state machine itself to misbehave.

**Instrumentation added (2026-09-07) to distinguish the two**, in [shrpx_connection.cc](../nghttp2/src/shrpx_connection.cc):
- `Connection::Connection` constructor: logs `fd`, `this`, `ssl` pointer, `proto` on every construction — establishes when a given fd number gets (re)claimed by a brand-new `Connection` object.
- `Connection::disconnect`: logs `fd`, `this`, `xlio_sock` right before `close(fd)` — pair with the constructor log to check the time gap between a given fd's close and its next reuse, and whether XLIO had time/opportunity to clean up.
- `Connection::tls_handshake()` success path: logs `in_init`, negotiated `proto` (`SSL_get_version`), `reused` (`SSL_session_reused`) at the exact moment `initial_handshake_done` is set — confirms baseline state right after a clean handshake for comparison against the later failing read.
- `Connection::read_tls()`: TRACE-level `CALL`/`RETURN` pair around every single `SSL_read()` on the plain (non-early-data) path — `in_init`, requested length, `SSL_pending()`, and `handshake_done` before the call, return value after. This exposes the full read sequence for a connection so you can see exactly which prior read (if any) preceded the `in_init` flip, rather than only the failing call.

_Avoid_: re-chasing the backend/`DHTTP2` session-cache hypothesis for this failure mode — live data shows the cache was empty on the backend side for this exact failing request; the earlier resumption theory may still be a real, separate issue but is not what's on display in this trace.

**Second live run (2026-09-07, same day) pinpoints the trigger: `xa.socket_from_fd()` called on a live fd mid-connection**

Full `tls-resume` TRACE capture of one failing frontend connection (fd=53, `this=0x7f825be37430`), in order:
1. `Connection::Connection constructed fd=53` — fresh object, no pooling/reuse (rules out the fd-reuse-staleness half of hypothesis 1 above for *this instance*: this fd wasn't recently freed and reissued within this trace window — no `disconnect closing fd=53` appears before it).
2. `tls_handshake COMPLETE fd=53 in_init=0 proto=TLSv1.3 reused=0 xlio_sock=0` — clean handshake, `xlio_sock` still unset at this point.
3. `read_tls CALL ... in_init=0 ... handshake_done=1` → `RETURN rv=116` — **first application-data read succeeds fine** (116 bytes, presumably the client's HTTP/2 preface+headers).
4. A second `Connection` gets constructed (`fd=-1`, `proto=2`) — this is the backend/`DHTTP2` connection being set up to serve the just-parsed request; unrelated fd, runs concurrently but on the same thread.
5. **Between step 3 and the next read**, `ClientHandler::tls_handshake()` (`shrpx_client_handler.cc:256`) calls `xa.socket_from_fd(conn_.fd)` on the frontend fd to acquire an XLIO "Ultra API" handle — this sets `conn_.xlio_sock` from `0` to a nonzero value for the *first time* on this connection. Per its own doc comment in [libxlio's `xlio_socket_from_fd`](../libxlio/src/core/sock/sock-extra.cpp#L489-L502) this is supposed to be "a lightweight read-only conversion" that "does not disrupt the connection" and doesn't touch the RX callback path — and per the caller's own comment ([shrpx_client_handler.cc:249-253](../nghttp2/src/shrpx_client_handler.cc#L249-L253)), for TLS connections this handle **isn't consumed by anything yet** (`write_tls` still uses plain `SSL_write`; it's future-proofing for a TX path that doesn't exist yet).
6. The very next `read_tls CALL ... in_init=0 ...` → `RETURN rv=0`, immediately followed by `SSL_ERROR_SSL ... in_init=1 ... xlio_sock=1` — **the first read after `xa.socket_from_fd()` runs is the one that fails**, on the exact same fd/SSL object that had just cleanly read 116 bytes moments earlier. `ERR_clear_error()` runs at the top of `Connection::read_tls()` (confirmed by reading the code — line 633), so this isn't a stale/leftover error from some unrelated OpenSSL operation; the error is real and specific to this `SSL_read()` call.

_Conclusion_: this is now the leading, most concrete hypothesis — acquiring the Ultra API handle on an fd that already has an in-progress, OpenSSL-owned plain-`recv()`-based read path appears to disturb that path, even though the handle is currently unused for TLS connections and its own documentation says it shouldn't. The cleartext (`h2c`) call site of the same function ([shrpx_client_handler.cc:578](../nghttp2/src/shrpx_client_handler.cc#L578)) is structurally different and not under suspicion: it runs immediately after accept, before any read has happened on that fd, so there's no "established read path" to disturb.

**Instrumentation added (2026-09-07)** in [shrpx_client_handler.cc](../nghttp2/src/shrpx_client_handler.cc):
- `tls-resume` `BEFORE`/`AFTER` log pair bracketing the `xa.socket_from_fd(conn_.fd)` call at the TLS call site, to pin the exact ordering relative to the `read_tls CALL`/`RETURN` trace lines already in place.
- **A/B diagnostic switch**: set env var `XLIO_SKIP_TLS_ULTRA_HANDLE=1` to skip acquiring the Ultra API handle entirely for TLS frontend connections (gated with `std::getenv`, checked alongside `xa.has_ultra_tx()`). Since this handle is currently unused on the TLS path, skipping it should be functionally inert if the hypothesis is wrong, and should make the alert disappear if `xa.socket_from_fd()` is in fact the trigger. **This is a temporary diagnostic, not a fix — remove once the hypothesis is confirmed or ruled out.**

Builds clean via `./scripts/compile.sh --nghttp2` (verified 2026-09-07).

_Next step_: run once normally (confirm the alert still reproduces) and once with `XLIO_SKIP_TLS_ULTRA_HANDLE=1` set in the environment. If the alert disappears with the switch on, the bug is conclusively in `xlio_socket_from_fd()`'s interaction with an fd that already has an OpenSSL-owned read in progress (next step would be reading `g_p_fd_collection->get_sockfd()`/`sockinfo_tcp` internals in libxlio for a lock, state flag, or receive-queue side effect the doc comment doesn't account for). If the alert still reproduces with the switch on, this hypothesis is dead and the next place to look is genuine TLS 1.3 post-handshake state (e.g. a real second flight from the client arriving unusually, or a true `SSL_in_init()` quirk unrelated to XLIO at all).

**Confirmed (2026-09-07, third live run with `XLIO_SKIP_TLS_ULTRA_HANDLE=1` set): the switch fixes the alert. Traffic gets much further — and immediately hits a second, separate, pre-existing bug.**

With the switch on, the `protocol_version` alert is gone and the request makes real progress (backend response comes back, HEADERS reach the client). But the response body never completes at the client — matches "first response header came back but the request didn't finish." Comparing a working non-ZC response (`COPY body appended`, `body=1024 zc_queue=0`, followed by a normal `GOAWAY` from the client and clean teardown) against the failing one (`ZC body queued: len=1024 ... total_zc_rleft=1024`, then `data_read_cb: zc_queue=1024 body=0`, then `[XLIO-ZC] COPY DATA frame: ... body_rleft=0 zc_queue=1024`, then the stream and downstream are torn down with **no further frontend read/write, no GOAWAY, nothing** — the connection just goes silent) pinpoints a second, independent bug, this one in [shrpx_http2_upstream.cc](../nghttp2/src/shrpx_http2_upstream.cc):

- `http2_downstream_data_read_callback` (~line 1594, the `data_read_cb`) decides whether to report a chunk as available from the zero-copy queue purely via `!downstream->zc_body_empty()` — it has **no visibility into, and does not check, whether the client connection actually has a usable Ultra TX socket.**
- `send_data_callback` (~line 832) is the thing that actually transmits the frame, and it gates the zero-copy send path on `csock = ...get_connection()->xlio_sock` being non-null **and** `xa.has_ultra_tx()`. When `csock` is null (which is exactly what `XLIO_SKIP_TLS_ULTRA_HANDLE=1` now causes on the frontend, but could also happen for any other reason `xlio_sock` ends up unset on a connection that XLIO otherwise recognizes as ZC-eligible), it silently falls through to the **copy path** below.
- The copy path (`body->remove(*wb, length)`) drains `downstream->get_response_buf()` (`body`) — but the bytes for this frame were never put there; they were queued separately via `push_zc_body()` into `zc_body_queue_`, which the copy path never touches. Result: nghttp2 has already committed to a DATA frame header claiming `length` (1024 in the trace) bytes of payload (decided earlier and independently by `data_read_cb`), but the copy path appends **zero** actual payload bytes for it (`body->rleft()` was already 0) — a truncated/corrupted DATA frame. The client is left waiting for body bytes that were never sent.

This is a **decision/consumption mismatch bug, independent of the RX alert above** — `data_read_cb`'s "is ZC available" check and `send_data_callback`'s "can I actually send ZC" check use different conditions. It was previously latent/masked because `xlio_sock` was always non-null by the time either callback ran on a normal (un-worked-around) connection; disabling the Ultra handle acquisition to fix the RX bug is what exposes it.

**Instrumentation added (2026-09-07)**: an `ERROR`-level `tls-resume` log at exactly this mismatch point in `send_data_callback` (`shrpx_http2_upstream.cc`, right before falling into the copy path when `!csock || !xa.has_ultra_tx()` but `zc_body_queue_` is non-empty) — logs `stream_id`, the frame length nghttp2 committed to, `zc_queue`/`body_rleft` sizes, and `csock`/`has_ultra_tx` state, explicitly calling out that the frame will be truncated. Builds clean via `./scripts/compile.sh --nghttp2`.

_Next step — two independent fixes needed, not one_:
1. The original RX bug (`xlio_socket_from_fd()` on a live fd breaking subsequent `SSL_read()`) still needs a real fix in libxlio, not just the `XLIO_SKIP_TLS_ULTRA_HANDLE=1` workaround — the workaround is what surfaced bug #2, and permanently disabling the Ultra TX handle also gives up whatever TX benefit it was meant to provide.
2. This TX mismatch needs `data_read_cb` and `send_data_callback` to agree on ZC-eligibility using the *same* condition — either make `data_read_cb` also check `get_connection()->xlio_sock`/`xa.has_ultra_tx()` before reporting zero-copy-queued bytes as available (so it falls back to reporting from `body` instead, keeping frame-size promises consistent with what `send_data_callback` can actually deliver), or make `send_data_callback`'s copy-path fallback correctly drain `zc_body_queue_` (e.g. via a plain `SSL_write`/memcpy of the ZC-referenced bytes) instead of silently ignoring it. Reproduce with the new mismatch log at `PROBNIK_ERROR` (always visible) to confirm the fix once applied — it should stop firing.
_Avoid_: treating this TX corruption as evidence against the RX-side `xlio_socket_from_fd()` fix — it's a second bug that the workaround exposed, not a refutation of the first finding. Both are real, both need fixing, independently.
