# LeanCam File Format (`.lcam`)

## 1. Overview

`.lcam` is a simple text-based format used by LeanCam to describe conversational lathe programs.

It is designed to be:

* human-readable
* easy to edit manually
* easy to parse in plain C
* portable between Windows and embedded systems

Each line represents one logical block of the program.

---

## 2. Basic Structure

A file consists of multiple lines:

```
SETUP|...
TOOL|...
CUT|...
ODTURN|...
FACE|...
```

Each line is processed in order from top to bottom.

---

## 3. Line Format

General form:

```
MODULE|KEY{VALUE}|KEY{VALUE}|KEY{VALUE}
```

### Components

* `MODULE` — defines what this line represents
* `KEY` — field name
* `VALUE` — field content inside `{}`

Example:

```
ODTURN|START_DIAMETER{50}|END_DIAMETER{42}|END_Z{-100}
```

---

## 4. Modules

Typical modules:

* `SETUP` — defines stock and global context
* `TOOL` — defines active tool
* `CUT` — defines feeds, speeds, DOC
* `ODTURN` — external turning cycle
* `FACE` — facing cycle
* `DRILL`, `GROOVE`, `IDBASIC`, etc.

Each module has a fixed set of expected fields.

---

## 5. Field Types

### 5.1 Required Fields

```
END_Z{*}
```

* must be filled before the cycle can be committed
* `*` means “missing value”

---

### 5.2 Default Values

```
FILLET_RADIUS{(0)}
MATERIAL{(ST45)}
```

* value in parentheses is the default
* used if operator does not change it

---

### 5.3 References

```
START_DIAMETER{(SETUP.OUTER_DIAMETER)}
```

* references another field
* resolved automatically when the cycle is created

After resolution:

```
START_DIAMETER{50}
```

---

### 5.4 Numeric Values

```
END_DIAMETER{42}
FEED{0.25}
END_Z{-100}
```

Supports:

* integers
* decimals
* negative values

---

## 6. Execution Model

### 6.1 Order Matters

The program runs line by line:

```
SETUP → TOOL → CUT → CYCLE → CYCLE → ...
```

---

### 6.2 Active Context

At any point:

* active setup = last `SETUP`
* active tool = last `TOOL`
* active cut = last `CUT`

Cycles use the current context automatically.

---

### 6.3 Inheritance

Cycles inherit values instead of asking for everything.

Example:

```
SETUP defines material and stock
TOOL/CUT define feeds and speeds
ODTURN only asks for geometry
```

---

## 7. Draft Mode (Editing Model)

LeanCam does not insert lines directly.

Instead:

1. A new line is created as a **draft**
2. Required fields are filled
3. Optional fields may be skipped
4. Only after confirmation is the line added to program

This ensures:

* no incomplete cycles
* predictable behavior
* fast operator workflow

---

## 8. Example Program

```
SETUP|LENGTH{120}|OUTER_DIAMETER{50}|INNER_DIAMETER{0}|CLAMP_LENGTH{0}|EXTRA_LENGTH{0}|TOOL_CLEARANCE{1}|MATERIAL{ST45}|UNITS{mm}|WORK_OFFSET{G54}

TOOL|TOOL_NUMBER{1}

CUT|ROUGH_CSS{180}|ROUGH_FEED{0.25}|FINISH_CSS{220}|FINISH_FEED{0.12}|ROUGH_DEPTH_OF_CUT{2.0}|FINISH_DEPTH_OF_CUT{0.5}

ODTURN|START_DIAMETER{50}|END_DIAMETER{42}|END_Z{-100}|START_Z{0}|FILLET_RADIUS{0}|TOOL_CLEARANCE{1}

FACE|DIAMETER{42}|Z{0}|DEPTH_OF_CUT{1.0}|TOOL_CLEARANCE{1}
```

---

## 9. UI Asset Mapping

LeanCam derives UI graphics automatically from module name.

Example:

```
ODTURN → odturn.bmp   (helper image)
ODTURN → odturn.ico   (button icon)
```

Rules:

* module name is converted to lowercase
* no metadata is stored in the file
* missing files are ignored

---

## 10. Editing Rules

* file can be edited manually
* order must be preserved
* unknown fields are ignored
* missing values fall back to defaults

---

## 11. Limitations (Current)

* no nested structures
* no expressions beyond simple references
* no multi-line blocks
* no complex geometry definitions yet

---

## 12. Philosophy

`.lcam` is intentionally simple.

It is not:

* G-code
* JSON
* XML

It is:

→ a **linear conversational description of machining intent**

---

## 13. Summary

The `.lcam` format provides:

* minimal input requirements
* predictable behavior
* easy parsing
* direct mapping to shop-floor workflow

It is designed to grow gradually without breaking simplicity.
