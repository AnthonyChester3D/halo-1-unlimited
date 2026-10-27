# Notice (online branch)

Halo 1 Unlimited's online branch combines work under three kinds of terms. This file says which is which.

## Under AGPL-3.0-or-later

- Code taken from [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) at or after its commit `4b09168`
  (19 October 2026), and everything in this branch changed or added from the commit that switched it to the AGPL
  ("Online branch: AGPL-3.0-or-later") on. Each port of OpenCE's code names the OpenCE commit or version it took, in
  its commit message.
- The whole of each build made from those commits (the parts of it that are this project's and OpenCE's work).

## Under CC0 1.0

- Every commit before the switch, on every branch, and all of `main`.
- The OpenCE code this project forked on 14 October 2026 (OpenCE commit `8102e711`), as it was then.

The CC0 text is in [LICENSES/CC0-1.0.txt](LICENSES/CC0-1.0.txt).

## Third-party code (its own licence, in its folder)

Each folder in `port/third_party` carries its own licence file and keeps its own terms. The ones the Xbox's online
builds use:

| Code | Where | Licence |
|---|---|---|
| lwIP 2.2.1 (the TCP/IP stack) | `port/third_party/lwip` | BSD 3-clause |
| nxdk's network driver (nvnetdrv, sys_arch) | `port/third_party/nxdk_net` | MIT and BSD 3-clause (see its README.oxbox) |
| KCP (reliable streams over the tunnel) | `port/third_party/kcp` | MIT |
| Monocypher (the tunnel's cryptography) | `port/third_party/monocypher` | CC0 1.0 or BSD 2-clause |

The rest (miniupnpc, mbedtls, opus, zlib, expat and others) belong to OpenCE's PC, Android and web builds.

The installer (`port/oxbox/installer`) is built with [nxdk](https://github.com/XboxDev/nxdk), which isn't in this
repository; its XBE contains nxdk's libraries (MIT; the kernel imports CC0 1.0), nxdk's USB stack libusbohci
(Apache 2.0), PDCLib (CC0 1.0), nxdk's SDL 2.0.9 (zlib licence), zlib 1.3.1 (zlib licence) and the unscii font
(public domain). Their notices are in
`port/oxbox/installer/THIRD_PARTY.txt`, which goes beside the installer in a release.

## Not covered

Halo, its maps and art, and the retail game's code in an XBE belong to Microsoft. None of them is included in this
repository, in the installer or in its packages, or covered by any licence here: the installer makes OpenCE from the
player's own retail XBE, and its pictures from the player's own `ui.map`.
