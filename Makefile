CFLAGS=-g -Wall -Werror -DVERSION=0.1 
LDFLAGS=-ldevmapper
scubed:

.PHONY: clean

clean:
	rm -f scubed

install: scubed
	install ./scubed $(DESTDIR)/sbin
