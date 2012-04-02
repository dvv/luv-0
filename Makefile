LUA_VERSION     := 2.0.0-beta9

####

PATH := .:$(PATH)

GET := $(shell which curl && echo ' -L --progress-bar')
ifeq ($(GET),)
GET := $(shell which wget && echo ' -q --progress=bar --no-check-certificate -O -')
endif
ifeq ($(GET),)
GET := curl-or-wget-is-missing
endif

####

LUA_DIR   := build/LuaJIT-$(LUA_VERSION)
UV_DIR    := build/libuv
HTTP_DIR  := build/http-parser
XS_DIR    := build/libxs-$(XS_VERSION)
STUD_DIR  := build/stud
HAPROXY_DIR := build/haproxy

####

CFLAGS    += -g -pipe -fPIC -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
LDFLAGS   += 

INCS      := -I$(LUA_DIR)/src -I$(UV_DIR)/include -I$(HTTP_DIR)
#LIBS      := $(LUA_DIR)/src/libluajit.a $(UV_DIR)/uv.a $(HTTP_DIR)/http_parser.o
LIBS      := $(UV_DIR)/uv.a $(HTTP_DIR)/http_parser.o

all: deps luv.so luv

DEPS  := \
  bin/luajit \
  $(UV_DIR)/uv.a \
  $(HTTP_DIR)/http_parser.o

deps: $(DEPS)

#####################
#
# Lua, LuaJIT
#
#####################

bin/luajit: $(LUA_DIR)/src/luajit
	mkdir -p bin
	cp $^ $@
	strip -s $@

$(LUA_DIR)/src/luajit: $(LUA_DIR)
	$(MAKE) CFLAGS='$(CFLAGS)' -j 8 -C $^

$(LUA_DIR):
	mkdir -p build
	$(GET) http://luajit.org/download/LuaJIT-$(LUA_VERSION).tar.gz | tar -xzpf - -C build
	touch -c $(LUADIR)/src/*.h

#####################
#
# libuv
#
#####################

$(UV_DIR)/uv.a: $(UV_DIR)
	$(MAKE) CFLAGS='$(CFLAGS)' -j 8 -C $^ uv.a

$(UV_DIR):
	mkdir -p build
	$(GET) https://github.com/joyent/libuv/tarball/master | tar -xzpf - -C build
	mv build/joyent-libuv* $@

#####################
#
# http-parser
#
#####################

$(HTTP_DIR)/http_parser.o: $(HTTP_DIR)
	$(MAKE) CFLAGS='$(CFLAGS)' -j 8 -C $^ http_parser.o

$(HTTP_DIR):
	mkdir -p build
	$(GET) https://github.com/joyent/http-parser/tarball/master | tar -xzpf - -C build
	mv build/joyent-http-parser* $@

#####################
#
# luv
#
#####################

luv.so: src/luv.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) $(INCS) -shared -o $@ $^ -lpthread -lm -lrt
	#cp $@ luv.luvit

luv: src/test.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	#nemiver ./luv
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	#chpst -o 2048 ./luv

ifeq ($(FALSE),TRUE)
fs: src/fs.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	#nemiver ./fs
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	./fs

luh: src/luh.c src/luv.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt -ldl

luv.h: $(HTTPDIR)/http_parser.h $(UVDIR)/include/uv.h src/luv.h
	cat $^ | $(CC) -E $(CFLAGS) - | sed '/^#/d;/^$$/d' >$@
endif

profile: luv
	chpst -o 2048 valgrind --tool=callgrind --dump-instr=yes --simulate-cache=yes --collect-jumps=yes ./luv

profile-mem: luv
	chpst -o 2048 valgrind --leak-check=full --show-reachable=yes -v ./luv

clean:
	rm -fr build bin luv luv.so

.PHONY: all deps profile profile-mem clean
#.SILENT:
