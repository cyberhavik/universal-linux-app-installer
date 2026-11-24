Name:           universal-app-installer
Version:        1.0.0
Release:        1%{?dist}
Summary:        Universal Linux Drag & Drop App Installer & Uninstaller (.deb, .rpm, .AppImage, .pkg.tar.zst, .flatpak)
License:        MIT
URL:            https://github.com/cyberhavik/universal-linux-app-installer
BuildArch:      x86_64
Requires:       gtk4
Requires:       libadwaita
Requires:       tar
Requires:       cpio
Requires:       desktop-file-utils

%description
Universal Linux App Installer (uapp) is an ultra-fast, zero-Python native C
(GTK4 + Libadwaita) desktop utility that brings the macOS drag-and-drop
application installation and uninstallation workflow to ANY Linux distribution.
Install .deb (Ubuntu/Debian), .rpm (Fedora/RHEL/openSUSE), .AppImage,
.pkg.tar.zst (Arch Linux), .apk (Alpine), .flatpak bundles, and portable
.tar.gz/.zip archives across any Linux distro without root password prompts or
cross-distro package manager conflicts. Includes smart in-place updates and a
dual-mode uninstaller (Keep Data vs. Complete Data Purge).

%install
mkdir -p %{buildroot}/usr/bin
mkdir -p %{buildroot}/usr/share/applications
mkdir -p %{buildroot}/usr/share/icons/hicolor/scalable/apps

install -m 0755 %{_sourcedir}/uapp %{buildroot}/usr/bin/uapp
ln -sf uapp %{buildroot}/usr/bin/universal-app-installer
install -m 0644 %{_sourcedir}/com.astro.UniversalAppInstaller.desktop %{buildroot}/usr/share/applications/com.astro.UniversalAppInstaller.desktop
install -m 0644 %{_sourcedir}/com.astro.UniversalAppInstaller.svg %{buildroot}/usr/share/icons/hicolor/scalable/apps/com.astro.UniversalAppInstaller.svg

%files
/usr/bin/uapp
/usr/bin/universal-app-installer
/usr/share/applications/com.astro.UniversalAppInstaller.desktop
/usr/share/icons/hicolor/scalable/apps/com.astro.UniversalAppInstaller.svg

%post
update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true

%postun
update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true

%changelog
* Tue Nov 24 2026 Saurav Kumar <0501saurav@gmail.com> - 1.0.0-1
- Initial release of Universal Linux App Installer (uapp) in Native C (GTK4/Libadwaita)
