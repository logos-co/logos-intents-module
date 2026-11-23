# logos-intents-module

> [!WARNING]
> **Work in progress — not finished, not released.** The API and the contracts
> can still change. Basecamp runs it only behind `--intents-module`, in
> development builds. Still missing: a pending-list UI in Basecamp (a request
> with no click behind it waits there unseen), follow-up requests from a
> provider after a hand-off, `basecamp://` links through the module, and apps
> ported onto it. Providers and requesters also need unreleased changes in
> `logos-module-builder`, `logos-view-module` and `logos-view-module-runtime`.
> Don't depend on it yet.

`intents_module` routes requests that need a person — approve a signature, pick a
wallet, open an app — from the module that asks to the module that provides,
and keeps the result for the requester alone. It is the broker that used to live
inside Basecamp (`IntentBroker`, `IntentRegistry`), as a core module.

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
an app only through the backend the presenter registered for it.

### Asking

```cpp
auto intents = modules().bind_intents("intents_module");
// subscribe to settled() first
auto t = json::parse(intents.request("signing.approve",
                                     R"({"service":"signer_manager_module","handle":"smh_3f2a"})",
                                     clickTokenFromMyView));
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
away — deliberately indistinguishable), `bad_request`, `cancelled`, `timeout`,
`failed`.

In Basecamp a request with no click token behind it waits in the pending list
until the person opens it (up to 30 minutes), whether or not anything provides
it: nothing about what is installed is decided before then.

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

An app's provider is its backend: the presenter tells `intents_module` which
backend it registered for which app, and only that backend may respond. With
logos-module-builder, an app whose metadata lists `provides` and has a
`"backend": "module"` backend gets `deliver` on that backend: implement
`QString deliver(const QString& dispatch, const QString& intent, const QString& paramsJson, const QString& requester)`
in the backend class; the generated glue already refuses anyone but
`intents_module`.

A core module that asks or provides ships its `metadata.json` (the builder adds
it when `provides` or `uses` is set): `set_installed` reads declarations from
there.

### Presenting

The host claims the role right after loading the module, before any app loads
and before the daemon accepts `logosctl` calls, then declares itself and the
installed packages:

```
claim({"attention": "interactive" | "immediate" | "headless"}) -> { ok, secret }
declare_shell(secret, {...}); set_installed(secret, [...]); backend_registered(secret, app, module, instance) ...
```

`instance` names one run of the backend. Registering the same app, module and
instance again changes nothing; another instance, or an empty one, is a new
backend, and whatever was dispatched to the one before fails. The host
unregisters a backend when that run stops.

- `interactive` (Basecamp): a request pops up only with a click token behind it;
  otherwise it waits in the pending list.
- `immediate` (a terminal presenter): everything is shown at once.
- `headless` (a daemon with no one to ask): only providers named with
  `configure_provider` are used, without asking.

The presenter pulls its work with `next_work(secret)` when `work_available`
fires, and answers items with `answer(secret, id, {...})`:

| Work item | Answer |
|---|---|
| `choose` {dispatch, intent, requester, providers} | `{"action":"choose","provider":m}` or `{"action":"cancel"}` |
| `activate` {app} | `{"action":"activated"}` (then `backend_registered`) or `{"action":"failed"}` |
| `pending` {dispatch, intent, requester} | `{"action":"open"}` (that is the click) or `{"action":"dismiss"}` |
| `deliver` {dispatch, intent, params, requester} — the shell is the provider | `{"action":"respond","ok":b,"data":d,"error":e}` |
| `present` {app}, `offer_install` {intent, candidates}, `completed` {...}, `result` {handle, result}, `dismiss` {work} | none |

An item that ends before the presenter pulled it is withdrawn from the queue;
`dismiss` is only for items the presenter already has. A pick of a provider that
changed while the chooser was open is taken, and a new `choose` follows. Install offers come only
for clicked or opened requests, once per intent a minute.

Package and module names in work items are plain (`[A-Za-z0-9_.-]`, the
registry refuses anything else), and requester and display names are stripped of
control, bidirectional and invisible characters. Params in a `deliver` item are
the requester's and are passed as they are.

## Security

See [`docs/security.md`](docs/security.md).

## Build and test

```sh
nix build                                     # result/lib/intents_module_plugin.so
nix build .#checks.aarch64-linux.unit-tests   # or your system
```
