#!/usr/bin/env bash
# Build .deb and .rpm packages for Motion Master.
#
# Requirements (all available via apt): dpkg-dev, rpm
# cert.pem and key.pem must already exist in build/<preset>/apps/motion_master/
# (fetched from the rolling tls-cert release in CI; copy manually for local testing).
#
# Usage: ./tools/package.sh [preset]
set -euo pipefail

PRESET="${1:-x64-linux-release}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build/$PRESET/apps/motion_master"
VERSION=$(cat "$REPO_DIR/VERSION")

# deb and rpm spell the same machine differently, so each preset maps to a pair.
case "$PRESET" in
    x64-linux-*)   DEB_ARCH=amd64; RPM_ARCH=x86_64 ;;
    arm64-linux-*) DEB_ARCH=arm64; RPM_ARCH=aarch64 ;;
    *) echo "Not a Linux packaging preset: $PRESET" >&2; exit 1 ;;
esac

for f in motion-master cert.pem key.pem; do
    [[ -f "$BUILD_DIR/$f" ]] || { echo "Missing: $BUILD_DIR/$f" >&2; exit 1; }
done

deb_root=$(mktemp -d)
rpm_root=$(mktemp -d)
trap 'rm -rf "$deb_root" "$rpm_root"' EXIT

# ── .deb ──────────────────────────────────────────────────────────────────────
mkdir -p "$deb_root/DEBIAN" \
         "$deb_root/opt/motion-master" \
         "$deb_root/usr/local/bin"

cp "$BUILD_DIR/motion-master" "$deb_root/opt/motion-master/motion-master"
cp "$BUILD_DIR/cert.pem"      "$deb_root/opt/motion-master/cert.pem"
cp "$BUILD_DIR/key.pem"       "$deb_root/opt/motion-master/key.pem"
cp "$REPO_DIR/setup.sh"       "$deb_root/opt/motion-master/setup.sh"
cp "$REPO_DIR/install-auto-tuning.sh" "$deb_root/opt/motion-master/install-auto-tuning.sh"
cp "$REPO_DIR/apps/motion_master/motion-master.example.jsonc" \
   "$deb_root/opt/motion-master/motion-master.example.jsonc"
chmod 755 "$deb_root/opt/motion-master/motion-master" \
          "$deb_root/opt/motion-master/setup.sh" \
          "$deb_root/opt/motion-master/install-auto-tuning.sh"
chmod 644 "$deb_root/opt/motion-master/cert.pem" \
          "$deb_root/opt/motion-master/key.pem" \
          "$deb_root/opt/motion-master/motion-master.example.jsonc"
ln -sf /opt/motion-master/motion-master "$deb_root/usr/local/bin/motion-master"

cat > "$deb_root/DEBIAN/control" <<EOF
Package: motion-master
Version: ${VERSION}
Architecture: ${DEB_ARCH}
Maintainer: Marko Sanković <msankovic@synapticon.com>
Depends: libcap2-bin, curl
Description: Motion control software for SOMANET servo drives
EOF

cp "$REPO_DIR/packaging/postinst" "$deb_root/DEBIAN/postinst"
cp "$REPO_DIR/packaging/postrm"   "$deb_root/DEBIAN/postrm"
chmod 755 "$deb_root/DEBIAN/postinst" "$deb_root/DEBIAN/postrm"

# cert and key are marked as conffiles so upgrades prompt rather than silently overwrite
printf '/opt/motion-master/cert.pem\n/opt/motion-master/key.pem\n' \
    > "$deb_root/DEBIAN/conffiles"

DEB="$REPO_DIR/motion-master-${VERSION}-${DEB_ARCH}.deb"
dpkg-deb --root-owner-group --build "$deb_root" "$DEB"
echo "Created: $DEB"

# ── .rpm ──────────────────────────────────────────────────────────────────────
# RPM version field cannot contain '-': split at first '-'
RPM_VERSION="${VERSION%%-*}"
RPM_RELEASE="${VERSION#*-}"
[[ "$RPM_RELEASE" == "$VERSION" ]] && RPM_RELEASE="1"

mkdir -p "$rpm_root"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

cp "$BUILD_DIR/motion-master" "$rpm_root/SOURCES/"
cp "$BUILD_DIR/cert.pem"      "$rpm_root/SOURCES/"
cp "$BUILD_DIR/key.pem"       "$rpm_root/SOURCES/"
cp "$REPO_DIR/setup.sh"       "$rpm_root/SOURCES/"
cp "$REPO_DIR/install-auto-tuning.sh" "$rpm_root/SOURCES/"
cp "$REPO_DIR/apps/motion_master/motion-master.example.jsonc" "$rpm_root/SOURCES/"

cat > "$rpm_root/SPECS/motion-master.spec" <<SPEC
Name:           motion-master
Version:        ${RPM_VERSION}
Release:        ${RPM_RELEASE}%{?dist}
Summary:        Motion control software for SOMANET servo drives
License:        GPL-3.0
URL:            https://motion-master.synapticon.com
BuildArch:      ${RPM_ARCH}
Requires:       libcap curl
AutoReq:        no

%description
Motion control software for SOMANET servo drives by Synapticon GmbH.

%build

%install
install -d %{buildroot}/opt/motion-master %{buildroot}/usr/local/bin
install -m 755 %{_sourcedir}/motion-master %{buildroot}/opt/motion-master/
install -m 644 %{_sourcedir}/cert.pem      %{buildroot}/opt/motion-master/
install -m 644 %{_sourcedir}/key.pem       %{buildroot}/opt/motion-master/
install -m 755 %{_sourcedir}/setup.sh      %{buildroot}/opt/motion-master/
install -m 755 %{_sourcedir}/install-auto-tuning.sh %{buildroot}/opt/motion-master/
install -m 644 %{_sourcedir}/motion-master.example.jsonc %{buildroot}/opt/motion-master/
ln -sf /opt/motion-master/motion-master %{buildroot}/usr/local/bin/motion-master

%post
setcap cap_sys_nice,cap_net_admin,cap_net_raw,cap_ipc_lock=eip /opt/motion-master/motion-master
# The twin of packaging/postinst; keep the two in step.
/opt/motion-master/install-auto-tuning.sh /opt/motion-master

%postun
# %post downloads the auto-tuning executable, so rpm never installed that file and does not know
# it. Without this, an uninstall would leave 65 MB behind. \$1 is 0 on a real uninstall only, so
# an upgrade keeps the file.
if [ \$1 -eq 0 ]; then
    rm -f /opt/motion-master/auto-tuning
fi

%files
%defattr(-,root,root,-)
/opt/motion-master/motion-master
%config(noreplace) /opt/motion-master/cert.pem
%config(noreplace) /opt/motion-master/key.pem
/opt/motion-master/setup.sh
/opt/motion-master/install-auto-tuning.sh
/opt/motion-master/motion-master.example.jsonc
/usr/local/bin/motion-master

%changelog
SPEC

# --target names the package arch explicitly instead of inheriting the build host's, so
# an arm64-linux-cross-* preset packages an aarch64 rpm from an x86_64 machine.
rpmbuild --target "$RPM_ARCH" --define "_topdir $rpm_root" -bb "$rpm_root/SPECS/motion-master.spec"
RPM="$REPO_DIR/motion-master-${VERSION}-${RPM_ARCH}.rpm"
find "$rpm_root/RPMS" -name "*.rpm" -exec cp {} "$RPM" \;
echo "Created: $RPM"
