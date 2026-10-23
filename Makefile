# Build botter: the CLI that checks, runs, scores and builds agents (docs/cli-design.md).
#
#   make            -> ./botter     (static x86-64 executable: the CLI + packer, with the botcore runtime and
#                                    the Botter skill (skill/botter) embedded)
#   make artifacts  -> artifacts/botcore (static runtime) + artifacts/packer (packer)
#   make builder    -> artifacts/botter.bot: the old agent that builds agents (repo root: AGENT.md, FLOW.lua,
#                      functions/, skills/). Kept until the CLI's acceptance test passes (progress.md §14).
#   make test       -> tests/run.sh (stub LLM, sandbox, functions, batch mode, HTTP client; BOTTER_TEST_NET=1 adds internet tests)
#   make dist       -> dist/botter-<VERSION>-linux-x86_64.tar.gz (+ .sha256): botter, README, licences
#   make clean
#
# Build-time only. Needs gcc and musl-gcc (Fedora: musl-gcc musl-libc-static).

# also drop compiler notes (Fedora annobin adds ~15KB) that plain strip keeps
VERSION     := $(shell sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' config.json 2>/dev/null)
export VERSION
STRIP_NOTES ?= -R .comment -R .note.gnu.property -R .annobin.notes -R .gnu.build.attributes

BOTCORE_DIR = tools/src/botcore
PACK_DIR    = tools/src/packer
BUILD_PROG  = skills/build-agent/scripts/agent_tools
CJSON       = $(BOTCORE_DIR)/lib/cjson
LUA         = $(BOTCORE_DIR)/lib/lua
LUA_SRCS    = $(wildcard $(LUA)/*.c)
BOTLUA      = $(BOTCORE_DIR)/src/botlua.c $(BOTCORE_DIR)/src/botlua.h

AGENT_FILES = $(shell find AGENT.md FLOW.lua config.json skills functions -type f ! -path '$(BUILD_PROG)' 2>/dev/null)

SKILL_FILES = $(shell find skill/botter -type f 2>/dev/null | sort)
CLI_SRC     = tools/src/cli/botter.c

botter: $(CLI_SRC) $(PACK_DIR)/packer.c $(BOTLUA) $(CJSON)/cJSON.c $(LUA_SRCS) artifacts/botcore artifacts/skill.bin
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -I$(CJSON) -I$(LUA) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -DBOTTER_VERSION='"$(VERSION)"' \
		-DBOTTER_RUNTIME_FILE='"artifacts/botcore"' -DBOTTER_SKILL_FILE='"artifacts/skill.bin"' \
		-o $@ $(CLI_SRC) $(CJSON)/cJSON.c $(LUA_SRCS) -lm
	strip $(STRIP_NOTES) $@

# The skill, one file: "path\nsize\n" + bytes per file (read by botter skill / botter new).
artifacts/skill.bin: $(SKILL_FILES)
	@mkdir -p artifacts
	@cd skill && for f in $$(find botter -type f | sort); do printf '%s\n%s\n' "$$f" "$$(stat -c %s "$$f")"; cat "$$f"; done > ../$@

builder: artifacts/botter.bot

artifacts/botter.bot: artifacts/botcore artifacts/packer $(BUILD_PROG) $(AGENT_FILES)
	artifacts/packer build . artifacts/botcore $@

tools: $(BUILD_PROG)

artifacts: artifacts/botcore artifacts/packer

artifacts/botcore: $(wildcard $(BOTCORE_DIR)/src/*.c $(BOTCORE_DIR)/src/*.h) $(LUA_SRCS)
	$(MAKE) -C $(BOTCORE_DIR) release
	@mkdir -p artifacts
	cp $(BOTCORE_DIR)/botcore-static $@

artifacts/packer: $(PACK_DIR)/packer.c $(BOTLUA) $(CJSON)/cJSON.c $(LUA_SRCS)
	@mkdir -p artifacts
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -I$(CJSON) -I$(LUA) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< $(CJSON)/cJSON.c $(LUA_SRCS) -lm
	strip $(STRIP_NOTES) $@

AGENT_TOOLS = tools/src/agent_tools/agent_tools

$(AGENT_TOOLS): tools/src/agent_tools/agent_tools.c $(PACK_DIR)/packer.c $(BOTLUA) $(CJSON)/cJSON.c $(LUA_SRCS)
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -I$(CJSON) -I$(LUA) -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o $@ tools/src/agent_tools/agent_tools.c $(CJSON)/cJSON.c $(LUA_SRCS) -lm
	strip $(STRIP_NOTES) $@

$(BUILD_PROG): $(AGENT_TOOLS)
	@mkdir -p $(dir $@)
	cp $< $@

test: botter artifacts/botter.bot
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
	rm -rf artifacts botter $(BUILD_PROG) $(AGENT_TOOLS) dist
	$(MAKE) -C $(BOTCORE_DIR) clean

.PHONY: tools artifacts builder test dist clean
