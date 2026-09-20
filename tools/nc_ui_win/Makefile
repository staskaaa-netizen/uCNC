CC = gcc.exe
UCNC = ../..
SRC = $(UCNC)/uCNC/src
BUILDDIR = build

CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -DPIO_UNIT_TESTING -DUCNC_IGNORE_BOARDMAP_OVERRIDES \
         -DUCNC_IGNORE_HAL_OVERRIDES -DENABLE_PARSER_MODULES \
         -DENABLE_MAIN_LOOP_MODULES -DENABLE_O_CODES -DENABLE_G7X_MODULE \
         -DG33_ENCODER=0 -DDISABLE_SAFE_SETTINGS -DDISABLE_ENDPROGRAM_LOCK \
         -DEMULATE_GRBL_STARTUP=3 -DLVDS_WIDTH=800 -DLVDS_HEIGHT=600 \
         -include host_boardmap.h \
         -I$(UCNC) -I$(UCNC)/uCNC -I$(SRC) -I$(SRC)/modules -I$(SRC)/modules/nc -I$(SRC)/modules/g7x \
         -I$(SRC)/modules/lvds_renderer -I.

# -static keeps the MinGW runtime (libwinpthread) inside the exe so it runs
# without the toolchain's bin directory on PATH.
LDFLAGS = -mwindows -static -lcomdlg32 -lgdi32 -luser32 -lcomctl32

# $(wildcard) matters: mingw32-make runs recipes through cmd.exe, which does not
# expand globs itself.
CORE = $(wildcard $(SRC)/*.c) $(wildcard $(SRC)/core/*.c) \
       $(wildcard $(SRC)/interface/*.c) $(wildcard $(SRC)/hal/kinematics/*.c) \
       $(wildcard $(SRC)/hal/tools/*.c) $(wildcard $(SRC)/hal/tools/tools/*.c) \
       $(wildcard $(SRC)/modules/*.c) \
       $(SRC)/hal/mcus/mcu.c $(SRC)/hal/mcus/virtual/mcu_virtual.c \
       $(SRC)/hal/mcus/virtual/virtual_windows.c

MODULES = $(SRC)/modules/g7_g8/parser_g7_g8.c \
          $(SRC)/modules/cam_keyboard/cam_keyboard.c \
          $(SRC)/modules/g7x/g7x.c $(SRC)/modules/g7x/g7x_contour.c \
          $(SRC)/modules/g7x/g7x_source.c \
          $(SRC)/modules/nc/nc.c $(SRC)/modules/nc/nc_emit.c \
          $(SRC)/modules/nc/nc_g7x.c $(SRC)/modules/nc/nc_files.c \
          $(SRC)/modules/nc/nc_feedback.c $(SRC)/modules/nc/nc_manual.c \
          $(SRC)/modules/nc/nc_menu.c \
          $(SRC)/modules/nc/nc_palette.c $(SRC)/modules/nc/nc_presets.c \
          $(SRC)/modules/nc/nc_preview.c $(SRC)/modules/nc/nc_draw.c \
          $(SRC)/modules/nc/nc_state.c $(SRC)/modules/nc/nc_text.c \
          $(SRC)/modules/nc/nc_tools.c $(SRC)/modules/nc/nc_vocab.c \
          $(SRC)/modules/nc/nc_visual.c \
          $(SRC)/modules/lvds_renderer/lvds_draw_api.c \
          $(SRC)/modules/lvds_renderer/lvds_palette.c \
          $(SRC)/modules/lvds_renderer/lvds_font_cond_6x8.c \
          $(SRC)/modules/lvds_renderer/lvds_font_ibm_8x14.c

APP = lvds_host.c host_fs.c host_shim.c main.c

TARGET = $(BUILDDIR)/nc_ui.exe

.PHONY: all clean

all: $(TARGET)

$(BUILDDIR):
	if not exist $(BUILDDIR) mkdir $(BUILDDIR)

$(TARGET): $(BUILDDIR) $(APP) $(MODULES)
	$(CC) $(CFLAGS) $(APP) $(CORE) $(MODULES) -o $(TARGET) $(LDFLAGS)

clean:
	del /Q $(BUILDDIR)\*.exe 2>NUL
