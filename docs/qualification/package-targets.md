<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-qualification-compact.svg"><img src="../media/nav/banner-qualification.svg" alt="CodexUI — Qualification" width="100%"></picture></p>
<p><a href="README.md#banner" title="Up to Qualification"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-qualification-dark.svg"><img src="../media/nav/up-qualification-light.svg" alt="Up to Qualification" title="Up to Qualification" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/account-dark.svg"><img align="right" src="../media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="CodexUI repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="CodexUI repository" title="CodexUI repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Native package targets

Index audit: 2026-11-11. The allowed starting set is exactly the 26 native targets from SNodeC/snode.c-idp at `f0cb4d44a8da33f68ab56f2fedf24bdb0ecef4c8`. Requirements are Qt6 Widgets and Network ≥6.6, KF6Sonnet ≥6.13, and libgit2. Versions below identify available upstream versions; distribution revisions and security suffixes are omitted.

| Target | Qt6 | KF6Sonnet | libgit2 | Included |
| --- | --- | --- | --- | --- |
| debian-trixie-amd64 | 6.8.2 | 6.13.0 | 1.9.0 | yes |
| debian-trixie-arm64 | 6.8.2 | 6.13.0 | 1.9.0 | yes |
| debian-trixie-armhf | 6.8.2 | 6.13.0 | 1.9.0 | yes |
| debian-trixie-riscv64 | 6.8.2 | 6.13.0 | 1.9.0 | yes |
| debian-forky-amd64 | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-forky-arm64 | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-forky-armhf | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-forky-riscv64 | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-sid-amd64 | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-sid-arm64 | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-sid-armhf | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| debian-sid-riscv64 | 6.11.2 | 6.30.0 | 1.9.7 | yes |
| ubuntu-noble-amd64 | 6.4.2 (<6.6) | unavailable | 1.7.2 | no |
| ubuntu-noble-arm64 | 6.4.2 (<6.6) | unavailable | 1.7.2 | no |
| ubuntu-resolute-amd64 | 6.10.2 | 6.24.0 | 1.9.1 | yes |
| ubuntu-resolute-arm64 | 6.10.2 | 6.24.0 | 1.9.1 | yes |
| rocky-9-x86_64 | 6.10.3 (EPEL) | unavailable | 1.7.2 (EPEL) | no |
| rocky-9-aarch64 | 6.10.3 (EPEL) | unavailable | 1.7.2 (EPEL) | no |
| rocky-10-x86_64 | 6.10.1 | 6.30.0 (EPEL) | 1.7.2 (EPEL) | yes |
| rocky-10-aarch64 | 6.10.1 | 6.30.0 (EPEL) | 1.7.2 (EPEL) | yes |
| fedora-43-x86_64 | 6.10.3 | 6.30.0 | 1.9.7 | yes |
| fedora-43-aarch64 | 6.10.3 | 6.30.0 | 1.9.7 | yes |
| fedora-44-x86_64 | 6.11.2 | 6.31.0 | 1.9.7 | yes |
| fedora-44-aarch64 | 6.11.2 | 6.31.0 | 1.9.7 | yes |
| raspberrypios-bookworm-arm64 | 6.4.2 (<6.6) | unavailable | 1.5.1 | no |
| raspberrypios-trixie-arm64 | 6.8.2 | 6.13.0 | 1.9.0 | yes |

Decision: include the 21 qualifying targets and exclude both Ubuntu noble targets, both Rocky 9 targets and Raspberry Pi OS bookworm arm64 because required distribution packages do not meet the stated minimums. No OpenWrt target is declared. A failed target does not block enrollment of the others.

The existing trusted preparation in DistributionPackages `coreci/build.py` already installs `epel-release` and enables CRB for Rocky. This audit uses only those existing repositories, plus each distribution's normally enabled repositories and updates. It adds no repository or dependency bootstrap to the product hook.

## Official index evidence

- Debian publishes architecture lists and versions for [Qt6](https://packages.debian.org/search?keywords=qt6-base-dev&searchon=names&suite=all&section=all), [KF6 Sonnet](https://packages.debian.org/search?keywords=libkf6sonnet-dev&searchon=names&suite=all&section=all), and [libgit2](https://packages.debian.org/search?keywords=libgit2-dev&searchon=names&suite=all&section=all). The four declared architectures are listed for trixie, forky and sid. A direct trixie `apt-cache policy` check confirmed Qt 6.8.2, Sonnet 6.13.0 and libgit2 1.9.0.
- Ubuntu's official indexes list [Qt6](https://packages.ubuntu.com/search?keywords=qt6-base-dev&searchon=names&suite=all&section=all), [KF6 Sonnet](https://packages.ubuntu.com/search?keywords=libkf6sonnet-dev&searchon=names&suite=all&section=all), and [libgit2](https://packages.ubuntu.com/search?keywords=libgit2-dev&searchon=names&suite=all&section=all). Noble has Qt 6.4.2 and no KF6 Sonnet package; resolute lists qualifying amd64 and arm64 packages.
- Fedora's official package indexes list [Qt6](https://packages.fedoraproject.org/pkgs/qt6-qtbase/qt6-qtbase-devel/), [Sonnet](https://packages.fedoraproject.org/pkgs/kf6-sonnet/kf6-sonnet-devel/) and [libgit2](https://packages.fedoraproject.org/pkgs/libgit2/libgit2-devel/). Direct `dnf repoquery` checks for releases 43 and 44 confirmed the table for x86_64 and aarch64.
- Rocky base repositories are at [Rocky 9](https://dl.rockylinux.org/pub/rocky/9/) and [Rocky 10](https://dl.rockylinux.org/pub/rocky/10/). The same EPEL package indexes above document supplemental packages. Direct `dnf repoquery` checks after the existing EPEL/CRB preparation confirmed both architectures: Rocky 9 provides Qt/libgit2 but no `kf6-sonnet-devel`; Rocky 10 provides all three.
- Raspberry Pi OS arm64 uses Debian packages plus the official Raspberry Pi archive. The [bookworm arm64 index](https://archive.raspberrypi.com/debian/dists/bookworm/main/binary-arm64/Packages.gz) and [trixie arm64 index](https://archive.raspberrypi.com/debian/dists/trixie/main/binary-arm64/Packages.gz) contain no overrides for these three development packages; the Debian suite versions therefore govern this selection.

The RPM checks used the official image's repository configuration and `repoquery --latest-limit 1`; `--forcearch=aarch64` selected ARM metadata without running ARM binaries. Availability is an index audit, not a claim that all 21 targets have completed package builds.

## Package scope

Decision: the package follows the established native verification configuration with `CODEXUI_INSTALL_WEB=OFF`. It includes `codex-ui`, `codex-manager`, both desktop files and icons, and the manager's installed web assets. The separate CodexWebUI production artifact retains its existing release workflow; it is not silently rebuilt with distribution Node versions below that artifact's declared Node ≥22 requirement.

Decision: `VERSION` supplies the requested 0.1.0 CMake version directly, rather than deriving it from old release tags. No tags are created, moved or deleted.

GUI package-hook tests run under Xvfb with `QT_QPA_PLATFORM=offscreen`. Packaging adds no alternate UI renderer, state owner or application configuration store.
