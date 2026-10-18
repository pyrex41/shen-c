# Default toolchain has no Boehm or pkg-config dependency.
include Makefile.rc
.DEFAULT_GOAL := all

all: $(RC_TOOL_BIN)/shen-c

# Preserve the conventional executable name for existing command invocations.
$(RC_TOOL_BIN)/shen-c: $(RC_TOOL_BIN)/shen-rc
	cp $< $@

rc: all
rc-test: test
rc-sanitize: sanitize
rc-counters: counters
rc-acyclic: acyclic

certify:
	scripts/certify-rc.sh

.PHONY: rc rc-test rc-sanitize rc-counters rc-acyclic certify
