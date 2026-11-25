# Generic intents design

**Status:** design record, kept to the code. What is built and what is left: [Steps](#steps). The wire API is in `contracts/`; the security model, with what each rule depends on, is in [`security.md`](security.md).
**Scope:** intents only. Activity (the feed, notifications, search) is a separate discussion. One link is decided: a request the person didn't just click for goes into a **pending list**, shown **alongside activity**, never popped up. Until Basecamp's Activity view shows that list, it opens at once only what a package the person trusts asks, and any other request on a click in its screen: an accepted limit ([`security.md`](security.md#known-limits)).

## Decisions

1. **Only modules ask and provide.** An app asks and answers through its backend module (`<app>_backend`), which declares the app's `uses` and `provides` in its own `metadata.json`; QML views make no intent calls. QML-only apps go away in 0.5, so no app is left without a way in.
2. **One presenter per core, fixed by the host at startup.** The frontend that starts `intents_module` is its presenter for that core's lifetime. There is no handover, so a second claim is always refused; a Signer CLI can't attach to Basecamp's core as presenter.
3. **The presenter has its own contract,** `intents_presenter.lidl`, used only by frontends. Apps never get bindings for it.
4. **A module only ever acts as itself, and the presenter decides when to interrupt.** `intents_module` knows modules and intents, not apps or screens. A request for another module's intent waits until the presenter opens it; the presenter, which sees the clicks, opens one at once when the person just clicked in the screen of the module that asked.
5. **Auto-return stays in the frontend.** `intents_module` reports a completed dispatch with its `handoff` flag; Basecamp applies its guards (frontmost, dialog open, dwell, circuit breaker) and decides whether to navigate.
6. **Rollout:** from 0.4.0 Basecamp runs `intents_module` in every build, beside its QML intent path: app backends and core modules ask and answer through the module, apps' screens on the QML path, unchanged. Each side reaches only its own providers; Basecamp's own intents answer on both. In 0.5 the QML path is turned off.

## The idea

- **Intents are for user actions.** A person has to decide, confirm or choose, or the requester mustn't know or depend on who provides the action. Everything else is a plain cross-module call.
- **The broker is a module.** `intents_module` is a core module **hosted by the shell**: Basecamp starts it in Basecamp, the `logoscore` daemon starts it for `logosctl` and CLI apps. Apps never load or list it; like `capability_module`, it's a platform service the host always runs.
- **Only modules talk to it.** Requesters and providers are modules, so the core attests every caller. Views reach intents through their own backend.
- **It knows modules and intents only.** What is installed, clicks, screens and starting apps come from the host.
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

## `intents_module`

| API | Called by | Does |
|---|---|---|
| **Requester** `request(intent, params)` → handle + receipt | any module | reads the caller from the core, checks `uses`, holds the request until the presenter opens it, resolves a provider, routes the result to the requester only |
| **Provider** `deliver(…)` / `respond(…)` | provider modules | receives a request and answers it |
| **Presenter** (the human hook) | the host's frontend only, fixed at startup | **pulls** its work and answers it: opening pending requests, choosers; declares the shell and what is installed |

**In the module:**
- the registry of who provides what, read from installed modules' manifests;
- the name and payload checks, and the error codes (`src/intent_rules.h`);
- holding each request for another module's intent until the presenter opens it, and resolving nothing before;
- the `unavailable` timing floor;
- rate limits;
- result routing.

**In each frontend:**
- drawing, and the pending list;
- seeing real input in each screen, and opening a request the person just clicked for;
- starting the provider the person picks;
- navigation: raising the provider, and auto-return after a dispatch completes.

**The presenter pulls.** `intents_module` announces new work with an event that carries only a counter; the presenter fetches the work with `next_work(secret)` and answers each item with `answer(secret, id, {...})`. Nothing has to call into the frontend's process, and every frontend (Basecamp, the Signer CLI, a standalone app) connects the same way.

| Work item (module → presenter) | Answer (presenter → module) |
|---|---|
| `pending`: a new request (dispatch, intent, requester) | `{"action":"open"}`, or `{"action":"dismiss"}` |
| `choose`: show a chooser (dispatch, intent, requester, providers) | `{"action":"choose","provider":m}` once that module runs, or `{"action":"cancel"}` |
| `deliver`: the shell itself provides the intent | `{"action":"respond","ok":b,"data":d,"error":e}` |
| `offer_install`: nothing installed provides an intent; the presenter may offer one from its catalogue | none |
| `present`: the module delivered to, to raise if it has a screen; `completed`: a dispatch completed, with `handoff`; `result`: the answer to the shell's own request; `dismiss`: take down an item already pulled | none: the frontend decides whether to raise or return the person |

Plus calls only the presenter makes: `declare_shell`, `set_installed` (what is installed), and `shell_request` for Basecamp's own requests and `basecamp://` links. A chooser work item lists installed providers, so the whole contract sits behind the presenter secret.

**How the presenter gets its secret.** The host loads `intents_module` straight after `capability_module`, before any app, and at once calls `claim()`. `intents_module` accepts the claim only from a `Host` caller (`currentCaller().isHost()`), only once, and answers with a fresh secret. Every presenter method after that requires the secret.
- **Why not hand the secret over at start.** The only private parent → child channel is the container's stdin pipe, which carries the module's auth token once and is then closed. Command-line arguments are readable by every process of the same user. Adding a second secret to the pipe would change `logos-container-subprocess`, `logos_host` and liblogos's loader API: a core change, for a protocol the new core replaces.
- **Why a `Host` check plus "once".** Modules call as `Module`. Besides Basecamp, `capability_module` calls as `Host`, and so do Basecamp's in-process QML views on logos-protocol below 0.7; under the daemon, every `logosctl call` is `Host` too. The module can't tell these apart, so "once, first" is what keeps them out: a deployment requirement on the host ([`security.md`](security.md#host-requirements)), not something the module checks.
- **Consequence: the presenter is the process that hosts the core.** Basecamp, a CLI app that embeds its own core, or the daemon itself. A Signer CLI can't become the presenter of a daemon it reaches through `logosctl`.
- **Restart.** If `intents_module` restarts, every in-flight request fails, and the host must claim the new instance before anything else reaches it. Until it does, any `Host` caller could claim first, and as presenter open requests and pick providers without the person; it still couldn't call a provider's `deliver`, which accepts only `intents_module`. Closing that window needs a presenter credential from the core.
- **The secret travels in a call reply.** `intents_module` answers every call directly (single dispatch), so neither the secret nor a result ever travels as a completion event, which over the Qt transport every replica of a module receives (threat 2).

## How apps declare and call intents

Apps don't list `intents_module` as a **package** dependency. A module lists the intents **contract** as an `interface_dependencies` entry: generated bindings only, nothing installed or loaded. `signer_manager_module` binds its signers the same way, through `signer.lidl`.

| Step | How |
|---|---|
| **Declare** | `uses` and `provides` in `metadata.json`, carried into the signed manifest. An app's go in its backend module's, which logos-module-builder copies from the app's. Modules add `intents.lidl` (and providers `intent_provider.lidl`) under `interface_dependencies`. |
| **Discover** | `intents_module` builds its registry from the installed modules' `uses` / `provides`. Nothing registers at runtime, so a package can't claim more than its manifest. |
| **Request** | `modules().bind_intents("intents_module")`, then `request` → handle + receipt, wait for `settled(handle)`, then `result(handle, receipt)`. A view that wants to ask calls its backend. |
| **Provide** | `intents_module` calls the provider's `deliver(dispatch, intent, params, requester)`; the provider answers with `intents.respond(dispatch, …)` and accepts `deliver` only from `intents_module`. If the person has to see something, the backend tells its own view through the `.rep`. |

The contracts live in this repo's `contracts/`, and they are the wire API:

| Contract | For |
|---|---|
| [`intents.lidl`](../contracts/intents.lidl) | requesters (`request`, `result`, `cancel`, event `settled`), and providers' `respond` |
| [`intent_provider.lidl`](../contracts/intent_provider.lidl) | providers: `deliver`, which only `intents_module` calls |
| [`intents_presenter.lidl`](../contracts/intents_presenter.lidl) | the presenter only: `claim`, then every other method with the secret; event `work_available` |

**Results are pulled with a receipt,** the pattern `signer_manager_module` uses too:
- a result can take minutes, longer than the 20-second call timeout;
- the `settled` event carries only a handle: no params, receipt or result. It still reaches every module, so it shows when requests finish ([`security.md`](security.md#known-limits));
- only the requester that asked, holding the receipt, can read the result;
- requesters don't need to expose a method for the broker to call.

Why not a package dependency:
- Every app would have to list a platform service.
- Per-app version ranges would let apps pin different broker versions.
- A dependency is something a package resolves and installs, which conflicts with "reserved name, shipped with the host, never from a catalog".

## When to interrupt the person

**Why.** A request the person just clicked for may open a dialog or raise an app at once. Any other request must not pop up over what the person is doing; it waits in the pending list, shown alongside activity.

**Who decides: the presenter.** It is the one that sees input. An app's QML view is a `QQuickWidget` inside Basecamp's own process, so every click and key press reaches Basecamp's window before the view, and Basecamp watches input per view with an event filter. Browsers use the same idea ("user activation") to allow popups only after a real click.

**How.** Every request for another module's intent reaches the presenter as a `pending` item, before anything about it is resolved. Basecamp opens it at once when the person clicked within about 2 s in the screen of the module that asked, and uses that click up; otherwise the request stays in the pending list until the person opens it. A terminal presenter, already in front of the person and showing one request at a time, may open everything; a graphical one must not. Nothing about clicks crosses IPC, and views make no intent calls.

**After a hand-off.** The click that started a hand-off was in the requester's screen, so the provider shows what was asked, and asks for anything that needs approval once the person clicks in its own view. Package Manager: `packages.install` shows the package, and clicking Install there asks for the install dialog.

**What the module keeps.** Nothing about what is installed is looked up before a request is opened, so asking reveals nothing. The pending list is bounded per requester (4) and in time (30 minutes).

## Flows

**A. Basecamp, the person clicks**
```
Wallet view click ─► Basecamp notes the input ─► backend.send(tx)
wallet_ui_backend ─intents.request(intent, params)─► intents_module: pending
Basecamp pulls it: a click in Wallet's screen just now ─► open
intents_module ─chooser─► Basecamp draws it; the person picks Signer; Basecamp starts it if it isn't running
intents_module ─deliver─► signer_ui_backend ─► its view shows the request ─► the person confirms ─► respond
intents_module ─settled─► wallet_ui_backend pulls the result
```

**B. Basecamp, a module asks with no click**
```
wallet module ─request─► intents_module: pending
Basecamp keeps it in the pending list alongside activity ("Wallet wants you to sign"), without interrupting
the person opens it ─► same as A from the chooser
```

**C. Signer CLI app, its own core, no Basecamp**
```
the host (the CLI app's core or the daemon) runs intents_module; the CLI app is its presenter (terminal) and opens every request; its manifest provides signing.approve
wallet module ─request─► intents_module ─deliver─► CLI app ─► prompt, QR codes, approve ─► respond
intents_module ─result─► wallet module
```

The requester's code is the same in A–C, and the provider contract is the same for the Basecamp Signer app and the Signer CLI app.

## API: the same everywhere

| Call | Module (backend, core module, CLI app) | QML view |
|---|---|---|
| Ask | `intents.request(intent, params)` → handle + receipt; `settled(handle)` → `intents.result(handle, receipt)` | calls its own backend |
| Provide | `deliver(dispatch, …)` → `intents.respond(dispatch, …)` | shows what its backend tells it |

**The same rules for every caller:**
- the name grammar;
- plain-data payloads (1000-node cap);
- the error codes `not_declared`, `unavailable`, `bad_request`, `cancelled`, `timeout`, `failed`.

**Long waits:** a request can wait minutes for the person, longer than a call's 20-second timeout. So `request` answers at once with a handle and a receipt, and the requester pulls the result once `settled(handle)` fires.

## Making intents generic (no core changes)

**1. The contract doesn't assume QML or Basecamp.**
- Each `provides` entry declares its `params` and its **result** shape, in the same syntax. A successful answer that doesn't fit its `result` reaches the requester as `failed`.
- The requester is the identity the platform gives the calling module. Providers don't check for `main_ui`.
- Whether to interrupt the person is the presenter's decision, not something a request carries.
- Raising the provider (`present`, `handoff`) is a frontend hint that a CLI ignores.

**2. Names say what's wanted, not who does it.**

| Namespace | For | Example |
|---|---|---|
| domain | what is wanted; any provider fits | `signing.approve {service, handle}`, `wallets.unlock` |
| `basecamp.*` | what only Basecamp does | `basecamp.settings.open` |
| `logos.*` | the platform (reserved) | — |

The namespaces are enforced: `logos.*` and the shell's prefix are refused from packages. Names are free strings: a provider that wants a generic name declares it next to its old one. `evm.signing.approve` → `signing.approve` is for the Signer CLI to settle.

## Security

`intents_module` is reachable and replaceable like any module, so it protects itself. The numbers below are the ones `docs/security.md` cites; that file says how each is handled in the code, and what is left to the host.

**Threats**

| # | Threat | Mitigation |
|---|---|---|
| 1 | Under `logoscore`, a `logosctl call` arrives as `Host` (developer guide). A "Host is the presenter" rule would let any `logosctl` user claim the presenter role, open requests, and skip the person. | The frontend that starts `intents_module` claims the presenter role **once, at startup, before any app loads and before the daemon accepts `logosctl`**, as a `Host` caller, and gets a one-time secret, and claims again first after any restart of the module. A second claim is always refused; there is no handover. The module can't tell which `Host` caller claims first, so the order is a deployment requirement. |
| 2 | A deferred call's **reply** travels as a `__logos_call_complete__` event, which over the Qt transport every replica of the module receives; before logos-protocol reserved that name (8bdb552), any subscriber could also read it. | `intents_module` answers every call directly (single dispatch, held by a flake check), so the secret and results never travel as events. Never put request or result content in an event. Hosts should still run a protocol with the reservation. |
| 3 | `logoscore` can give a module a `tcp` / `tcp_ssl` listener, which makes the broker reachable from the LAN. | Callers are identified by their token on every transport, so the checks hold; the secret and results travel in replies, so a module exposed beyond the machine needs `tcp_ssl`. |
| 4 | Whether the person clicked could be forged or replayed. | No proof of a click crosses IPC: the presenter, which sees real input, opens pending requests itself, once per click. |
| 5 | Strings shown in a terminal can carry escape sequences that fake the screen. | Strip control characters; plain text only, as `evm_signer_cli` does for names. |
| 6 | Any module can call a module that has no access rule. | Check the caller on every method. `respond` is accepted only from the provider the dispatch went to (dispatch id **and** caller). There's no "list providers" method; choosers reach only the presenter. |
| 7 | The broker can be replaced by a module with the same name. | Reserved name, bundled with each frontend, never installable from a catalog. How a frontend checks the files it loads is still open (open question 5). |
| 8 | Sensitive payloads pass through one more process. | Memory only, size caps, never logged. |
| 9 | A module named `<app>_backend` could pose as an app's backend. | A module only ever acts as itself, with the declarations in its own `metadata.json`; a real backend's ship in the app's package, and installers reserve `<app>_backend` names. |
| 10 | A requester's rights change while its request is open: it is uninstalled, updated to drop the intent, or left out of a new restriction. | The request ends: it is checked again whenever the registry changes, and right before delivery. |

**On the CLI too** (the Signer CLI, step 6, isn't built yet)

| # | Threat | Mitigation |
|---|---|---|
| 11 | Provider identity is a name from unsigned `metadata.json`. | A CLI prompt shows the package name and whether it is signed, as Basecamp's chooser does. |
| 12 | A provider chosen by configuration could be rerouted by editing it. | None is: every delivery goes to the provider the person picks. |
| 13 | Two presenters on one core. | Impossible: one presenter per core, fixed at startup, no handover. |
| 14 | `basecamp://` links, catalog install offers, grants by role. | They stay in Basecamp's presenter. Only the presenter may submit as the link requester. |
| 15 | The module crashes mid-request. | Fail closed: the new instance knows none of the old requests, so requesters get `unknown_handle` and treat them as failed, the presenter's own requests fail, and frontends clear stale dialogs. A provider that already acted isn't stopped. |
| 16 | The timing floor is noisier over IPC. | Keep it, and make the answers for absent, denied and malformed identical. |

Consent is drawn by the frontend and the provider, never the requester. The requester's identity is the core's attestation of the calling module.

**Refusal tests,** all in the unit tests:
- a `logosctl` caller claiming the presenter role;
- a second presenter claim;
- a claim from a `Module` caller;
- any presenter method called without the secret;
- a module named like the shell, the link requester or an installed app;
- `respond` from the wrong module;
- a request whose requester no longer declares its intent, or a new restriction leaves out;
- a display name containing escape sequences.

## Steps

| # | Step | Needs | Status |
|---|---|---|---|
| 1 | Write the generic contract: result schemas, attested requester, `handoff` as a hint; split the namespaces | — | **Done** (`contracts/`) |
| 2 | Generic names for current intents, with aliases (`signing.approve` first) | 1 | **Dropped**: names are free strings, see "Names say what's wanted" |
| 3 | `intents_module`: the broker and registry behind requester / provider / presenter APIs (the presenter pulls), with the security rules above | 1 | **Done**: this repo |
| 4 | Basecamp: becomes the presenter at startup, pulls its work, starts what the person picks; auto-return driven by completion notices | 3 | **Done** (`logos-basecamp`); until the Activity view shows the pending list, opens at once only what a trusted package asks |
| 4b | Basecamp: watches input per view, and opens a pending request on a click in that module's screen | 4 | **Done** (`logos-basecamp`): for packages the person doesn't trust, and for every request with `--intents-attention interactive` |
| 5 | Apps ask and provide through their backend module, which declares the app's intents | 3 | **Done** (`logos-module-builder`, `logos-view-module`) |
| 6 | CLI presenter and provider (the Signer CLI app) | 3, 5 | Not started |
| 7 | Move every app's intent calls from its view to its backend (requesters and providers); update the tutorials and guides | 3, 5, and each app's `"backend": "module"` port | Started: Package Manager UI; no other app |
| 8 | 0.5 switch: QML intent path off in `logos-view-module-runtime` and Basecamp | 4, 4b, 7 | 0.5 |

Steps 1–7 are built during 0.4, and 0.4 releases run `intents_module` beside the QML intent path. Step 8 ships in 0.5, with the removal of QML-only apps.

**Still missing before the switch:**
- the pending list next to activity, so Basecamp opens only what the person clicked for;
- IPC tests that drive the real contracts between modules, not only the unit tests;
- an open finding from review: a `result` or `dismiss` work item can be evicted when the presenter stops pulling, so the result of the shell's own request can be lost. Keeping results by handle for the presenter to fetch changes the presenter contract.

**Later, from the core:** module lifecycle (`modules_state`), so the installed list comes from the core instead of `set_installed`, and a provider that dies after taking a delivery fails at once instead of at its deadline.

## Open questions

1. Does the new core broker intents between modules? If it does, steps 3–6 shrink to adopting it.
2. How the pending list looks next to activity (one list or a section, badges, expiry of old requests): decided in the activity discussion.
3. Should `evm_signer_cli` grow into the Signer CLI app, or be replaced by it?
4. **Enforced access policy.** With `--access-policy enforce` (opt-in in both Basecamp and the daemon, off by default), a module may call only the modules it declares as dependencies. Apps don't declare `intents_module` as a dependency, and `intents_module` is no provider's dependent. So under enforcement every `request` and every `deliver` is refused. It needs either an explicit policy entry written by the frontend, or `intents_module` in the core's trusted/exempt lists (a core change).
5. How a frontend checks that the `intents_module` it loads is the one it bundled (threat 7).
6. **Shared intent contracts.** Each provider declares its own `params` and `result`, so two providers of one intent can disagree, and a requester that doesn't know which one answers can't rely on either. Should an intent have one versioned contract, with providers free to add optional fields?
