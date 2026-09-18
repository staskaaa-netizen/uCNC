CXX = g++.exe
CC = gcc.exe
BUILDDIR = build
TARGET = $(BUILDDIR)/LeanCamWin.exe

UCNC_ROOT = ../..
LEANCAM = $(UCNC_ROOT)/uCNC/src/modules/leanCam
G7X = $(UCNC_ROOT)/uCNC/src/modules/g7x

CXXFLAGS = -std=gnu++17 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -I$(LEANCAM)
CFLAGS = -std=gnu99 -O2 -Wall -Wextra -DG7X_HOST_TEST -I$(LEANCAM) -I$(G7X)
# -static keeps the MinGW runtime (libwinpthread) inside the exe: the tool must
# run without the toolchain's bin directory on PATH.
LDFLAGS = -mwindows -static -static-libgcc -static-libstdc++ -lcomdlg32 -lgdi32 -luser32

# The generator grew into several module files over time. Compile the module
# sources here instead of listing them one by one by hand, so a future split
# does not silently break this tool (the Makefile used to compile only the
# generator and stopped linking when leancam_code.c was extracted).
LEANCAM_SRC = $(LEANCAM)/leancam_gcode.c $(LEANCAM)/leancam_code.c \
              $(G7X)/g7x.c $(G7X)/g7x_contour.c $(G7X)/g7x_source.c

OBJS = $(BUILDDIR)/leancam_win.o $(BUILDDIR)/leancam_gcode.o \
       $(BUILDDIR)/leancam_code.o $(BUILDDIR)/host_stub.o \
       $(BUILDDIR)/g7x.o $(BUILDDIR)/g7x_contour.o $(BUILDDIR)/g7x_source.o

.PHONY: all clean run

all: $(TARGET)

$(BUILDDIR):
	mkdir $(BUILDDIR)

$(TARGET): $(BUILDDIR) $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)

$(BUILDDIR)/leancam_win.o: leancam_win.cpp | $(BUILDDIR)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

$(BUILDDIR)/leancam_gcode.o: $(LEANCAM)/leancam_gcode.c | $(BUILDDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILDDIR)/leancam_code.o: $(LEANCAM)/leancam_code.c | $(BUILDDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILDDIR)/host_stub.o: host_stub.c | $(BUILDDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILDDIR)/g7x.o: $(G7X)/g7x.c | $(BUILDDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILDDIR)/g7x_contour.o: $(G7X)/g7x_contour.c | $(BUILDDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILDDIR)/g7x_source.o: $(G7X)/g7x_source.c | $(BUILDDIR)
	$(CC) -c $< -o $@ $(CFLAGS)

run: all
	$(TARGET)

clean:
	del /Q $(BUILDDIR)\*.o $(TARGET) 2>NUL
