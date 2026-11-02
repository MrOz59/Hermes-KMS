# Hermes-KMS native Fedora packages: DKMS source + optional private seatd.
# Built with a source tarball from the exact release commit.
Name:           hermes-kms-dkms
Version:        0.4.1
Release:        1%{?dist}
Summary:        Hermes-KMS virtual display driver (DKMS source)
License:        GPL-2.0-only AND MIT
URL:            https://github.com/MrOz59/Hermes-KMS
Source0:        Hermes-KMS-%{version}.tar.gz
BuildArch:      noarch

BuildRequires:  make
Requires:       dkms
Requires:       acl
Requires:       kmod
Requires:       make
Requires:       gcc
Recommends:     kernel-devel

%description
Source for the Hermes-KMS virtual DRM/KMS capture driver, registered with
DKMS to rebuild for the installed Linux kernels. Includes public UAPI headers
and read-only capture render-node access rules.

%package -n hermes-kms-seatd
Summary:        Optional private seatd brokers for Hermes-KMS sessions
Requires:       %{name} = %{version}-%{release}
Requires:       seatd
Suggests:       polkit

%description -n hermes-kms-seatd
Private seat brokers and setup helper for multiple isolated virtual DRM/KMS
sessions. Not required for standard single-session capture.

%prep
%autosetup -n Hermes-KMS
bash scripts/check-release-version.sh v%{version}

%build
# DKMS builds from the packaged sources on the target computer.

%install
make DESTDIR=%{buildroot} install-core-configs install-uapi install-broker-configs
install -dm0755 %{buildroot}/usr/src/hermes-kms-%{version}
cp -a Makefile dkms.conf include kernel \
    %{buildroot}/usr/src/hermes-kms-%{version}/

%posttrans
# A packaging operation must never load or force-unload a graphics module.
# Attempt the current-kernel DKMS build if exact headers are available.
if command -v dkms >/dev/null 2>&1; then
    if ! dkms status -m hermes-kms -v %{version} 2>/dev/null | grep -q .; then
        dkms add -m hermes-kms -v %{version} || :
    fi
    if [ -e "/lib/modules/$(uname -r)/build/Makefile" ]; then
        if ! dkms build -m hermes-kms -v %{version} \
             -k "$(uname -r)" ||
           ! dkms install -m hermes-kms -v %{version} \
             -k "$(uname -r)" --force; then
            echo 'Hermes-KMS DKMS build incomplete; inspect dkms status and the build log.' >&2
        fi
    else
        echo 'Hermes-KMS: install kernel-devel matching the running kernel, then build with DKMS.' >&2
    fi
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || :
    udevadm trigger --subsystem-match=drm --action=change || :
fi
echo 'If Hermes-KMS was already loaded, reboot before testing the updated module.'

%preun
if [ "$1" -eq 0 ] && command -v dkms >/dev/null 2>&1; then
    dkms remove -m hermes-kms -v %{version} --all || :
fi

%posttrans -n hermes-kms-seatd
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || :
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || :
fi
printf '%s\n' 'Optional private seats can be configured with:' \
    '  sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto'

%files
%license LICENSE
%doc README.md
/usr/src/hermes-kms-%{version}/
/usr/lib/modules-load.d/hermes-kms.conf
/usr/lib/modprobe.d/hermes-kms.conf
/usr/lib/udev/rules.d/72-hermes-kms-render-access.rules
/usr/lib/udev/rules.d/92-hermes-kms-access.rules
/usr/lib/udev/rules.d/95-hermes-kms-host-access.rules
/usr/lib/hermes-kms/hermes-kms-render-acl
/usr/bin/hermes-kms-unload
/usr/bin/hermes-kms-rebind
/usr/include/drm/hermes_kms_drm.h
/usr/include/hermes-kms/hermes_session.h
/usr/share/licenses/hermes-kms/MIT.txt

%files -n hermes-kms-seatd
/usr/lib/udev/rules.d/72-hermes-kms-session-seats.rules
/usr/lib/hermes-kms/hermes-kms-seatd-instance
/usr/lib/hermes-kms/hermes-kms-setup
/usr/lib/systemd/system/hermes-kms-seatd@.service
/usr/share/polkit-1/actions/io.github.mroz59.hermes-kms.policy

%changelog
* Mon Nov 02 2026 MrOz59 <74338254+MrOz59@users.noreply.github.com> - 0.4.1-1
- Initial native RPM packaging with DKMS driver and optional private seats.
