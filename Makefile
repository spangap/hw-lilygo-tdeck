# Consumer release-ceremony targets. The build itself runs through idf.py;
# this Makefile is for operations that should NOT run on every build —
# release-time only, so per-build images stay reproducible.

.PHONY: timezones

# Regenerate data/factory_state/storage/external/s.time.zones.json from the
# IANA→POSIX mapping (network fetch). The script is platform-owned (lives in
# diptych-core); the resulting file is consumer-owned and checked in with the
# release. ETag cache goes to build/ so `idf.py fullclean` forces re-download.
ZONES_SCRIPT := $(firstword $(wildcard \
    ../diptych/diptych-core/scripts/update-zones.py \
    managed_components/diptych__diptych-core/scripts/update-zones.py))

timezones:
	@test -n "$(ZONES_SCRIPT)" || { echo "update-zones.py not found (looked in diptych-core path: override and managed_components/)"; exit 1; }
	python3 $(ZONES_SCRIPT) data --cache-dir build

# --- Test harness (Python Reticulum + LXMF + pytest) -------------------------
# `make harness` clones the upstream Python references into research/ and
# editable-installs them into a project-local venv. `make harness-clean` wipes
# research/ so the next `make harness` re-downloads from scratch. Nothing
# committed; see docs/plans/test-harness.md.

.PHONY: harness harness-clean rns-reset

HARNESS_DIR     := research
HARNESS_VENV    := $(HARNESS_DIR)/.venv
HARNESS_PY      := $(HARNESS_VENV)/bin/python
HARNESS_PIP     := $(HARNESS_VENV)/bin/pip
RETICULUM_REPO  := https://github.com/markqvist/Reticulum
LXMF_REPO       := https://github.com/markqvist/LXMF
RNS_DIR         := .rns

harness: $(HARNESS_DIR)/Reticulum/.git $(HARNESS_DIR)/LXMF/.git $(HARNESS_VENV)/.installed
	@echo "harness ready"
	@cat $(HARNESS_DIR)/INSTALLED.txt

$(HARNESS_DIR)/Reticulum/.git:
	git clone $(RETICULUM_REPO) $(HARNESS_DIR)/Reticulum

$(HARNESS_DIR)/LXMF/.git:
	git clone $(LXMF_REPO) $(HARNESS_DIR)/LXMF

$(HARNESS_VENV)/.installed: $(HARNESS_DIR)/Reticulum/.git $(HARNESS_DIR)/LXMF/.git
	python3 -m venv $(HARNESS_VENV)
	$(HARNESS_PIP) install --upgrade pip
	$(HARNESS_PIP) install -e $(HARNESS_DIR)/Reticulum
	$(HARNESS_PIP) install -e $(HARNESS_DIR)/LXMF
	$(HARNESS_PIP) install pytest
	@( cd $(HARNESS_DIR)/Reticulum && echo "Reticulum $$(git rev-parse HEAD)" ) > $(HARNESS_DIR)/INSTALLED.txt
	@( cd $(HARNESS_DIR)/LXMF       && echo "LXMF       $$(git rev-parse HEAD)" ) >> $(HARNESS_DIR)/INSTALLED.txt
	touch $@

harness-clean:
	@test -n "$(HARNESS_DIR)"
	rm -rf $(HARNESS_DIR)

rns-reset:
	@test -n "$(RNS_DIR)"
	rm -rf $(RNS_DIR)
