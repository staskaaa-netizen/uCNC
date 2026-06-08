# NC TODO

- Later: from RUN, jump to EDIT with the same file and line marked.
- Later: when EDIT and SIM are intentionally linked, entering SIM should reopen the edited file without making RUN share that file.
- Promote `g71_g72` into the parser/runtime path so NC streams source lines as-is. RUN is source-line based now; `nc_emit` remains only as the temporary SIM/preview bridge until parser-side G7x owns expansion.
