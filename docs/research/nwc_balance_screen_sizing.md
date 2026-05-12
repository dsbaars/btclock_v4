# NWC Balance Screen — Flash Delta Per Variant

End-to-end NWC integration on top of commit `4e6c68b` (NIP-47 client
base — get_balance + notification scaffolding). The delta covers:
boot wiring (`init_nwc.cpp/.hpp`), settings reader (`nwc_config.*`),
schema fields + URI validation in `settings_api.cpp`, DataSnapshot
fields + Merge logic, panel-text mirror, ScreenManager dispatch
(`current_kind`, `MaybeAutoRotate`, `Render`, `SetNwcPaymentNotify`),
balance + payment-notify renderers (`screens/nwc_balance.cpp`),
event-loop dispatch, slot-map shift (kAgnosticSlots 8→9).

Built with ESP-IDF v6.0.1, sdkconfig defaults per variant.

| Variant       | Flash | Baseline (`4e6c68b`) | NWC stack | Δ (bytes) | Δ (%) | Free after  |
|---------------|-------|---------------------:|----------:|----------:|------:|------------:|
| Rev A 2.13"   | 4 MB  | 1,731,216            | 1,760,272 | +29,056   | +1.68 | 51% (app)   |
| Rev A 2.9"    | 4 MB  | 1,731,296            | 1,760,336 | +29,040   | +1.68 | 51% (app)   |
| Rev B 2.13"   | 8 MB  | 1,736,240            | 1,765,312 | +29,072   | +1.67 | 51% (app)   |
| V8 2.13"      | 16 MB | 1,733,488            | 1,762,512 | +29,024   | +1.67 | 51% (app)   |

Notes:

- The deltas are nearly identical across variants because the NWC code
  paths compile to the same machine code on every board (the only
  per-variant code in the project is the EPD driver pin map).
- `+29 KiB` is the full vertical: NwcClient state machine + JSON-RPC
  codec + NIP-04/44 v2 dispatch (lwf.3 was already linked, NWC just
  exercises it), settings reader + GET/PATCH validators, DataSnapshot
  fields, two new renderers, ScreenManager bookkeeping.
- The Rev A free-flash budget after the change still has ~30 KiB
  headroom on the smallest app partition (`0x370000` = 3.5 MiB, with
  ~1.76 MiB used). The estimate in `docs/research/nwc_nip47_estimate.md`
  (~22 KiB for the NWC component itself) was the dominant pre-existing
  contributor; this delta adds the integration glue around it.
- LittleFS partition sizes are unchanged; the WebUI section was
  deferred to a follow-up so `data/build_gz/` is not in this delta.
