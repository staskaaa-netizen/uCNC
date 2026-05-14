# LeanCam UI / Workflow Developer Notes

## Philosophy

LeanCam is a lightweight conversational CNC workflow system focused on:

- direct machine interaction
- deterministic workflows
- visual feedback
- low operator friction
- keypad-first operation
- transparent machine behavior

The operator works with:
- stock
- tools
- machining cycles
- live previews

instead of manually writing large amounts of G-code.

LeanCam is intended for:
- shop-floor programming
- prototype work
- manual + conversational hybrid workflows
- lightweight industrial controllers

LeanCam uses compact conversational parameter editing optimized for limited screen space.

The interface follows principles commonly found in older industrial CNC controls:
- dense information layout
- direct field editing
- deterministic navigation
- minimal screen transitions
- continuous operator context

The design intentionally favors efficiency and clarity over modern GUI complexity.

---

# Main UI Structure

## Home Screen = File Manager

The Home screen is the root workflow hub.

Available actions:

- Open `.lcam`
- Open G-code
- Create new `.lcam`
- Create new G-code
- Convert `.lcam` → G-code
- Run program
- Delete file
- Duplicate file
- Edit tool file
- Select active toolset

The Home screen combines:
- program launcher
- project manager
- conversational entry point

---

# LeanCam Program Workflow

## Create New LeanCam File

Workflow:

HOME
  ↓
NEW LCAM
  ↓
ENTER FILE NAME
  ↓
ENTER DRAFT MODE

After filename entry:
- an empty draft is created
- stock preview becomes active
- conversational editing begins immediately

---

# Draft Mode

Draft Mode is the central editing environment.

The screen continuously shows:
- current stock
- active tool
- committed geometry
- current cycle preview

The part evolves visually while programming.

---

# Step 1 — Define Stock

Stock definition is mandatory.

Current setup template:

SETUP|L{}|OD{}|ID{(0)}|CLAMP{(0)}|EXTRA{(0)}|CLR{(1)}|MAT{(ST45)}|WOFF{(G54)}

During editing:
- stock updates live
- dimensions are shown immediately
- geometry exists before first machining cycle

---

# Step 2 — Define Tool

Tool definition establishes:
- geometry constraints
- cutting orientation
- feeds and speeds
- machine offsets
- default machining behavior

The tool preview should visually show:
- insert shape
- orientation
- cutting direction
- nose radius
- active cutting edges

---

# Tool File System

LeanCam separates:
- conversational geometry
- machine tooling data

Programs reference logical tool numbers.

Real machine offsets and tooling data are supplied by the active toolset.

This allows:
- reusable programs
- machine portability
- offline editing
- multiple machine configurations

---

# Active Toolset

LeanCam supports selection of:

ACTIVE TOOLSET

Examples:

LATHE_6T
GANG_SMALL
PROTO_SETUP
TURRET_A

The active toolset supplies:
- real offsets
- installed tool geometry
- feeds and speeds
- machine-local tooling state

---

# Tool File Editing

Workflow:

HOME
  ↓
EDIT TOOL FILE

Tool editor allows:
- create tool
- duplicate tool
- delete tool
- modify offsets
- modify insert geometry
- modify feeds/speeds
- modify comments/names

---

# Recommended Tool Table

Recommended tool definition:

TOOL|T{}|NAME{}|SHAPE{}|ORI{}|NOSE_R{}|XOFF{}|ZOFF{}|WIDTH{}|SIDE{}|S{}|R_FEED{}|FIN_FEED{}|R_DOC{}|FIN_DOC{}

Example:

TOOL|T{(1)}|NAME{(OD_LEFT)}|SHAPE{(DNMG)}|ORI{(LEFT)}|NOSE_R{(0.4)}|XOFF{(12.52)}|ZOFF{(-145.20)}|WIDTH{(3)}|SIDE{(FRONT)}|S{(800)}|R_FEED{(120)}|FIN_FEED{(60)}|R_DOC{(2.0)}|FIN_DOC{(0.5)}

---

# Tool Offset Notes

Tool offsets belong to:
- active toolset
- machine tooling configuration

Offsets should NOT be stored inside every `.lcam` file.

This keeps programs:
- portable
- reusable
- independent from machine state

---

# Conversational Layout

LeanCam conversational cycles use a compact two-row table layout.

Top row:
- field labels

Bottom row:
- editable values

Fields are visually separated using pipes `|`.

Example:

OD TURN

D1      | Z1    | Z2     | D2      | RND | CHMF | DT      | CLR
50.00   | 0.00  | -50.00 | 50.00   | 0   | 0    | 50.00   | 1.00

The selected field is edited in place while:
- geometry preview updates live
- stock updates continuously
- cycle result is visualized immediately

The operator never edits raw template syntax directly.

---

# Cycle Menu System

LeanCam uses hierarchical 3×3 menus.

Reason:
- current cycle count already exceeds 13
- future expansion expected
- deterministic keypad navigation
- strong spatial memory
- avoids long scrolling menus

Workflow:

3×3 ROOT
  ↓
DIRECT CYCLE
or
CHILD CATEGORY
  ↓
FINAL CYCLE

---

# Example Menu Structure

TURN
  OD
  ID
  FACE
  TAPER

THREAD
  OD THREAD
  ID THREAD
  TAP

GROOVE
  GROOVE
  PART
  CHAMFER

---

# Cycle Insertion Workflow

After stock and tool definition:

INSERT CYCLE

Workflow:

3×3 MENU
  ↓
SELECT CYCLE
  ↓
LIVE PREVIEW
  ↓
INSERT PARAMETERS
  ↓
COMMIT

While parameters are entered:
- geometry updates continuously
- cycle result is shown immediately
- stock removal preview updates live

---

# Commit Workflow

After cycle commit:

COMMIT
  ↓
RETURN TO 3×3 ROOT

Returning to root menu after every cycle is intentional.

Advantages:
- predictable navigation
- scalable UI
- avoids modal traps
- preserves spatial workflow memory

---

# Direct Run Capability

After each committed cycle:

RUN PROGRAM

may be selected immediately.

This enables:
- incremental machining
- conversational/manual hybrid workflows
- quick testing
- prototype operation

---

# Editing Existing LeanCam Programs

Workflow:

OPEN LCAM
  ↓
EDIT MODE

Operator can:
- move up/down between cycles
- inspect geometry
- preview selected cycle
- modify parameters
- insert new cycles
- delete cycles

---

# Insert New Cycle Into Existing Program

Workflow:

INSERT
  ↓
RETURN TO 3×3 ROOT
  ↓
SELECT CYCLE
  ↓
PREVIEW
  ↓
PARAMETERS
  ↓
COMMIT

The same insertion workflow is reused everywhere.

This intentionally avoids:
- separate insert mode
- separate creation mode
- separate editing logic

Result:
- lower cognitive load
- deterministic operation
- simpler UI architecture

---

# Template Syntax

Current template syntax:

{}                  required user input
{(literal)}         default literal value
{(SETUP.FIELD)}     inherit from setup line
{(THIS.FIELD)}      inherit from current cycle

This enables:
- compact cycle definitions
- conversational defaults
- minimal typing
- self-propagating values

---

# Abbreviation List

SETUP     program setup line
TOOL      active tool definition

L         stock length
OD        outer diameter
ID        inner diameter
CLAMP     clamped stock length
EXTRA     extra stock length
CLR       clearance / safe distance
MAT       material
WOFF      work offset

T         tool number
NAME      tool name/comment
SHAPE     insert shape
ORI       tool orientation
SIDE      cutting side

XOFF      machine X offset
ZOFF      machine Z offset

NOSE_R    insert nose radius
WIDTH     groove / cutoff width

D         diameter
D1        start diameter
D2        end diameter
DT        target diameter

Z         target Z position
Z1        start Z position
Z2        end Z position

S         spindle speed
RPM       spindle speed
FEED      feed rate
R_FEED    roughing feed
FIN_FEED  finishing feed

DOC       depth of cut
R_DOC     roughing DOC
FIN_DOC   finishing DOC

RND       corner radius / rounding
CHMF      chamfer
R         radius

M         thread size
P         thread pitch
N         thread passes
ST        spring passes

DEPTH     drilling/tapping depth
PECK      peck drilling amount

---

# Workflow Priorities

LeanCam prioritizes:

## Deterministic Navigation
No hidden GUI complexity.

## Live Geometry
Everything updates visually while editing.

## Minimal Hardware
Designed for keypad-driven operation.

## Conversational CNC
Operator thinks in machining operations instead of G-code.

## Honest Engineering
Simple systems with direct behavior.

---

# LeanCam Is Not

LeanCam is not:
- full CAD
- full CAM
- touchscreen-heavy UI
- cloud platform
- dependency-heavy framework

---

# LeanCam Is

LeanCam is:
- conversational CNC layer
- deterministic editor
- lightweight industrial workflow
- direct machine interface
- shop-floor programming system

Design principle:

> “More with less.”