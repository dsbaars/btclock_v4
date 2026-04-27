# Convenience targets. Most of the firmware build flow stays in
# `idf.py` (see docs/BUILD_FROM_SOURCE.md); this Makefile groups the
# small set of commands that are awkward to remember verbatim.

VENV_DOCS := .venv-docs
PY := python3

.PHONY: help docs-deps docs-serve docs-build docs-clean

help:
	@echo "BTClock convenience targets:"
	@echo
	@echo "  Docs site (MkDocs Material — see docs/BUILD_FROM_SOURCE.md#13):"
	@echo "    make docs-deps    # create .venv-docs and install pinned dependencies"
	@echo "    make docs-serve   # live-reload preview at http://127.0.0.1:8000"
	@echo "    make docs-build   # render the static site into ./site/"
	@echo "    make docs-clean   # remove ./site/ and the docs venv"
	@echo
	@echo "Firmware builds: source the IDF env first, then idf.py — see"
	@echo "docs/BUILD_FROM_SOURCE.md."

$(VENV_DOCS)/bin/mkdocs: mkdocs-requirements.txt
	$(PY) -m venv $(VENV_DOCS)
	$(VENV_DOCS)/bin/pip install --upgrade pip
	$(VENV_DOCS)/bin/pip install -r mkdocs-requirements.txt
	@touch $@

docs-deps: $(VENV_DOCS)/bin/mkdocs

docs-serve: docs-deps
	$(VENV_DOCS)/bin/mkdocs serve

docs-build: docs-deps
	$(VENV_DOCS)/bin/mkdocs build --clean --strict || $(VENV_DOCS)/bin/mkdocs build --clean

docs-clean:
	rm -rf site $(VENV_DOCS)
