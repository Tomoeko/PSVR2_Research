# BusyBox configuration

`psvr2-maximal.config` contains supported BusyBox 1.29.0 Kconfig overrides.
The build helper obtains the pinned public PSVR2 OSS archive, selects the real
inner `busybox/src` tree, starts from `allyesconfig`, applies this fragment and
resolves dependencies with `oldconfig`. The archive's outer SDK wrapper is not
used.

The fragment enables static linking, ash and hush, and disables integrations
that require unavailable external libraries or incompatible glibc internals.
The GPL BusyBox source and its notices remain in the external build cache;
this directory contains the authored configuration fragment and the small
`link-options.patch` for linkers that reject optional diagnostic flags.
