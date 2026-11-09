# Vendored tusb_libdlo

This directory holds a verbatim copy of the `src/` tree of
[ianhan/tusb_libdlo](https://github.com/ianhan/tusb_libdlo), with the local
patches listed in `PATCHES.md` applied.

| Item | Value |
| --- | --- |
| Upstream | https://github.com/ianhan/tusb_libdlo |
| Commit | `8124b0e6708ab4ae7720394fa317cf0efc153295` |
| Commit date | 2026-09-10 |
| Commit subject | Add DisplayLink video blanking and USB wait callback |
| Copied on | 2026-10-16 |

Why it is vendored rather than added as a submodule: the build has to work with
no network access, and the four patches below are part of the firmware's
bring-up behaviour (they are what turns a silent failure into a reported
stage). `PATCHES.md` records each change so the copy can be re-based onto a
newer upstream revision.

Upstream file-level licensing is preserved: the libdlo sources carry
DisplayLink's GNU Library General Public License version 2 notices, and the
repository as a whole is GPL-2.0 (`LICENSE-GPL-2.0` in this directory).

`src/CMakeLists.txt` is taken from upstream unchanged; it defines the
`tusb_libdlo` INTERFACE target, the `DLO_*` build options and the requirement
for the Pico SDK `tinyusb_host` target.
