# Ryu source in LuxIR

Upstream: https://github.com/ulfjack/ryu
Pinned commit: `4c0618b0e44f7ef027ebae05d2cc7812048f7c8f`.
Copyright Ulf Adams and contributors. Included under Boost Software License
1.0; original copyright/license headers and both upstream licenses retained.

LuxIR changes: remove allocating `d2s` and NUL-terminated `s2d` convenience
functions, redirect memcpy to raw luxrt at integration, replace umul128
with C-reference uint128 / generated-IR umulh+mul. No stdlib calls remain
in the integrated module. Ryu's public shortest and <=17-digit conversion
algorithms are used; arbitrary long input fallback is LuxIR code.
