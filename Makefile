# ==============================================================================
# Makefile — Universal Linux App Installer (uapp)
# Native C (GTK4 + Libadwaita) Universal Drag & Drop Package Installer
# ==============================================================================

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
PKG_CFG  ?= pkg-config

# Support both standard system pkg-config and custom sysroot if present
SYSROOT  ?= /home/astro/.local/share/astro-claude-sysroot
ifneq ($(wildcard $(SYSROOT)/usr/lib64/pkgconfig/libadwaita-1.pc),)
    GTK_FLAGS := $(shell PKG_CONFIG_PATH="$(SYSROOT)/usr/lib64/pkgconfig:$(SYSROOT)/usr/share/pkgconfig" $(PKG_CFG) --cflags --libs libadwaita-1 gtk4)
else
    GTK_FLAGS := $(shell $(PKG_CFG) --cflags --libs libadwaita-1 gtk4)
endif

PREFIX      ?= /usr
BINDIR      ?= $(PREFIX)/bin
DATADIR     ?= $(PREFIX)/share
APPDIR      ?= $(DATADIR)/applications
ICONDIR     ?= $(DATADIR)/icons/hicolor/scalable/apps

USER_BINDIR  := $(HOME)/.local/bin
USER_APPDIR  := $(HOME)/.local/share/applications
USER_ICONDIR := $(HOME)/.local/share/icons/hicolor/scalable/apps

TARGET := uapp

.PHONY: all clean install install-user uninstall-user

all: $(TARGET)

$(TARGET): src/main.c
	$(CC) $(CFLAGS) src/main.c -o $(TARGET) $(GTK_FLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	ln -sf $(TARGET) $(DESTDIR)$(BINDIR)/universal-app-installer
	install -Dm644 packaging/com.astro.UniversalAppInstaller.desktop $(DESTDIR)$(APPDIR)/com.astro.UniversalAppInstaller.desktop
	install -Dm644 assets/com.astro.UniversalAppInstaller.svg $(DESTDIR)$(ICONDIR)/com.astro.UniversalAppInstaller.svg

install-user: $(TARGET)
	install -Dm755 $(TARGET) $(USER_BINDIR)/$(TARGET)
	ln -sf $(USER_BINDIR)/$(TARGET) $(USER_BINDIR)/universal-app-installer
	install -Dm755 packaging/com.astro.UniversalAppInstaller.desktop $(USER_APPDIR)/com.astro.UniversalAppInstaller.desktop
	install -Dm644 assets/com.astro.UniversalAppInstaller.svg $(USER_ICONDIR)/com.astro.UniversalAppInstaller.svg
	-update-desktop-database $(USER_APPDIR) >/dev/null 2>&1 || true

uninstall-user:
	rm -f $(USER_BINDIR)/$(TARGET) $(USER_BINDIR)/universal-app-installer
	rm -f $(USER_APPDIR)/com.astro.UniversalAppInstaller.desktop
	rm -f $(USER_ICONDIR)/com.astro.UniversalAppInstaller.svg
	-update-desktop-database $(USER_APPDIR) >/dev/null 2>&1 || true

clean:
	rm -f $(TARGET)
