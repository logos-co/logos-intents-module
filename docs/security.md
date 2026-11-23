# intents_module security

`intents_module` is reachable like any module, so it checks every caller
itself.
Numbers in parentheses refer to the gaps in [`design.md`](design.md#security).

Threat model: the presenter (the host frontend holding the secret) is trusted.
Every installed package is not — its `metadata.json`, its module calling any
method with any arguments at any rate, its replies as a provider, and its choice
of name. Neither is a `logosctl` user before the host has claimed.

## Enforced by the module

### Who is who

| Rule | Where | Test |
|---|---|---|
| The presenter role is claimed once, by a `Host` caller; no handover (1, 12) | `IntentsService::claim` | `the_presenter_role_is_claimed_once`, `a_module_cannot_claim_the_presenter_role` |
| Every presenter method needs the secret **and** a `Host` caller | `presenterCall` | `every_presenter_method_needs_the_secret`, `the_secret_is_useless_to_a_module` |
| A `Host` caller can't request directly (under the daemon that is any `logosctl` user) | `requesterForCaller` | `a_host_caller_cannot_request_directly` |
| A module named like the shell, the link requester or an installed app is refused; shell and link requests are marked by the presenter's entry point, never by name | `requesterForCaller`, `IntentBroker::submit(fromPresenter)` | `a_module_named_like_the_link_requester_is_refused`, `a_module_named_like_the_shell_is_refused`, `a_module_named_like_an_app_is_refused`, `a_request_under_the_shells_name_is_the_shells_only_from_the_presenter` |
| `respond` only from the module the dispatch went to (6) | `IntentBroker::respond` | `respond_is_accepted_only_from_the_module_delivered_to`, `only_the_module_delivered_to_may_respond` |
| Results need the requester's identity and receipt; `settled` carries a handle only (2) | `IntentBroker::result` | `the_settled_event_carries_only_the_handle`, `another_module_cannot_read_a_result` |
| Each shell declaration replaces its restrictions whole; one whose `restricted` can't be read (not an object, an empty list) is refused and changes nothing | `IntentsService::declareShellLocked`, `IntentRegistry::setRestrictions` | `a_shell_declaration_replaces_its_restrictions_whole`, `restricted_intents_admit_only_their_requesters` |
| `logos.` and the shell's namespace can't be provided by a package, even one read before the shell was declared | `IntentRegistry` | `reserved_namespaces_are_refused_from_packages`, `a_late_shell_or_link_declaration_still_applies` |
| A package's `metadata.json` must carry the name the host registered; names are `[A-Za-z0-9_.-]{1,64}` | `IntentRegistry::rebuild`, `isValidPackageName` | `rebuild_reads_disk_and_checks_the_name`, `package_names_with_anything_but_plain_characters_are_skipped` |

### What the person sees

| Rule | Where | Test |
|---|---|---|
| Every request waits as pending until the presenter opens it, and nothing is resolved before, so asking reveals nothing about what is installed | `IntentBroker::start` | `a_new_request_waits_until_the_presenter_opens_it`, `unopened_requests_reveal_nothing_about_what_is_installed`, `unopened_requests_wait_alike_whatever_is_installed` |
| Install offers only for opened requests, once per intent a minute | `offerInstall` | `install_offers_need_an_opened_request`, `an_install_offer_comes_once_opened_and_once_a_minute` |
| `unavailable` for absent, denied, unreachable and unloaded alike, never before 400 ms (15) | `failWithFloor` | `nothing_provides_it_is_unavailable_but_only_after_the_floor` |
| One provider isn't a reason to skip the chooser, for the shell's own requests too; only the shell as the provider is | `IntentBroker::start` | `a_shell_request_with_one_package_provider_still_asks` |
| One chooser at a time; one shown gets its full 10 minutes from when it's shown | `presentChoice`, `showChoice` | `only_one_choice_is_on_screen_and_the_next_follows`, `a_queued_choice_gets_its_full_time_once_shown` |
| A chooser lists the providers of the moment it's shown, not when it was queued, and a pick is delivered only if that provider is still allowed, still reachable by the requester (links: `web`) and unchanged since it was shown; otherwise the person is asked again | `showChoice`, `stillEligible`, `IntentRegistry::revisionOf` | `a_provider_changed_under_the_chooser_is_offered_again`, `a_queued_chooser_shows_the_providers_of_the_moment_it_comes_up`, `a_link_request_cannot_go_to_a_provider_that_dropped_web` |
| Names a presenter may draw are stripped of control, bidirectional, zero-width and line-separator characters (5) | `displaySafe` | `display_text_loses_control_characters`, `work_items_show_display_safe_names` |
| No method lists providers; choosers reach only the presenter (6) | API | — |
| Before a presenter claims, nothing is shown or delivered: every request is `unavailable` | `IntentBroker::start` | `nothing_is_delivered_before_a_presenter_claims` |

### Staying up

| Rule | Where | Test |
|---|---|---|
| JSON from outside is checked for size, nesting and value count before anything is built: payloads 1 MiB / 8 levels, manifests 1 MiB / 32 levels, `deliver` replies 64 KiB (a larger reply isn't an acceptance) | `parseBounded` | `json_past_its_limits_is_refused_before_it_is_built`, `deeply_nested_metadata_cannot_crash_set_installed`, `an_oversized_deliver_reply_is_not_an_acceptance`, `a_deeply_nested_deliver_reply_cannot_crash_the_module` |
| Nothing a caller sends is parsed until the caller is known to be entitled: a declared requester within its rate, the provider a dispatch waits on | `request`, `respond` | `a_response_from_a_stranger_reads_nothing` |
| At most 64 `provides` / `uses` entries and 32 params per entry per package; a manifest is read only if it is a regular file, and the open never waits (the host runs one call at a time, so a FIFO would stall everyone) | `IntentRegistry`, `readManifest` | `a_package_declares_at_most_64_entries`, `a_huge_manifest_is_refused_quickly`, `web_opt_ins_do_not_slow_down_rebuilds`, `a_fifo_manifest_cannot_stall_set_installed` |
| Per requester, the shell and links included: 20 requests at once and 5 a second, 16 open, 4 pending, 4 MiB of open params, 64 results and 4 MiB of results; 64 MiB of results and 1024 open requests in all. A refusal over these limits holds no open slot while it waits out the floor, and at most 1024 wait at once; past that they settle at once | `BrokerConfig`, `holdRefusal` | `requests_past_the_rate_are_refused_in_the_reply`, `the_presenters_own_requests_have_a_rate_too`, `the_open_request_limit_holds_while_refusals_wait_out_their_floor`, `refusals_waiting_out_their_floor_are_bounded`, `open_requests_stay_within_each_requesters_byte_budget`, `results_stay_within_each_requesters_byte_budget`, `results_stay_within_the_global_byte_budget`, `an_oversized_result_is_refused` |
| `not_declared` and rate refusals come back in the reply, with no event; `settled` never goes out before its request has returned | `IntentsService::request` | `undeclared_requests_raise_no_events`, `settled_is_never_sent_before_the_request_returns` |
| Work items someone must answer are never dropped from the queue, and there are never more of them than open requests; past 1000 items the oldest notice goes, then the oldest `result` or `dismiss`. Items that end before they're pulled are withdrawn, not answered with `dismiss` | `pushWork`, `dropWorkTarget` | `the_work_queue_stays_bounded_when_the_presenter_stops_pulling`, `withdrawn_requests_cannot_crowd_out_another_apps_chooser`, `a_flood_of_notices_cannot_push_out_an_item_awaiting_the_person`, `the_work_queue_does_not_keep_withdrawn_payloads` |
| Presenter input is read with checked reads, and an answer changes nothing unless it is valid; any exception that still escapes becomes a refusal | `answer`, `claim`, `guarded` | `a_malformed_presenter_answer_changes_nothing`, `a_shell_answer_with_a_null_error_completes` |
| Every log line is one line: untrusted text is escaped and truncated, payloads are never logged (8) | `forLog` | `log_fields_stay_on_one_line`, `an_intent_name_cannot_forge_log_lines`, `metadata_cannot_forge_log_lines` |
| A provider's answer must fit the `result` it declared (required fields, declared types; others pass), or the requester gets `failed` | `IntentBroker::resultViolation` | `an_answer_that_misses_its_declared_result_is_failed`, `a_provides_entry_describes_its_result_like_its_params` |
| A provider can't mint `not_declared` / `unavailable` | `normalizeError` | `a_provider_cannot_send_back_a_broker_only_code_or_a_bad_payload` |

## Left to the host

- **Claim early.** The host loads the module after `capability_module`, claims
  at once, and only then loads apps or accepts `logosctl`.
- **Reserved names.** Every installer refuses packages named `intents_module`,
  or like the shell or link requester the presenter declares (Basecamp:
  `main_ui`, `logos_link`). The module refuses such a module as a requester, but
  the name must not be installable in the first place (7).
- **Tell the module about every change.** Call `set_installed` after each
  install or uninstall, before the new package loads, with every app's backend
  as the module it is.
- **Reserve backend names.** Installers refuse packages named like an app's
  backend (`<app>_backend`), not only bundled ones; otherwise a package
  installed first takes the name the app's backend asks and answers under.
- **Restrict the shell's confirm intents.** Any package that declares a shell
  intent may ask for it. List every `confirm_*` intent under `restricted`,
  naming the Package Manager's backend module (`package_manager_ui_backend`)
  (Basecamp's declaration leaves out `basecamp.packages.confirm_install`), in
  every declaration: each one replaces the last whole.
- **Decide when to interrupt.** Open a pending request at once only on a real
  click in the requesting module's own screen, the one the host loaded it for,
  once per click and within about 2 s; leave the rest in the pending list.
  Without a pending list, open everything.
- **Start what the person picks.** Before answering `choose`, start the chosen
  module, or the app it belongs to; a provider that isn't running fails as
  `unavailable`.
- **Who holds `Host`.** `capability_module`, and on logos-protocol below 0.7
  Basecamp's in-process QML views, call as `Host` too. The claim right after
  loading, including after a restart of `intents_module`, is what keeps them
  out of the presenter role.
- **Let the module reach app backends.** Basecamp restricts each app's backend
  to its view (`PluginLoader::restrictBackendToView`); `intents_module` must be
  on that list, or every delivery to an app fails as `unavailable`.
- **Minimum daemon version (2).** Older daemons broadcast method replies as
  events, which would publish the secret and every result. The host refuses to
  start the module below that version.
- **Remote transport.** Callers are identified by the token they present, so the
  checks above hold over `tcp` too; but the secret and results travel in call
  replies, so a module exposed beyond the machine needs `tcp_ssl`.
- **`--access-policy enforce`.** Not usable yet: requests and deliveries aren't
  dependency edges. Enforce mode already blocks every app → backend call for
  the same reason; the fix for both is a frontend policy entry.
- **Same-user processes.** A native module runs as the user and can rewrite any
  package's `metadata.json`; the registry trusts what is on disk at each
  `set_installed`. A file on a FUSE mount the package controls can still block
  the read.

## Known limits

Accepted for now; each is a policy choice, not a bug in a check.

- **The work counter is public.** `work_available(counter)` reaches every
  module. A new request always adds one `pending` item, whatever is installed;
  but once the presenter opens it (at once in Basecamp for now, and on a
  click), a provider adds a chooser where none adds at most one install offer
  a minute, so the 400 ms floor doesn't hide what is installed. It also shows
  how busy the person is.
- **A presenter that opens everything has no pending list.** Any declared
  requester can queue up to 16 choosers at a time, ahead of newer ones.
- **A provider that dies after taking a delivery** is noticed only at the
  request's deadline, up to 10 minutes: nothing reports module lifecycle to
  the module yet.
- **A chooser can be replaced right after a cancel.** Answers name their work
  id, so a stale click fails, but a presenter that redraws in place should not
  take input on a new chooser at once.
- **Display names are labels the package picks.** Control, bidirectional and
  zero-width characters are removed; homoglyphs, tag characters, variation
  selectors and length are not. The presenter shows the module name beside
  every display name, and treats `iconSource` as untrusted.
- **Global budgets are shared.** Packages acting together can use up the 64 MiB
  of results (evicting anyone's oldest) or the 1024 open requests.
- **Non-string `deliver` replies** are parsed by the SDK before the module sees
  them (and then count as not taken), so the 64 KiB limit applies only to
  string replies.
- **Delivery replies after unload.** Provider clients are the SDK's
  (`bind_intent_provider`) and last as long as the process. A reply after
  `aboutToUnload` still reaches the service, which is alive until then, and
  one after the service is gone is dropped (`m_alive`); one racing the
  service's destruction at exit isn't excluded. Closing a module's clients at
  unload is for the SDK, for every module.
