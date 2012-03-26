DEPS    := ../luvit/deps
LUADIR  := $(DEPS)/luajit/src
UVDIR   := $(DEPS)/uv
HTTPDIR := $(DEPS)/http-parser

INCS    := -Isrc/ -I$(LUADIR)/ -I$(UVDIR)/include -I$(HTTPDIR)/
LIBS    := $(LUADIR)/libluajit.a $(UVDIR)/uv.a $(HTTPDIR)/http_parser.o

CFLAGS  += -DGNU_SOURCE -g -O2 -pipe -fPIC $(INCS)
LDFLAGS +=

all: fs #luv luh luv.luvit luv.so

#luv.so: src/luv.c src/alloc.c
#	$(CC) $(CFLAGS) -shared -o $@ $^ $(HTTPDIR)/http_parser.o $(LDFLAGS)

luv.luvit: src/luv.c src/uhttp.c
	$(CC) $(CFLAGS) -shared -o $@ $^

luv.so: src/luv.c src/uhttp.c
	$(CC) $(CFLAGS) -shared -o $@ $^
	# $(UVDIR)/uv.a $(HTTPDIR)/http_parser.o

luv: src/test.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) -O2 -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	#nemiver ./luv
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	#chpst -o 2048 ./luv

fs: src/fs.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	nemiver ./fs
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	#./fs

luh: src/luh.c src/luv.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt -ldl

profile: luv
	chpst -o 2048 valgrind --tool=callgrind --dump-instr=yes --simulate-cache=yes --collect-jumps=yes ./luv

luv.h: $(HTTPDIR)/http_parser.h $(UVDIR)/include/uv.h src/luv.h
	cat $^ | $(CC) -E $(CFLAGS) - | sed '/^#/d;/^$$/d' >$@

.PHONY: all profile clean
#.SILENT:
