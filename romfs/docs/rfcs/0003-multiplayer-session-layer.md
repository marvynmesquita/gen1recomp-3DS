# RFC 0003 — Add a reusable multiplayer session layer

## Status

Proposed. Engine: `Session.lua`, `Net.lua`, `LinkState.lua`,
`Tournament.lua`. Tests: `link_session.lua`.

## Motivation

Link play and tournaments currently own transport lifecycle details and
temporarily remove and reinsert packets in `Net.inbox` when a handshake or
battle starts. That makes packet ownership fragile and gives a future
shared-world mode no stable host/guest-aware boundary to reuse.

The engine needs one small layer that preserves today's wire protocol while
owning received-packet order and terminal cleanup. Pokémon, battle, tournament,
save, and overworld rules remain outside that layer.

## The decision it extends

Extends the existing split between `Net` (backend setup, framing, and relay
controls), `Handshake`/`Protocol` (mode payloads), and the states that
interpret those payloads. It does not replace any of those components.

## The exact API delta

Backward-compatible and internal-only.

### `Session.new(transport, options)`

Wraps one successfully configured Net-compatible transport. `options.role`
is exactly `"host"` or `"guest"`; `options.kind` is a non-empty
local label such as `"link"` or `"tournament"`. Role and kind are
immutable session metadata selected locally and are never inferred from peer
packets.

The facade forwards the narrow fields current consumers need:
`paired`, `code`, `address`, `target`,
`error`, and `closed`.
It forwards valid outbound tables unchanged through `send(message)`.

### Receive and lifecycle methods

- `update()` pumps the transport, validates decoded inbound values, and
  appends accepted messages to a private FIFO.
- `pollOne()` removes the oldest queued message.
- `poll()` removes every queued message in order.
- `take(type)` removes the first queued message with that type without
  disturbing any other message.
- `hasPending()` reports whether the FIFO is non-empty.
- `getRole()`, `getKind()`, `getStatus()`, and
  `getFailure()` expose local metadata and lifecycle.
- `close()` closes the underlying transport once and is safe to repeat.

Statuses are `connecting`, `paired`, `draining`, `closed`,
and `failed`. A transport close or failure becomes `draining` while
accepted packets remain queued. The terminal `closed`/`error`
compatibility projection appears only after that FIFO drains, so a last packet
travelling with a disconnect remains observable.

An inbound value is structurally valid only when it is a table with a string
`type`. Invalid decoded values end the session with a protocol failure.
Unknown but structurally valid types remain queued for the owning mode; the
session does not contain a packet allowlist.

## Authority direction

A later `WorldSession` may compose this facade. In that mode the host
will own the world snapshot, map state, NPC state, event results, and shared
progression. A guest will bring a trainer identity plus their Pokémon party,
inventory, and other explicitly selected profile snapshot.

Guest profile data and commands will be untrusted input. The host must validate
them and must authorize every world mutation before rebroadcasting the result.
The concrete snapshot schema, command vocabulary, conflict rules, and
persistence policy require a separate RFC and are not introduced here.

## Compatibility and security

No packet envelope, message name, payload shape, framing rule, relay protocol,
save schema, or engine protocol version changes. Existing valid outbound
messages encode exactly as before, and existing link and tournament screens
keep their current player-facing behavior.

The layer does not authenticate players or encrypt traffic. Existing LAN and
relay access assumptions remain unchanged; knowledge of a join address or code
still grants the same access it grants today. Authentication, reconnect
identity, rate limits, and abuse controls remain future protocol decisions.

## Migration note for players, mods, and peers

**Nothing.** `LinkState` and `Tournament` adopt the facade
internally. Existing peers receive the same messages, mods gain no new API, and
players do not migrate saves or settings.

## Parity tests

- **ROM-free facade:** constructor validation, immutable role/kind, unchanged
  send shape, FIFO ordering, typed retrieval, unknown typed packets, draining,
  terminal failure latching, protected transport calls, and decoded-value
  rejection.
- **Existing modes:** source guards prohibit direct inbox mutation; headless
  module loads and the complete engine tier cover both migrated states.
- **ROM-backed link play:** run the existing link driver when generated ROM
  data is available; the normal quick suite remains the required baseline.

## Deprecation etiquette and non-goals

Nothing deprecated. This RFC adds an internal facade and removes no transport
method.

It does not add shared-world packets, co-op screens, a remote actor, save
transfer, server persistence, matchmaking, reconnect, or a protocol-version
bump. Those changes require the world-specific layer and its own review.
