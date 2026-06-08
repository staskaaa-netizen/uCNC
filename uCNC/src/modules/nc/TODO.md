# NC TODO

- Later: from RUN, jump to EDIT with the same file and line marked.
- Later: when EDIT and SIM are intentionally linked, entering SIM should reopen the edited file without making RUN share that file.
- Promote `g71_g72` into the parser/runtime path so NC streams source lines as-is. Then shrink/delete the temporary `nc_emit` bridge: NC should not own a second large G7x emitter.
