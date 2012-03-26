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

CFLAGS    += -pipe -fPIC -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 \
  -I$(LUA_DIR)/src \
  -I$(UV_DIR)/include \
  -I$(HTTP_DIR)

#CFLAGS    += -O2

LDFLAGS   += 

all: deps luv.luvit

DEPS  := \
  bin/luajit \
  $(UV_DIR)/uv.a \
  $(HTTP_DIR)/http_parser.o

deps: $(DEPS)

UVO := $(addprefix build/libuv/,src/unix/core.o src/unix/dl.o src/unix/fs.o src/unix/cares.o src/unix/udp.o src/unix/error.o src/unix/thread.o src/unix/process.o src/unix/tcp.o src/unix/pipe.o src/unix/tty.o src/unix/stream.o src/unix/linux/core.o src/unix/linux/inotify.o src/uv-common.o src/unix/uv-eio.o src/unix/ev/ev.o src/unix/eio/eio.o  src/ares/ares__close_sockets.o src/ares/ares__get_hostent.o src/ares/ares__read_line.o src/ares/ares__timeval.o src/ares/ares_cancel.o src/ares/ares_data.o src/ares/ares_destroy.o src/ares/ares_expand_name.o src/ares/ares_expand_string.o src/ares/ares_fds.o src/ares/ares_free_hostent.o src/ares/ares_free_string.o src/ares/ares_gethostbyaddr.o src/ares/ares_gethostbyname.o src/ares/ares_getnameinfo.o src/ares/ares_getopt.o src/ares/ares_getsock.o src/ares/ares_init.o src/ares/ares_library_init.o src/ares/ares_llist.o src/ares/ares_mkquery.o src/ares/ares_nowarn.o src/ares/ares_options.o src/ares/ares_parse_a_reply.o src/ares/ares_parse_aaaa_reply.o src/ares/ares_parse_mx_reply.o src/ares/ares_parse_ns_reply.o src/ares/ares_parse_ptr_reply.o src/ares/ares_parse_srv_reply.o src/ares/ares_parse_txt_reply.o src/ares/ares_process.o src/ares/ares_query.o src/ares/ares_search.o src/ares/ares_send.o src/ares/ares_strcasecmp.o src/ares/ares_strdup.o src/ares/ares_strerror.o src/ares/ares_timeout.o src/ares/ares_version.o src/ares/ares_writev.o src/ares/bitncmp.o src/ares/inet_net_pton.o src/ares/inet_ntop.o)

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
	$(MAKE) -j 8 -C $^

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
	$(MAKE) -j 8 -C $^ uv.a

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
	$(MAKE) -j 8 -C $^ http_parser.o

$(HTTP_DIR):
	mkdir -p build
	$(GET) https://github.com/joyent/http-parser/tarball/master | tar -xzpf - -C build
	mv build/joyent-http-parser* $@

luv.luvit: src/luv.c src/uhttp.c $(UVO) #$(UV_DIR)/uv.a
	echo $(UVO)
	$(CC) $(CFLAGS) -shared -o $@ $^
	cp $@ luv.so

ifeq ($(FALSE),TRUE)
luv.so: src/luv.c src/uhttp.c
	$(CC) $(CFLAGS) -shared -o $@ $^
	# $(UVDIR)/uv.a $(HTTPDIR)/http_parser.o

luv: src/test.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	#nemiver ./luv
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	#chpst -o 2048 ./luv

fs: src/fs.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt
	#nemiver ./fs
	#valgrind --leak-check=full --show-reachable=yes -v ./luv
	./fs

luh: src/luh.c src/luv.c src/uhttp.c $(LIBS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread -lm -lrt -ldl

profile: luv
	chpst -o 2048 valgrind --tool=callgrind --dump-instr=yes --simulate-cache=yes --collect-jumps=yes ./luv

luv.h: $(HTTPDIR)/http_parser.h $(UVDIR)/include/uv.h src/luv.h
	cat $^ | $(CC) -E $(CFLAGS) - | sed '/^#/d;/^$$/d' >$@
endif

.PHONY: all deps profile clean
#.SILENT:
