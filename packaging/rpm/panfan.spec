Name:           panfan
Version:        0.1.0
Release:        1%{?dist}
Summary:        Fan controller for Panasonic CF-SC laptops

License:        MIT
URL:            https://github.com/tobyxdd/panfan
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-rpm-macros
Requires:       acpi_call-dkms
Requires:       systemd
ExclusiveArch:  x86_64

%description
panfan hands fan ownership to Linux through Panasonic ACPI methods and
controls the standard thermal cooling device from CPU package temperature.
It supports Panasonic CF-SC laptops reporting the CFSC-2 DMI product name.

%prep
%autosetup

%build
%make_build

%install
%make_install SBINDIR=%{_sbindir} SLEEPLINK=../../../bin/panfan
sed -i 's|/usr/sbin/panfan|%{_sbindir}/panfan|g' \
    %{buildroot}%{_unitdir}/panfan.service

%check
test "$(./panfan --version)" = "panfan %{version}"
./panfan policy
./panfan policy panfan.conf.example

%post
%systemd_post panfan.service

%preun
%systemd_preun panfan.service

%postun
%systemd_postun_with_restart panfan.service

%files
%license %{_docdir}/%{name}/LICENSE
%doc %{_docdir}/%{name}/README.md
%doc %{_docdir}/%{name}/panfan.conf.example
%{_sbindir}/panfan
%{_mandir}/man8/panfan.8*
%{_unitdir}/panfan.service
%{_prefix}/lib/modules-load.d/panfan.conf
%{_prefix}/lib/systemd/system-sleep/panfan

%changelog
* Sun Aug 16 2026 Toby <tobyxdd@gmail.com> - 0.1.0-1
- Initial package
