DEPS    := ../luvit/deps
LUADIR  := $(DEPS)/luajit/src
UVDIR   := $(DEPS)/uv
HTTPDIR := $(DEPS)/http-parser

INCS    := -Isrc/ -I$(LUADIR)/ -I$(UVDIR)/include -I$(HTTPDIR)/
LIBS    := $(LUADIR)/libluajit.a $(UVDIR)/uv.a $(HTTPDIR)/http_parser.o

CFLAGS  += $(INCS)
LDFLAGS +=

all: luv

luv: src/luv.c src/alloc.c $(LIBS)
	$(CC) -pipe -g $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	#gdb ./luv
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	chpst -o 2048 ./luv

profile: luv
	chpst -o 2048 valgrind --tool=callgrind --dump-instr=yes --simulate-cache=yes --collect-jumps=yes ./luv

.PHONY: all profile clean
#.SILENT:
