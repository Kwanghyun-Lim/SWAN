COMMIT_REV ?= $(shell git describe  --always --abbrev=12)
CFLAGS += -I.. -I. -I../driver/ -DCOMMIT_REV="\"$(COMMIT_REV)\"" -g
CFLAGS +=  -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64  -D_GNU_SOURCE
PROGRAMS += dmsrc_create
INSTALL_DIR = $(DESTDIR)/sbin/

.PHONY:all
all: $(PROGRAMS)

#get_agsize: get_agsize.o
#	$(CC) $^ -o $@

#-include get_agsize.d

dmsrc_create: dmsrc_create.o
	$(CC) $^ -o $@

-include dmsrc_create.d

#flashcache_destroy: flashcache_destroy.o
#	$(CC) $^ -o $@

#-include flashcache_destroy.d

#flashcache_load: flashcache_load.o
#	$(CC) $^ -o $@

#-include flashcache_load.d

#flashcache_setioctl: flashcache_setioctl.o
#	$(CC) $^ -o $@

#-include flashcache_setioctl.d

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	@$(CC) -MM $(CFLAGS) -MF $*.d -MT $*.o $*.c

.PHONY: install
install: $(PROGRAMS)
	install -d -m 755 $(INSTALL_DIR)
	install -m 755 $(PROGRAMS) $(INSTALL_DIR)

.PHONY: clean
clean:
	rm -f *.[od] $(PROGRAMS)

