# Upstream integration

## 2026-09-15

- Core: `https://github.com/Paciente8159/uCNC.git`, master `31e5686b`.
- Vendored modules: `https://github.com/Paciente8159/uCNC-modules.git`, master `66063076`.
- Updated installed upstream modules: `g33`, `g7_g8`, `sd_card_v2`.
- Local NC, G7x, LeanCam, display, keyboard and hardware encoder modules are retained.

The local repository started with a source snapshot, not upstream Git history.
Core changes were merged using upstream `a6898fbd` (the closest original snapshot,
560 identical file blobs) as the explicit three-way baseline. The integration
commit includes the current upstream core as a parent, so later core updates can
use an ordinary Git merge. Installed module changes used `88da78f2` as their
pre-update baseline. Future module updates should use `66063076` as their base
and map module paths under `uCNC/src/modules/`.

Recovery references:

- `checkpoint/pre-upstream-20260915`: original branch tip.
- `checkpoint/local-work-20260915`: stash commit containing tracked changes and
  untracked files before integration; retained even after restoration.
- `06f4f0da`: source checkpoint including local changes before the upstream merge.

The RP2350 LVDS configuration and local module loading remain project-specific.
Networking follows upstream's socket API. The common encoder implementation
comes from upstream, with compatibility adapters for the local indexed hardware
reader callbacks. No hardware is flashed by the integration/build checks.

## Runtime follow-up (2026-09-16)

The G7x runtime follow-up adds serialized generated-block submission, native
single-line G76 and NC cancellation/error cleanup. Generator, NC emitter and
virtual-MCU parser/planner suites pass with `python tools/test_g7x.py all`.
RP2350-LEANCAM-LVDS and RP2350-G7X-MODULE compile successfully. Builds still
report existing module-macro redefinition warnings. These checks validate
software integration; spindle synchronization and machine operation remain
unverified. See `uCNC/src/modules/g7x/README.md` and the NC hardware checklist.
