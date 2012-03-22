DEPS    := ../luvit/deps
LUADIR  := $(DEPS)/luajit/src
UVDIR   := $(DEPS)/uv
HTTPDIR := $(DEPS)/http-parser

INCS    := -Isrc/ -I$(LUADIR)/ -I$(UVDIR)/include -I$(HTTPDIR)/
LIBS    := $(LUADIR)/libluajit.a $(UVDIR)/uv.a $(HTTPDIR)/http_parser.o

CFLAGS  += $(INCS)
LDFLAGS +=

all: luv

luv: src/luv.c $(LIBS)
	$(CC) -pipe -g -O2 $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	./luv
