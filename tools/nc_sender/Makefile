CC = gcc.exe
UCNC = ../..
NC = $(UCNC)/uCNC/src/modules/nc
G7X = $(UCNC)/uCNC/src/modules/g7x
BUILDDIR = build

CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -DG7X_HOST_TEST -DNC_HOST_TEST \
         -DNC_MAX_LINES=4096 -DNC_MAX_LINE_LEN=128 \
         -I$(NC) -I$(G7X) -I$(UCNC)

MODULES = $(NC)/nc.c $(NC)/nc_emit.c $(NC)/nc_g7x.c \
          $(G7X)/g7x.c $(G7X)/g7x_contour.c $(G7X)/g7x_source.c
SENDER = nc_sender.c grbl_stream.c grbl_port_win32.c nc_send_job.c

TARGET = $(BUILDDIR)/nc_send.exe
TESTBIN = $(BUILDDIR)/nc_sender_test.exe

.PHONY: all test clean

all: $(TARGET)

$(BUILDDIR):
	if not exist $(BUILDDIR) mkdir $(BUILDDIR)

$(TARGET): $(BUILDDIR) main.c $(SENDER) $(MODULES)
	$(CC) $(CFLAGS) main.c $(SENDER) $(MODULES) -o $(TARGET)

$(TESTBIN): $(BUILDDIR) tests/nc_sender_host_test.c $(SENDER) $(MODULES)
	$(CC) $(CFLAGS) -I. tests/nc_sender_host_test.c $(SENDER) $(MODULES) -o $(TESTBIN)

test: $(TESTBIN)
	$(TESTBIN)

clean:
	del /Q $(BUILDDIR)\*.exe 2>NUL
