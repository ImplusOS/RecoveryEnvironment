# This repository is a component of ImplusOS (github.com/ImplusOS).
# It has no independent build of its own: `all` delegates two levels up to
# the top-level ImplusOS Makefile's `recovery_build` target (see README.md).
# All source lives in Source/.

.PHONY: all clean

all clean:
	@$(MAKE) -C Source $@
