$(CURDIR)/version.h: force-version
	@set -e; \
	tmp=$$(mktemp "$@.XXXXXX"); \
	trap 'rm -f "$$tmp"' 0; \
	if test -e "$(srcdir)/.git"; then \
		version=$$(git -C "$(srcdir)" describe --tags --always --exclude="latest"); \
		escaped=$$(printf '%s' "$$version" | sed 's/[\\"]/\\&/g'); \
		printf '#define VERSION "%s"\n' "$$escaped" > "$$tmp"; \
	else \
		cat "$(srcdir)/version.h" > "$$tmp"; \
	fi; \
	if ! cmp -s "$$tmp" "$@"; then mv -f "$$tmp" "$@"; fi

.PHONY: force-version
force-version:
