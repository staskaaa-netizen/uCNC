# nc_sender - how it is checked

```powershell
python tools\test_nc_sender_module.py
```

The script builds the emitter, G7x and lathe compensation with no NC panel or
framebuffer and runs the module's own host test.

Software-verified:

- `U` and `W` are resolved to absolute `X`/`Z` words;
- direct controller lines and setup/comment/cycle exclusions;
- source-line filtering;
- bounded stream termination;
- compensated cycles resolve their radius through the caller's callback.

The full panel checks still run through `python tools\test_ncc.py`, which keeps
the previous expansion, run, pacing, compensation, state and block-mark
behaviour. That suite exercises the run portion against the fake controller;
the standalone script currently covers the emitter.

Hardware still needs the same bench checks as before: controller acceptance of
the emitted stream and the real machine's move ordering.
