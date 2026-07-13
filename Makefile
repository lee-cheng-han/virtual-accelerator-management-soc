SHELL := /bin/sh

.DEFAULT_GOAL := help

PHASE0_DOCS := README.md \
	docs/requirements.md docs/architecture.md docs/roadmap.md \
	docs/register-map.md docs/host-firmware-abi.md \
	docs/descriptor-format.md docs/firmware-architecture.md \
	docs/command-lifecycle.md docs/fault-recovery.md \
	docs/verification-plan.md docs/performance-plan.md docs/demo.md

.PHONY: help check check-docs tree clean demo

help:
	@printf '%s\n' \
	  'Virtual Accelerator Management SoC — Phase 0' \
	  '' \
	  '  make check       Validate the Phase 0 specification' \
	  '  make tree        Print the repository tree' \
	  '  make demo        Explain demo availability' \
	  '  make clean       Remove generated output'

check: check-docs

check-docs:
	@set -eu; \
	for file in $(PHASE0_DOCS); do \
		test -s "$$file" || { echo "missing or empty: $$file" >&2; exit 1; }; \
	done; \
	if LC_ALL=C grep -RIn '[[:blank:]]$$' README.md docs; then \
		echo 'trailing whitespace found' >&2; exit 1; \
	fi; \
	grep -q 'Phase 0.*Complete' README.md; \
	grep -q 'sizeof(struct vams_submission) == 64' docs/descriptor-format.md; \
	grep -q 'sizeof(struct vams_completion) == 32' docs/descriptor-format.md; \
	echo 'Phase 0 documentation checks: PASS'

tree:
	@find . -path './.git' -prune -o -path './build' -prune -o -print | sort

demo:
	@echo 'make demo becomes functional in Phase 10; see docs/demo.md'

clean:
	rm -rf build out test-results coverage

