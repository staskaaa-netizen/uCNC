# uCNC-modules

Addon modules for uCNC - Universal CNC firmware for microcontrollers

## About G7-G8 for uCNC

This module adds custom G7/G8 code to the uCNC parser for lathe X-axis diameter/radius mode.

* `G7` selects diameter mode. Programmed `X` words are diameters, while uCNC motion still receives radius values internally.
* `G8` selects radius mode. Programmed `X` words are used as radius values.
* Diameter conversion is applied only when the parsed command contains an `X` word, so Z-only moves and modal bookkeeping are not disturbed.

The module also exports helper functions in `parser_g7_g8.h` so other parser modules can share the same conversion rule instead of duplicating X scaling logic.

## Adding G7-G8 to uCNC

To use the G7-G8 parser module follow these steps:

1. Copy the `G7-G8` directory and place it inside the `src/modules/` directory of uCNC.
2. Load the module inside uCNC. Open `src/module.c` and at the bottom of the file add the following line inside `load_modules()`:

```c
LOAD_MODULE(g7_g8);
```

3. Enable `ENABLE_PARSER_MODULES` inside `cnc_config.h`.
