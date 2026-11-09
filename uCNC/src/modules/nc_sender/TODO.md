# nc_sender remaining work

- [ ] **Remote uCNC transport is this module's job** (requested 2026-11-09).
      `nc_sender` is the intended owner for sending expanded, numbered blocks to
      another uCNC board over serial. It already owns expansion, pacing, queueing
      and execution-line mapping, but that path is currently wired to the local
      `grbl_stream` and core events. The future change is a transport interface
      beneath the sender - local controller and remote serial implementations -
      without moving editor, screen or file ownership into the sender.

- [x] The pacer and execution-line mapping now belong to this module.
- [ ] Add a standalone pacer host test with scripted planner/interpolator
      doubles. The panel's `--run2test`, `--pace2test`, `--state2test` and
      `--runlock2test` currently exercise it through the station.
- [ ] Keep the standalone test's source provider small. It should stay a
      text-line source; files, card access and editor state belong to callers.
- [ ] Rename the remaining `[MSG:NCC ...]` diagnostic prefix only if the
      station and bench logs are updated in the same change. The prefix is
      operational output, not an API.
