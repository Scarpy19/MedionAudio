CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra
FRAMEWORKS = -framework IOKit -framework CoreFoundation

.PHONY: all clean

all: medion_ctl

medion_ctl: medion_ctl.c
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

clean:
	rm -f medion_ctl
