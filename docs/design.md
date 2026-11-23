# Generic intents design

**Status:** design record · 2026-10-30 · Khushboo · decisions added 2026-11-04 · brought up to date with the code 2026-11-23. What is built and what is left: [Steps](#steps). The README and the files in `contracts/` are the reference; where this document and they differ, they win.
**Scope:** intents only. Activity (the feed, notifications, search) is a separate discussion. One link is decided: with an `interactive` presenter, a request with no click behind it goes into a **pending list** and is shown **alongside activity**, never popped up. Until Basecamp's Activity view shows that list, Basecamp claims `immediate` and pops every request up, as its QML path does today.

## Decisions (2026-11-04)

1. **Only modules ask and provide.** An app's backend (`<app>_backend`) or a core module calls `intents_module`; QML views don't. A view's one intent-related call is `logos.clickToken()`, whose result it passes to its own backend. QML-only apps go away in 0.5, so no app is left without a way in.
2. **One presenter per core, fixed by the host at startup.** The frontend that starts `intents_module` is its presenter for that core's lifetime. There is no handover, so a second claim is always refused; a Signer CLI can't attach to Basecamp's core as presenter.
3. **The presenter has its own contract,** `intents_presenter.lidl`, used only by frontends. Apps never get bindings for it.
4. **App ↔ backend comes from the presenter, not from the name.** Basecamp tells `intents_module` which backend it registered for which app; a click token is accepted only from that backend.
5. **Auto-return stays in the frontend.** `intents_module` reports a completed dispatch with its `handoff` flag; Basecamp applies its guards (frontmost, dialog open, dwell, circuit breaker) and decides whether to navigate.
6. **Rollout:** the QML intent path stays on, unchanged, through every 0.4 release. `intents_module` and the backend path are built during 0.4 but not switched on in a released Basecamp. In 0.5 both flip together: QML intents off, `intents_module` on. No release runs two brokers.

## The idea

- **Intents are for user actions.** A person has to decide, confirm or choose, or the requester mustn't know or depend on who provides the action. Everything else is a plain cross-module call.
- **The broker becomes a module.** Today the intent broker lives inside Basecamp and is reached only from QML. It moves into its own core module, `intents_module`, **hosted by the shell**: Basecamp starts it in Basecamp, the `logoscore` daemon starts it for `logosctl` and CLI apps. Apps never load or list it; like `capability_module`, it's a platform service the host always runs.
- **Only modules talk to it.** Requesters and providers are modules, so the core attests every caller. Views reach intents through their own backend.
- **One module, the same behaviour everywhere.** Each frontend only plugs in its way of reaching the person.
- **This is the interim piece.** If the new core brings its own broker between modules, `intents_module` gives way to it, and the contract stays the same.

## Intent or call?

| Use an **intent** when… | Use a **direct call** when… |
|---|---|
| a person must **decide, confirm or choose** (approve a signature, pick a wallet, consent to an install) | no person is involved |
| the requester **mustn't know or depend on** the provider ("send with *a* wallet") | the target module is known and fixed |
| it's about **attention**: open or raise an app, show a request | it's work: fetch, compute, store, sign once approved |
| the answer mustn't reveal what is installed | the callee enforces access itself with `currentCaller()` |

**Signing has both.** The wallet *calls* `signer_manager_module.request_approval` (work). The only intent is "get a person to look at this request" (`signing.approve`). The manager then *calls* the signer.

## Today → after

| | Today | After |
|---|---|---|
| Broker | inside Basecamp | `intents_module`, hosted by the shell (Basecamp, `logoscore`) |
| Who can ask | QML views | app backends, core modules |
| Who can provide | `ui_qml` views | modules: an app's backend, a CLI app's module |
| What a view does | `logos.request`, `onIntentRequested`, `logos.respond` | `logos.clickToken()` only, passed to its backend |
| Who is asking | Basecamp maps the view's bridge to a name | the core's caller identity (`module <app>_backend`, verified 2026-10-29), for every requester and provider |
| Outside Basecamp | nothing services intents | the same module, with the CLI app or daemon as presenter |

## `intents_module`

| API | Called by | Does |
|---|---|---|
| **Requester** `request(intent, params, click_token)` → handle + receipt | any module | reads the caller from the core, checks `uses`, resolves a provider, routes the result to the requester only |
| **Provider** `deliver(…)` / `respond(…)` | provider modules | receives a request and answers it |
| **Presenter** (the human hook) | the host's frontend only, fixed at startup | **pulls** its work and answers it: choosers, raising a provider, the pending list alongside activity; registers click tokens and app backends |

**Kept in the module:**
- the registry of who provides what, read from installed manifests;
- the name and payload checks, and the error codes (`src/intent_rules.h`);
- the `unavailable` timing floor;
- rate limits;
- result routing;
- the **pending list**: requests with no click, waiting for the person to open them;
- which backend belongs to which app, as the presenter registered it.

**Kept in each frontend:**
- drawing;
- seeing real input in each view and issuing click tokens for it;
- navigation, including auto-return after a dispatch completes.

**The presenter pulls.** `intents_module` announces new work with an event that carries only a counter; the presenter fetches the work with `next_work(secret)` and answers each item with `answer(secret, id, {...})`. Nothing has to call into the frontend's process, and every frontend (Basecamp, the Signer CLI, a standalone app) connects the same way.

| Work item (module → presenter) | Answer (presenter → module) |
|---|---|
| `choose`: show a chooser (dispatch, intent, requester, providers) | `{"action":"choose","provider":m}` or `{"action":"cancel"}` |
| `activate`: load and raise app X | `{"action":"activated"}` (then `backend_registered`) or `{"action":"failed"}` |
| `pending`: a request joined the pending list | `{"action":"open"}` when the person opens it (that is the click), or `{"action":"dismiss"}` |
| `deliver`: the shell itself provides the intent | `{"action":"respond","ok":b,"data":d,"error":e}` |
| `offer_install`: a provider could be installed | none: fire and forget, as today |
| `present`: raise the provider's app; `completed`: a dispatch completed, with `handoff`; `result`: the answer to the shell's own request; `dismiss`: take down an item already pulled | none: the frontend decides whether to return the person |

Plus calls only the presenter makes: `declare_shell`, `set_installed` and `set_installable` (what is installed and what the catalogue offers), `register_click_token`, `backend_registered(app, module, instance)`, `backend_unregistered(app)`, `configure_provider` (headless), and `shell_request` for Basecamp's own requests and `basecamp://` links. A chooser work item lists installed providers, so the whole contract sits behind the presenter secret.

**How the presenter gets its secret** (decided 2026-11-04). The host loads `intents_module` straight after `capability_module`, before any app, and at once calls `claim()`. `intents_module` accepts the claim only from a `Host` caller (`currentCaller().isHost()`), only once, and answers with a fresh secret. Every presenter method after that requires the secret.
- **Why not hand the secret over at start.** The only private parent → child channel is the container's stdin pipe, which carries the module's auth token once and is then closed. Command-line arguments are readable by every process of the same user. Adding a second secret to the pipe would change `logos-container-subprocess`, `logos_host` and liblogos's loader API: a core change, for a protocol the new core replaces.
- **Why a `Host` check plus "once".** In Basecamp, only Basecamp itself calls as `Host`, and modules call as `Module`. Under the daemon, every `logosctl call` is also `Host`, so "once, before the daemon accepts `logosctl`" is what keeps a `logosctl` user out.
- **Consequence: the presenter is the process that hosts the core.** Basecamp, a CLI app that embeds its own core, or the daemon itself, which then simply never pulls work (flow D). A Signer CLI can't become the presenter of a daemon it reaches through `logosctl`.
- **Restart.** If `intents_module` restarts, every in-flight request fails, and the host claims again as part of reloading it. Under the daemon a `logosctl` user could race that re-claim; that user already controls the daemon.
- **The secret travels in an RPC reply,** so the minimum daemon version (gap 2) is a hard requirement for it, not only for results.

**Unloading a provider.** When Basecamp unregisters a backend, `intents_module` fails every dispatch still in flight to it, so a reloaded app can't answer a request its previous instance was given. A registration names the run (`instance`): the same run registered again changes nothing, another run is a new backend. This replaces the broker's pointer-identity guard, which a separate process can't use.

Most of the code exists already. Basecamp's `IntentBroker` / `IntentRegistry` reach the world through pluggable seams (endpoint, presenter, chooser, installer), which map onto the three APIs. They include only QtCore (checked 2026-11-04: no widgets, no QML), so the port is mostly moving code. The synchronous presenter queries (`isAppFrontmost`, `anyDialogOpen`) don't move: they serve auto-return, which stays in the frontend.

## How apps declare and call intents

Apps don't list `intents_module` as a **package** dependency. A module lists the intents **contract** as an `interface_dependencies` entry: generated bindings only, nothing installed or loaded. `signer_manager_module` already binds its signers this way, through `signer.lidl`.

| Step | How | Exists? |
|---|---|---|
| **Declare** | `uses` and `provides` in `metadata.json`, carried into the signed manifest. Modules add `intents.lidl` (and providers `intent_provider.lidl`) under `interface_dependencies`. | `uses`/`provides` yes · contracts new |
| **Discover** | `intents_module` builds its registry from the installed packages' `uses` / `provides`. Apps don't register at runtime, so a package can't claim more than its manifest. | Basecamp's registry does this today |
| **Request** | `modules().bind_intents("intents_module")`, then `request` → handle + receipt, wait for `settled(handle)`, then `result(handle, receipt)`. A view that wants to ask calls its backend, passing `logos.clickToken()`. | new |
| **Provide** | `intents_module` calls the provider's `deliver(dispatch, intent, params, requester)`; the provider answers with `intents.respond(dispatch, …)` and accepts `deliver` only from `intents_module`. If the person has to see something, the backend tells its own view through the `.rep`. | new |

The contracts live in this repo's `contracts/`; the presenter's is sketched here, see `contracts/intents_presenter.lidl` for all of it.

```
module intents {                       // contracts/intents.lidl
  version "1.0.0"
  method request(intent: tstr, params_json: tstr, click_token: tstr) -> tstr   // { ok, handle, receipt } at once; token may be empty
  method result(handle: tstr, receipt: tstr) -> tstr           // { ok, state } or { ok, state: "settled", result: { ok, data, error } }
  method cancel(handle: tstr, receipt: tstr) -> tstr
  method respond(dispatch: tstr, ok: bool, data_json: tstr, error: tstr) -> tstr   // providers only
  event settled(handle: tstr)                                   // handles only, no content
}

module intent_provider {               // contracts/intent_provider.lidl, implemented by provider modules
  version "1.0.0"
  method deliver(dispatch: tstr, intent: tstr, params_json: tstr, requester: tstr) -> tstr
}

module intents_presenter {             // contracts/intents_presenter.lidl, frontends only; every method but claim takes the secret
  version "1.0.0"
  method claim(options_json: tstr) -> tstr                      // once, by the host: { ok, secret }
  method next_work(secret: tstr) -> tstr                        // pending work items, see the table above
  method answer(secret: tstr, work_id: tstr, answer_json: tstr) -> tstr
  method register_click_token(secret: tstr, token: tstr, app: tstr, intent: tstr) -> tstr
  method backend_registered(secret: tstr, app: tstr, module: tstr, instance: tstr) -> tstr
  method backend_unregistered(secret: tstr, app: tstr) -> tstr
  method shell_request(secret: tstr, intent: tstr, params_json: tstr, as_link: bool) -> tstr   // the shell's own requests, basecamp:// links
  // also declare_shell, set_installed, set_installable, configure_provider
  event work_available(counter: uint)                           // a counter only, no content
}
```

**Results are pulled with a receipt,** the pattern `signer_manager_module` already uses:
- a result can take minutes, longer than the 20-second call timeout;
- the `settled` event carries only a handle, so broadcasting it leaks nothing;
- only the receipt holder can read the result;
- requesters don't need to expose a method for the broker to call.

Why not a package dependency:
- Every app would have to list a platform service.
- Per-app version ranges would let apps pin different broker versions.
- A dependency is something a package resolves and installs, which conflicts with "reserved name, shipped with the host, never from a catalog".

## Click tokens

**Why.** A request the user just clicked for may open a dialog or raise an app at once. Any other request must not pop up over what the person is doing; it goes into the pending list, shown alongside activity. So something has to know, unforgeably, whether the person really just interacted with the requesting app.

**Where the knowledge comes from.** An app's QML view is a `QQuickWidget` inside Basecamp's own process, so every click and key press reaches Basecamp's window before the view. Basecamp watches input per view with an event filter and knows "the user pressed in Wallet's view at 10:00:01.200". Browsers use the same idea ("user activation") to allow popups only after a real click.

**Why a token.** The check happens in another process (`intents_module`), and the request comes from the app's backend, not its view. A click token carries the fact "this app's view was just clicked" there, in a form that can't be forged or reused.

| Step | What happens |
|---|---|
| **Issue** | On real input in an app's view, Basecamp can issue a token: a random, meaningless string. |
| **Register** | Basecamp registers it with `intents_module`, using its presenter secret: token → app, expiry, (intent). |
| **Hand out** | The view asks `logos.clickToken(intent?)`. With no recent input in that view there is no token. |
| **Carry** | The view passes it to its backend, which adds it to `intents.request(…, click_token)`. |
| **Check** | `intents_module` compares the request's token with the registered ones: valid for this requester? Then it marks it used. |

**Checks:**
- **same app:** issued for Wallet's view, and the caller is the backend Basecamp registered for Wallet (`backend_registered`). The name alone proves nothing: Basecamp refuses to register `<app>_backend` when an installed module already has that name, and `intents_module` must not accept tokens from that module either. A standalone core module that serves an app (`wallet_backend_module` for `evm_wallet_ui`) is not its backend. A token from another app is refused;
- **single use, short-lived:** about two seconds after the input, so a backend can't save tokens up;
- **one at a time per view,** only on real input events, so a timer in the view can't produce them;
- **optionally bound to one intent name**, when the view asked for it with that name.

A missing, expired, reused or foreign token never fails the request. It is simply treated as **no click**, and the request goes into the pending list.

**Other frontends.** A CLI app has no views and gives no tokens. It is already in front of the person, so it prompts when a request is delivered.

## Flows

**A. Basecamp, the user clicks**
```
Wallet view click ─► token = logos.clickToken() ─► backend.send(tx, token)
wallet_ui_backend ─intents.request(intent, params, token)─► intents_module
intents_module: token registered for Wallet, caller is Wallet's registered backend, fresh → valid, used up
intents_module ─new work─► Basecamp presenter pulls it (draws chooser, raises Signer window)
intents_module ─deliver─► Signer app backend ─► its view shows the request ─► user confirms ─► respond
intents_module ─settled─► wallet_ui_backend pulls the result
```

**B. Basecamp, a module asks with no click**
```
wallet module ─request─► intents_module: no click → added to the pending list
Basecamp presenter shows the pending list alongside activity (e.g. "Wallet wants you to sign"), without interrupting
user opens it (now there is a click) ─► same as A from "choose / raise"
```

**C. Signer CLI app, its own core, no Basecamp**
```
the host (the CLI app's core or the daemon) runs intents_module; the CLI app is its presenter (terminal); its manifest provides signing.approve
wallet module ─request─► intents_module ─deliver─► CLI app ─► prompt, QR codes, approve ─► respond
intents_module ─result─► wallet module
```

**D. Headless daemon, no presenter**
```
request ─► intents_module: one configured provider → deliver
                           a human choice needed → unavailable
```

The requester's code is the same in A–D, and the provider contract is the same for the Basecamp Signer app and the Signer CLI app.

## API: the same everywhere

| Call | Module (backend, core module, CLI app) | QML view |
|---|---|---|
| Ask | `intents.request(intent, params, token)` → handle + receipt; `settled(handle)` → `intents.result(handle, receipt)` | calls its own backend |
| Provide | `deliver(dispatch, …)` → `intents.respond(dispatch, …)` | shows what its backend tells it |
| Click token | received from its view as an argument | `logos.clickToken(intent?)`, passed to its backend |

**The same rules for every caller:**
- the name grammar;
- plain-data payloads (1000-node cap);
- the error codes `not_declared`, `unavailable`, `bad_request`, `cancelled`, `timeout`, `failed`.

**Long waits:** a request can wait minutes for the person, longer than a call's 20-second timeout. So `request` answers at once with a handle and a receipt, and the requester pulls the result once `settled(handle)` fires.

## Making intents generic (no core changes)

**1. The contract doesn't assume QML or Basecamp.**
- Each `provides` entry declares its `params` and its **result** shape, in the same syntax. A successful answer that doesn't fit its `result` reaches the requester as `failed`.
- The requester is the identity the platform gives the caller. Providers stop checking for `main_ui`.
- "The user clicked" is an input on the request (a single-use token), not an assumption.
- Raising the provider's window (`handoff`) is a frontend hint that a CLI ignores.

**2. Names say what's wanted, not who does it.**

| Namespace | For | Example |
|---|---|---|
| domain | what is wanted; any provider fits | `signing.approve {service, handle}`, `wallets.unlock` |
| `basecamp.*` | what only Basecamp does | `basecamp.settings.open` |
| `logos.*` | the platform (reserved) | — |

The namespaces are enforced: `logos.*` and the shell's prefix are refused from packages. Renaming today's intents is **dropped** (2026-11-23): names are free strings, and a provider that wants a generic name declares it next to the old one. `evm.signing.approve` → `signing.approve` is revisited with the Signer CLI.

## Security

The broker used to be inside Basecamp's process. As a module, it is reachable and replaceable like any other, so several protections that came free must now be built in. The numbers below are the ones `docs/security.md` cites; that file says how each is handled in the code now, and what is left to the host.

**New gaps from the move**

| # | Gap | Mitigation |
|---|---|---|
| 1 | Under `logoscore`, a `logosctl call` arrives as `Host` (developer guide). A "Host is the presenter" rule would let any `logosctl` user claim the presenter role, or submit "clicked" requests, and skip the person. | The frontend that starts `intents_module` claims the presenter role **once, at startup, before any app loads and before the daemon accepts `logosctl`**, as a `Host` caller, and gets a one-time secret. A second claim is always refused; there is no handover. |
| 2 | Daemons older than the completion-channel reservation publish every method **reply** as a `__logos_call_complete__` event, so every result would be broadcast. | Require a minimum daemon version and refuse to start below it. Never put request or result content in an event. |
| 3 | `logoscore` can give a module a `tcp` / `tcp_ssl` listener, which makes the broker reachable from the LAN. | Local transport only. Refuse remote callers until cross-core identity and pairing exist. |
| 4 | Proof of a click now crosses IPC and could be replayed or borrowed. | Click tokens: issued by Basecamp only on real input, bound to one app (optionally one intent), accepted only from that app's registered backend, single use, about two seconds. A bad token counts as no click; it never fails the request. |
| 5 | Strings shown in a terminal can carry escape sequences that fake the screen. | Strip control characters; plain text only, as `evm_signer_cli` already does for names. |
| 6 | Any module can call a module that has no access rule. | Check the caller on every method. `respond` is accepted only from the provider the dispatch went to (dispatch id **and** caller). There's no "list providers" method; choosers reach only the presenter. |
| 7 | The broker can be replaced by a module with the same name. | Reserved name, bundled with each frontend, hash-checked, never installable from a catalog. |
| 8 | Sensitive payloads pass through one more process. | Memory only, size caps, never logged. |
| 9 | A module named `<app>_backend` could pose as an app's backend to spend its click tokens. | Tokens are checked against the backend the presenter registered for the app, never against the name. |

**Existing risks that now also reach the CLI**

| # | Issue | Handling |
|---|---|---|
| 10 | Provider identity is a name from unsigned `metadata.json`. | A CLI prompt shows the package name and whether it is signed, as Basecamp's chooser does. A headless daemon's config names the provider explicitly. |
| 11 | A config-chosen provider can be silently rerouted by editing the config. | Only the host may set it, and every change is logged. |
| 12 | Two presenters on one core. | Impossible: one presenter per core, fixed at startup, no handover. |
| 13 | `basecamp://` links, catalog install offers, grants by role. | They stay in Basecamp's presenter. Only the presenter may submit as the link requester. |
| 14 | The module crashes mid-request. | Fail closed: in-flight requests answer `failed`, and frontends clear stale dialogs. |
| 15 | The timing floor is noisier over IPC. | Keep it, and make the answers for absent, denied and malformed identical. |

**Not worse than today:**
- Consent is still drawn by the frontend and the provider, never the requester.
- Requester identity improves: the core attests every caller, instead of a bridge mapping.

**Refusal tests for the module's first PR** (all in the unit tests, except the remote caller: callers are identified by their token on every transport, so it isn't refused; see "Remote transport" in `docs/security.md`):
- a `logosctl` caller claiming the presenter role;
- a second presenter claim;
- a claim from a `Module` caller;
- any presenter method called without the secret;
- a remote caller;
- a reused click token, an expired one, one issued for another app, and one presented by a module named `<app>_backend` that the presenter never registered;
- `respond` from the wrong module, and from a backend unregistered after delivery;
- a display name containing escape sequences.

## Steps

| # | Step | Needs | Status (2026-11-23) |
|---|---|---|---|
| 1 | Write the generic contract: result schemas, attested requester, click token, `handoff` as a hint; split the namespaces | — | **Done** (`contracts/`, merged) |
| 2 | Generic names for current intents, with aliases (`signing.approve` first) | 1 | **Dropped**: see "Names say what's wanted" |
| 3 | `intents_module`: Basecamp's broker and registry ported behind requester / provider / presenter APIs (the presenter pulls), with the security rules above | 1 | **Done**, work in progress: this repo, behind `--intents-module` |
| 4 | Basecamp: becomes the presenter at startup, pulls its work, registers app backends; auto-return driven by completion notices | 3 | **Built, not merged** (`logos-basecamp`); claims `immediate` until the Activity view shows the pending list |
| 4b | Basecamp: per-view input tracking and click tokens (`logos.clickToken`), registered with `intents_module` | 4 | **Built, not merged** (`logos-view-module-runtime`, `logos-basecamp`) |
| 5 | Providers as modules: delivery to a provider's backend | 3 | **Built, not merged** (`logos-module-builder`, `logos-view-module`) |
| 6 | CLI presenter and provider (the Signer CLI app) | 3, 5 | Not started |
| 7 | Move every app's intent calls from its view to its backend (requesters and providers); update the tutorials and guides | 3, 5, and each app's `"backend": "module"` port | Started: Package Manager UI, not merged; no other app |
| 8 | 0.5 switch: QML intent path off in `logos-view-module-runtime` and Basecamp, `intents_module` on | 4, 4b, 7 | 0.5 |

Steps 1–7 are built during 0.4; 0.4 releases keep today's QML path on. Step 8 ships in 0.5, with the removal of QML-only apps.

**Still missing before the switch:**
- the pending list next to activity, so Basecamp can claim `interactive`;
- a follow-up request from a provider after a hand-off: Package Manager's `confirm_install` after `packages.install` has no click behind it;
- IPC tests that drive the real contracts between modules, not only the unit tests;
- an open finding from review: a `result` or `dismiss` work item can be evicted when the presenter stops pulling, so the result of the shell's own request can be lost. Keeping results by handle for the presenter to fetch changes the presenter contract.

## Open questions

1. Does the new core broker intents between modules? If it does, steps 3–6 shrink to adopting it.
2. How the pending list looks next to activity (one list or a section, badges, expiry of old requests): decided in the activity discussion.
3. Should `evm_signer_cli` grow into the Signer CLI app, or be replaced by it?
4. ~~How the presenter gets its secret~~ **Decided 2026-11-04: a claim, once, before any app loads.** See "How the presenter gets its secret" above.
5. **Enforced access policy.** With `--access-policy enforce` (opt-in in both Basecamp and the daemon, off by default), a module may call only the modules it declares as dependencies. Apps don't declare `intents_module` as a dependency, and `intents_module` is no provider's dependent. So under enforcement every `request` and every `deliver` is refused. It needs either an explicit policy entry written by the frontend, or `intents_module` in the core's trusted/exempt lists (a core change).
6. ~~Click-token lifetime~~ **Decided: two seconds from the presenter registering the token to its use**, the whole view → backend → `intents_module` route.
7. ~~The full rename list for step 2~~ **Dropped 2026-11-23** with step 2.
8. ~~Result schemas~~ **Decided: the same syntax as `params`, checked on `respond`.**
9. The minimum daemon version (the release that reserved `__logos_call_complete__`), and what "hash-checked" compares against.
10. **Shared intent contracts.** Each provider declares its own `params` and `result`, so two providers of one intent can disagree, and a requester that doesn't know which one answers can't rely on either. Should an intent have one versioned contract, with providers free to add optional fields?
