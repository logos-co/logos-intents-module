# logos-intents-module

> [!NOTE]
> **Not in a release yet.** Basecamp runs it behind `--intents-module`, in
> development builds, until the 0.5 switch. Basecamp's presenter, and providers
> and requesters, need changes in `logos-basecamp`, `logos-module-builder` and
> `logos-view-module` that aren't released yet. Still to come: Basecamp's
> pending list (until then it opens every request), and apps ported onto it.

`intents_module` routes requests that need a person — approve a signature, pick a
wallet, open an app — from the module that asks to the module that provides,
and keeps the result for the requester alone. It knows modules and intents
only: the presenter decides how and when to reach the person.

The shell hosts it, like `capability_module`: Basecamp starts it in Basecamp, the
`logoscore` daemon for `logosctl` and CLI apps. Apps never list it as a
dependency; they declare the contract under `interface_dependencies`.

## Who calls what

| Caller | Methods | Checked by |
|---|---|---|
| Requester: an app's backend, or a core module | `request`, `result`, `cancel`; event `settled(handle)` | the core's caller identity; `uses` in its `metadata.json` |
| Provider | implements `deliver` ([`contracts/intent_provider.lidl`](contracts/intent_provider.lidl)), answers with `respond` | `respond` only from the module the dispatch went to |
| Presenter: the frontend that started the module | `claim` once, then everything else with the secret; event `work_available(counter)` | a `Host` caller, the first claim only, and the secret |

Requesters and providers declare [`contracts/intents.lidl`](contracts/intents.lidl)
under `interface_dependencies` and bind it to `intents_module`. Frontends use
[`contracts/intents_presenter.lidl`](contracts/intents_presenter.lidl). The
`contracts` check fails the build if the two stop matching the module.

A module is only ever itself. One named like the shell, the link requester or
an installed app is refused: the shell and links ask through the presenter, and
an app through its backend module.

### Asking

```cpp
auto intents = modules().bind_intents("intents_module");
// subscribe to settled() first
auto t = json::parse(intents.request("signing.approve",
                                     R"({"service":"signer_manager_module","handle":"smh_3f2a"})"));
if (!t["ok"]) { /* t["error"]: not_declared, or failed when over the rate */ }
// on settled(handle) for t["handle"], and once right away in case it already settled:
auto r = json::parse(intents.result(t["handle"], t["receipt"]));
// { ok, state: "pending" } or { ok, state: "settled", result: { ok, data, error } }
```

A request answers at once with a handle and a receipt, because a person can take
longer than a call's timeout. `settled` carries the handle only; the result is
pulled with the receipt and is readable only by the requester. Two refusals come
back in the reply itself, with no handle and no event: `not_declared` (your
manifest doesn't list the intent in `uses`) and `failed` (more than 20 requests
at once, or 5 a second sustained).

Errors in a result: `unavailable` (no provider, not allowed, or the provider went
away — deliberately indistinguishable), `not_declared` (your manifest stopped
listing the intent while the request was open: an update, or an uninstall),
`bad_request`, `cancelled`, `timeout`, `failed`.

A request for another module's intent waits as pending until the presenter
opens it (up to 30 minutes), whether or not anything provides it: nothing about
what is installed is decided before then. One for an intent your module
provides itself goes straight to it. Basecamp opens it at once when the person just clicked
in the requesting app's screen.

Limits per requester: params are plain data of at most 1 MiB (8 levels, 1000
values); 16 requests open and 4 in the pending list at a time; results are kept
for 10 minutes, or until `cancel(handle, receipt)`, within 4 MiB. Cancelling
withdraws a request; it doesn't stop a provider that already took it (see
Providing).

**Restarts.** If `intents_module` restarts, its requests are gone: `result()`
answers `unknown_handle`. Watch your subscription's status (`LpClient`'s
`onSubscriptionStatus` reports a new generation) and treat outstanding handles as
`failed` when it changes.

### Providing

List the intent in `provides` (with `params` for the payload you expect, and
`result` for what your answer carries), implement
`deliver(dispatch, intent, params_json, requester)`, accept it only
when `logos::currentCaller().isModule("intents_module")`, and return
`{"ok":true}` at once — within 5 seconds, or the provider counts as unreachable.
Then answer with `respond(dispatch, ok, data_json, error)`, data of at most
1 MiB. If a person has to see something, show it through your own view.

`result` takes the syntax of `params`:

```json
{ "intent": "signing.approve",
  "params": [ { "name": "handle", "type": "string", "required": true } ],
  "result": [ { "name": "signature", "type": "string", "required": true } ] }
```

An answer with `ok` true must be an object with every required field, of the
declared type; fields it doesn't describe pass. One that doesn't fit reaches
the requester as `failed`. Refusals aren't checked, and an entry without
`result` isn't either.

**Cancelling.** A requester's `cancel` withdraws the request and drops its
result. The provider isn't told, and a later `respond` from it is dropped.
Nothing it already started (a transfer, a signature) is stopped or undone, so
a provider that acts on a delivery should have the person confirm in its own
view first. There is no cancellation protocol for providers yet.

An app asks and answers through its backend module, which declares the app's
`uses` and `provides` in its own `metadata.json`. With logos-module-builder, an
app with a `"backend": "module"` backend gets them copied there (shipped in the
app's package as `<app>_backend/metadata.json`), and its backend gets
`deliver`: implement
`QString deliver(const QString& dispatch, const QString& intent, const QString& paramsJson, const QString& requester)`
in the backend class; the generated glue already refuses anyone but
`intents_module`.

A module that asks or provides ships its `metadata.json` (the builder adds it
when `provides` or `uses` is set): `set_installed` reads declarations from
there.

### Presenting

The host claims the role right after loading the module, before any app loads
and before the daemon accepts `logosctl` calls, then declares itself and the
installed packages:

```
claim() -> { ok, secret }
declare_shell(secret, {...}); set_installed(secret, [...])
```

Until a presenter claims, nobody can be asked: every request is `unavailable`.

The presenter decides when to interrupt the person. A request for another
module's intent comes as a `pending` item and waits until the presenter opens
it: a terminal presenter opens everything; Basecamp opens a request when the
person just clicked in that module's screen, once per click, and until its
Activity view shows the pending list it opens everything, as a development
default. What a host must do is in [`docs/security.md`](docs/security.md#host-requirements).

The presenter pulls its work with `next_work(secret)` when `work_available`
fires, and answers items with `answer(secret, id, {...})`:

| Work item | Answer |
|---|---|
| `choose` {dispatch, intent, requester, providers} | `{"action":"choose","provider":m}` or `{"action":"cancel"}` |
| `pending` {dispatch, intent, requester} | `{"action":"open"}` or `{"action":"dismiss"}` |
| `deliver` {dispatch, intent, params, requester} — the shell is the provider | `{"action":"respond","ok":b,"data":d,"error":e}` |
| `present` {provider}, `offer_install` {intent}, `completed` {...}, `result` {handle, result}, `dismiss` {work} | none |

Before answering `choose`, the presenter makes sure the chosen module runs:
Basecamp starts the app or module the person picked. A provider that isn't
running fails as `unavailable`. `present` and `completed` name the module
delivered to; the presenter raises it, and later returns the person, only if
that module has a screen.

An item that ends before the presenter pulled it is withdrawn from the queue;
`dismiss` is only for items the presenter already has. A pick of a provider that
changed while the chooser was open is taken, and a new `choose` follows.
Install offers come only for opened requests, once per intent a minute; the
presenter finds what could be installed in its own catalogue.

Package and module names in work items are plain (`[A-Za-z0-9_.-]`, the
registry refuses anything else), and requester and display names are stripped of
control, bidirectional and invisible characters. Params in a `deliver` item are
the requester's and are passed as they are.

## Design and security

Why it is built this way, what is done and what is left before the 0.5 switch:
[`docs/design.md`](docs/design.md). The security rules and who enforces each:
[`docs/security.md`](docs/security.md).

## Build and test

```sh
nix build                                     # result/lib/intents_module_plugin.so
nix build .#checks.aarch64-linux.unit-tests   # or your system
```
