PLUGIN    := mcp_plugin
SRCDIR    := src
BUILDDIR  := build
PKG_CLAWS := claws-mail
PKGS      := glib-2.0 gio-2.0 gobject-2.0 gtk+-3.0

PKG_CONFIG ?= pkg-config

CLAWS_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKG_CLAWS) $(PKGS))
PKG_LIBS     := $(shell $(PKG_CONFIG) --libs $(PKGS))
PLUGINDIR    ?= $(shell $(PKG_CONFIG) --variable=plugindir $(PKG_CLAWS))

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter -fPIC -D_CRT_RAND_S $(CLAWS_CFLAGS)
LDFLAGS += -shared

ifeq ($(OS),Windows_NT)
  SHLIB_EXT := dll
  CLAWS_LIBDIR ?= /mingw64/lib
  PKG_LIBS += -L$(CLAWS_LIBDIR) -lclaws-mail
  LDFLAGS  += -Wl,--enable-auto-import
else
  SHLIB_EXT := so
endif

TARGET  := $(BUILDDIR)/$(PLUGIN).$(SHLIB_EXT)
SOURCES := $(SRCDIR)/plugin.c $(SRCDIR)/mcp_server.c $(SRCDIR)/mcp_http.c \
           $(SRCDIR)/mcp_json.c $(SRCDIR)/mcp_tools.c
OBJECTS := $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

TEST_BIN    := $(BUILDDIR)/test_json
TEST_CFLAGS := -std=gnu99 -Wall -Wextra -g $(shell $(PKG_CONFIG) --cflags glib-2.0)
TEST_LIBS   := $(shell $(PKG_CONFIG) --libs glib-2.0)

CPPCHECK   ?= cppcheck
CLANG_TIDY ?= clang-tidy

.PHONY: all clean install uninstall test analyze cppcheck tidy

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(PKG_LIBS) -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(PLUGINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGINDIR)/

uninstall:
	rm -f $(DESTDIR)$(PLUGINDIR)/$(PLUGIN).$(SHLIB_EXT)

test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/test_json.c $(SRCDIR)/mcp_json.c | $(BUILDDIR)
	$(CC) $(TEST_CFLAGS) -I$(SRCDIR) $^ $(TEST_LIBS) -o $@

analyze: cppcheck tidy

cppcheck:
	$(CPPCHECK) --enable=warning,performance,portability --std=c99 \
	  --inline-suppr --suppress=missingIncludeSystem --suppress=missingInclude \
	  -DG_GSIZE_FORMAT='"zu"' -DG_GINT64_FORMAT='"lld"' \
	  --error-exitcode=1 -q -I$(SRCDIR) $(SRCDIR)

tidy:
	$(CLANG_TIDY) $(SOURCES) -- $(CFLAGS)

clean:
	rm -rf $(BUILDDIR)
