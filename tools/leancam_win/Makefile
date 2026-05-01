CXX = g++.exe
CC = gcc.exe
BUILDDIR = build
TARGET = $(BUILDDIR)/LeanCamWin.exe

UCNC_ROOT = ../..
LEANCAM = $(UCNC_ROOT)/uCNC/src/modules/leanCam

CXXFLAGS = -std=gnu++17 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -I$(LEANCAM)
CFLAGS = -std=c99 -O2 -Wall -Wextra -I$(LEANCAM)
LDFLAGS = -mwindows -static-libgcc -static-libstdc++ -lcomdlg32 -lgdi32 -luser32

OBJS = $(BUILDDIR)/leancam_win.o $(BUILDDIR)/leancam_gcode.o

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

run: all
	$(TARGET)

clean:
	del /Q $(BUILDDIR)\*.o $(TARGET) 2>NUL
