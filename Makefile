# fanshell-qs — the bar and desktop shell shared by the fanoxwm-qs and
# fanqtile-qs sessions. One build runs under either window manager.
#
#   make              the shell
#   make install      shell and helper scripts into $(PREFIX)/bin
#   make install-pam  the lock screen's PAM service into /etc/pam.d
#   make link-config  link the shell's config files into $HOME

PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
PAMDIR ?= $(DESTDIR)/etc/pam.d

SCRIPTS = $(wildcard scripts/fanshell-qs-*)

all: shell/fanshell-qs-shell

shell/fanshell-qs-shell:
	$(MAKE) -C shell

# The helper scripts must be on the PATH that fanshell-qs-shell inherits: the
# widgets exec them by bare name, and a missing one just makes that widget
# quietly show defaults. Installing them alongside the binary is what
# guarantees that.
install: all
	install -d $(BINDIR)
	install -m 755 shell/fanshell-qs-shell $(BINDIR)/fanshell-qs-shell
	install -m 755 $(SCRIPTS) $(BINDIR)

# Not under $(PREFIX): PAM looks for services in /etc/pam.d.
install-pam:
	install -d $(PAMDIR)
	install -m 644 config/pam.d/fanshell-qs-lock $(PAMDIR)/fanshell-qs-lock

# Per user, so never run as root: it links into $HOME.
link-config:
	./tools/link-config

uninstall:
	rm -f $(BINDIR)/fanshell-qs-shell
	for s in $(notdir $(SCRIPTS)); do rm -f $(BINDIR)/$$s; done

clean:
	$(MAKE) -C shell clean

.PHONY: all install install-pam link-config uninstall clean shell/fanshell-qs-shell
