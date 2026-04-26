# WebUI guide: mining-pool credential fields

Audience: WebUI agents / contributors editing the BTClock settings UI
(the `data/` Svelte submodule). This document describes the firmware's
contract for the two shared mining-pool credential slots so the UI can
render the right labels, suppress the right inputs, and avoid leaking
secrets back to the user.

## The two shared fields

The firmware exposes a single pair of mining-pool credential keys via
`/api/settings`:

| Key | Type | Purpose |
|---|---|---|
| `miningPoolUser` | string | Primary identifier — payout address, username, worker name, OR API key |
| `poolWorker` | string | Optional secondary identifier — worker name OR subaccount |

Per-pool fields like `viabtcApiKey`, `foundryApiKey`, `foundrySubacct`
do **not** exist. Pools that need credentials overload these two slots.
The firmware reads the right slot internally based on the active
`miningPoolName`.

## Per-pool field semantics

Render labels + placeholders + visibility based on the currently-selected
`miningPoolName`. The field itself never changes — only the WebUI's
presentation does.

| `miningPoolName` | `miningPoolUser` label | `miningPoolUser` is secret? | `poolWorker` label | `poolWorker` shown? |
|---|---|---|---|---|
| `ocean` | "Payout address" | no | — | hide |
| `noderunners` | "Payout address" | no | — | hide |
| `satoshi_radio` | "Payout address" | no | — | hide |
| `braiins` | "Username" | no | "Worker name (optional)" | show |
| `public_pool` | "BTC address" | no | — | hide |
| `local_public_pool` | "BTC address" | no | — | hide |
| `gobrrr_pool` | "BTC address" | no | — | hide |
| `ckpool` | "BTC address" | no | "Worker name (optional)" | show |
| `eu_ckpool` | "BTC address" | no | "Worker name (optional)" | show |
| `nerdminers_org` | "BTC address" | no | — | hide |
| `nerdminer_io` | "BTC address" | no | — | hide |
| `viabtc` | "API key" | **yes** | — | hide |
| `foundry_usa` | "API key" | **yes** | "Subaccount name" | show |

The "is secret?" column drives input type (`type="password"`),
autocomplete suppression (`autocomplete="off"`), and the protocol below.

## The secret-suppression protocol

When the active pool's `miningPoolUser` is a secret (ViaBTC, Foundry):

- `GET /api/settings` **omits** the raw `miningPoolUser` value entirely.
- Instead, the response includes a companion `miningPoolUserSet` boolean:
  - `true` → an API key is stored in NVS (don't make the user re-enter)
  - `false` → no key stored yet
- `PATCH /api/settings` **does** accept `miningPoolUser` writes — that's
  how the user enters or rotates the key.

For public-info pools, `miningPoolUser` rides plaintext as normal and
`miningPoolUserSet` is **not emitted**.

### Recommended UX for the secret slot

- **`miningPoolUserSet === true`**: render the input as `placeholder="●●●●●●●● (saved)"`, value empty. A user who wants to rotate the key clears the field, types the new value, saves. A user who wants to keep the existing key just doesn't touch it — a PATCH that doesn't include `miningPoolUser` leaves NVS unchanged.
- **`miningPoolUserSet === false`**: render the input as `placeholder="Paste your API key"`, value empty.
- **Switching pools**: when the user picks `viabtc`/`foundry_usa` in the dropdown, immediately re-render the labels per the table above. The PATCH for `miningPoolName` will trigger a fresh GET that surfaces `miningPoolUserSet` for the new pool — but the WebUI can predict the visibility from the table without waiting.
- **Switching FROM a secret pool TO a public pool**: `miningPoolUser` will start emitting plaintext again. The previously-stored API key is still in NVS — it's just that the public pool will read the same slot and try to use it as an address (likely failing). The user typically wants to clear and re-enter; the WebUI can prompt.

## State diagram for `miningPoolUser`

```
       (active pool is secret-key?)
              │
       ┌──────┴──────┐
      yes            no
       │             │
       ▼             ▼
GET omits raw    GET emits raw
miningPoolUser   miningPoolUser
                 (no companion)
       │
       ▼
GET emits
miningPoolUserSet bool
```

## Multi-pool credential isolation

A single device can only run one pool at a time. If the user switches
from Braiins → ViaBTC → Braiins, they will need to re-enter the Braiins
username (the slot was overwritten by the ViaBTC API key in between).
The firmware does not maintain per-pool credential history. If a future
WebUI wants this, it would need to keep a local-storage cache keyed by
pool name and re-PATCH on switch — and warn the user the cache lives
only in their browser.

## Reference

- Source of truth for the schema: `components/settings/include/settings/schema.hpp`
- Suppression list: `components/settings/settings_api.cpp` — search for `kSecretUserPools`. New keyed pools must be added there AND in `main/io/mining_pool_selector.cpp`.
- Per-pool override that drives the suppression: `bool user_is_secret() const` on `PoolDataSource` (default `false`); ViaBTC and Foundry override to `true`.
- Full settings reference: `docs/SETTINGS.md`.
