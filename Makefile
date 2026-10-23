# Build botter: the CLI that profiles data, checks, runs, scores, seals and builds agents (docs/cli-design.md).
#
#   make            -> ./botter     (static x86-64 executable: the CLI + packer, with the botcore runtime and
#                                    the Botter skill (skill/botter) embedded)
#   make artifacts  -> artifacts/botcore (static runtime) + artifacts/packer (packer, used by the tests)
#   make test       -> tests/run.sh (stub LLM, sandbox, functions, batch mode, CLI, HTTP client;
#                      BOTTER_TEST_NET=1 adds internet tests)
#   make dist       -> dist/botter-<VERSION>-linux-x86_64.tar.gz (+ .sha256): botter, README, licences
#   make clean
#
# Build-time only. Needs gcc and musl-gcc (Fedora: musl-gcc musl-libc-static). The version is in VERSION.

VERSION     := $(shell cat VERSION)
export VERSION
# also drop compiler notes (Fedora annobin adds ~15KB) that plain strip keeps
STRIP_NOTES ?= -R .comment -R .note.gnu.property -R .annobin.notes -R .gnu.build.attributes

BOTCORE_DIR = tools/src/botcore
PACK_DIR    = tools/src/packer
CJSON       = $(BOTCORE_DIR)/lib/cjson
LUA         = $(BOTCORE_DIR)/lib/lua
LUA_SRCS    = $(wildcard $(LUA)/*.c)
BOTLUA      = $(BOTCORE_DIR)/src/botlua.c $(BOTCORE_DIR)/src/botlua.h
SKILL_FILES = $(shell find skill/botter -type f 2>/dev/null | sort)
CLI_SRC     = tools/src/cli/botter.c

# Written to a temporary name, then renamed: a botter that is running keeps working while it is rebuilt.
botter: $(CLI_SRC) $(PACK_DIR)/packer.c $(BOTLUA) $(CJSON)/cJSON.c $(LUA_SRCS) artifacts/botcore artifacts/skill.bin VERSION
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -I$(CJSON) -I$(LUA) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -DBOTTER_VERSION='"$(VERSION)"' \
		-DBOTTER_RUNTIME_FILE='"artifacts/botcore"' -DBOTTER_SKILL_FILE='"artifacts/skill.bin"' \
		-o $@.tmp $(CLI_SRC) $(CJSON)/cJSON.c $(LUA_SRCS) -lm
	strip $(STRIP_NOTES) $@.tmp
	mv -f $@.tmp $@

# The skill, one file: "path\nsize\n" + bytes per file (read by botter skill / botter new).
artifacts/skill.bin: $(SKILL_FILES)
	@mkdir -p artifacts
	@cd skill && for f in $$(find botter -type f | sort); do printf '%s\n%s\n' "$$f" "$$(stat -c %s "$$f")"; cat "$$f"; done > ../$@

artifacts: artifacts/botcore artifacts/packer

artifacts/botcore: $(wildcard $(BOTCORE_DIR)/src/*.c $(BOTCORE_DIR)/src/*.h) $(LUA_SRCS) VERSION
	$(MAKE) -C $(BOTCORE_DIR) release
	@mkdir -p artifacts
	cp $(BOTCORE_DIR)/botcore-static $@

artifacts/packer: $(PACK_DIR)/packer.c $(BOTLUA) $(CJSON)/cJSON.c $(LUA_SRCS)
	@mkdir -p artifacts
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -I$(CJSON) -I$(LUA) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< $(CJSON)/cJSON.c $(LUA_SRCS) -lm
	strip $(STRIP_NOTES) $@

test: botter artifacts/packer
	sh tests/run.sh

DIST = dist/botter-$(VERSION)-linux-x86_64

dist: botter
	rm -rf $(DIST) $(DIST).tar.gz
	mkdir -p $(DIST)
	cp botter README.md LICENSE THIRD_PARTY.md $(DIST)/
	cp tools/src/botcore/lib/bearssl/LICENSE.txt $(DIST)/LICENSE.BearSSL.txt
	cp tools/src/botcore/lib/lua/LICENSE $(DIST)/LICENSE.Lua.txt
	tar -C dist -czf $(DIST).tar.gz botter-$(VERSION)-linux-x86_64
	cd dist && sha256sum botter-$(VERSION)-linux-x86_64.tar.gz > botter-$(VERSION)-linux-x86_64.tar.gz.sha256
	@echo "dist: $(DIST).tar.gz"

clean:
	rm -rf artifacts botter botter.tmp dist
	$(MAKE) -C $(BOTCORE_DIR) clean

.PHONY: artifacts test dist clean
