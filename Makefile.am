SUBDIRS = \
	data \
	doc \
	images \
	po \
	src \
	tests

# ACLOCAL_AMFLAGS = -I m4

CLOUD_HELPER_GO_SOURCES = \
	cloud-helper/archive.go \
	cloud-helper/backup_test.go \
	cloud-helper/commands.go \
	cloud-helper/config.go \
	cloud-helper/config_test.go \
	cloud-helper/crypto.go \
	cloud-helper/keyfile.go \
	cloud-helper/keyring.go \
	cloud-helper/lock.go \
	cloud-helper/main.go \
	cloud-helper/recovery.go \
	cloud-helper/recovery_snapshot_test.go \
	cloud-helper/remote.go \
	cloud-helper/remote_test.go \
	cloud-helper/restore_safety_test.go \
	cloud-helper/snapshot.go \
	cloud-helper/status.go \
	cloud-helper/terminal.go \
	cloud-helper/terminal_test.go

EXTRA_DIST = \
	autogen.sh \
	ci/install-go-toolchain.sh \
	$(CLOUD_HELPER_GO_SOURCES) \
	cloud-helper/go.mod \
	cloud-helper/go.sum \
	cloud-helper/vendor \
	src/COPYING.LESSER

# --- Go E2EE backup helper (conditional on AC_CHECK_PROG for go) ---
# Built from cloud-helper/ using the checksummed vendored modules; installed to
# $(bindir). Source and Debian builds therefore require no network access.
# The C build is never gated on this — devs without Go still build fine.
if HAVE_GO
cloud-helper/xnote-cloud-backup: $(srcdir)/cloud-helper/go.mod $(srcdir)/cloud-helper/vendor/modules.txt $(addprefix $(srcdir)/,$(CLOUD_HELPER_GO_SOURCES))
	$(MKDIR_P) cloud-helper
	cd $(srcdir)/cloud-helper && GOTOOLCHAIN=local GOPROXY=off $(GO) build -mod=vendor -trimpath -o $(abs_builddir)/$@ .

all-local: cloud-helper/xnote-cloud-backup

install-exec-local: cloud-helper/xnote-cloud-backup
	$(INSTALL_PROGRAM) cloud-helper/xnote-cloud-backup $(DESTDIR)$(bindir)/xnote-cloud-backup

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/xnote-cloud-backup

clean-local:
	rm -f cloud-helper/xnote-cloud-backup

check-go:
	cd $(srcdir)/cloud-helper && GOTOOLCHAIN=local GOPROXY=off $(GO) test -mod=vendor ./...

.PHONY: check-go
endif

DISTCLEANFILES = \
	xnote.desktop \
	po/xnote.pot \
	Makefile.in \
	aclocal.m4 \
	autom4te.cache/* \
	compile \
	config.h.in \
	configure \
	depcomp \
	install-sh \
	missing \
	doc/Makefile.in \
	images/Makefile.in \
	po/Makefile.in \
	src/Makefile.in \
	*.in~ \
	*.~1~
