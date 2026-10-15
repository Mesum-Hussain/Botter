# Build the botter agent (this project is itself an agent project: agent.md, config.json, skills/, tools/).
#
#   make            -> ./botter     (static x86-64 executable: the TUI front end with the agent
#                                    artifacts/botter.bot = botcore + embedded pack, appended)
#   make tools      -> tools/bin/*  (static musl executables from tools/src/agent_tools)
#   make artifacts  -> artifacts/botcore (static runtime) + artifacts/packer (packer)
#   make tui        -> tui/botter-tui (botter-only full-screen UI; agents built by botter never get it)
#   make test       -> tests/run.sh (stub LLM, sandbox, functions, batch mode, HTTP client; BOTTER_TEST_NET=1 adds internet tests)
#   make dist       -> dist/botter-<VERSION>-linux-x86_64.tar.gz (+ .sha256): botter, README, licences
#   make clean
#
# Build-time only; none of this is embedded. Needs gcc and musl-gcc (Fedora: musl-gcc musl-libc-static).

# also drop compiler notes (Fedora annobin adds ~15KB) that plain strip keeps
VERSION     := $(shell sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' config.json 2>/dev/null)
export VERSION
STRIP_NOTES ?= -R .comment -R .note.gnu.property -R .annobin.notes -R .gnu.build.attributes

BOTCORE_DIR = tools/src/botcore
PACK_DIR    = tools/src/packer
TOOL_NAMES  = agent_build agent_inspect tool_test
TOOL_BINS   = $(addprefix tools/bin/,$(TOOL_NAMES))
CJSON       = $(BOTCORE_DIR)/lib/cjson
LUA         = $(BOTCORE_DIR)/lib/lua
LUA_SRCS    = $(wildcard $(LUA)/*.c)
BOTLUA      = $(BOTCORE_DIR)/src/botlua.c $(BOTCORE_DIR)/src/botlua.h

AGENT_FILES = $(shell find agent.md config.json skills functions tools/doc -type f 2>/dev/null)

# The TUI runs the agent from a memfd, so the agent's own executable (which agent_build
# uses as the runtime for new agents) is plain botcore + pack, without the TUI.
botter: tui/botter-tui artifacts/botter.bot
	tui/botter-tui --bundle artifacts/botter.bot $@

artifacts/botter.bot: artifacts/botcore artifacts/packer $(TOOL_BINS) $(AGENT_FILES)
	artifacts/packer build . artifacts/botcore $@

tui: tui/botter-tui

tui/botter-tui: $(wildcard tui/*.c tui/*.h) $(CJSON)/cJSON.c
	$(MAKE) -C tui

tools: $(TOOL_BINS)

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

# One executable, three tool names (the tool runner starts it under its tool name).
$(TOOL_BINS): $(AGENT_TOOLS)
	@mkdir -p tools/bin
	cp $< $@

test: botter
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
	rm -rf artifacts botter tools/bin $(AGENT_TOOLS) dist
	$(MAKE) -C tui clean
	$(MAKE) -C $(BOTCORE_DIR) clean

.PHONY: tools artifacts tui test dist clean
