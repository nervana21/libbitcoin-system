# fix-bip54-header-timestamp

## In one breath

BIP54 timewarp and negative timespan must judge the header stamp.

A safe looking `ctx.timestamp` must not hide an attack header stamp.

## What the branch is solving

`header::accept` used to feed `ctx.timestamp` into BIP54 stamp bounds.

`ctx.timestamp` is chain context clipboard, not the candidate header field.

A stale safe clipboard value could skip `early_timestamp` and `negative_timespan`.

The fix passes `timestamp_` from the header into both checks.

## What to notice in the commits

First commit adds two red pin tests.

Each pin pairs a safe looking `ctx.timestamp` with an attack header stamp.

Those pins assert the wrong outcome on purpose until the fix lands.

Second commit adds `block_timestamp` to `is_early_timestamp` and `is_negative_timespan`.

`header::accept` passes `timestamp_` into both helpers.

The red pin cases flip to expect `early_timestamp` and `negative_timespan`.

`ctx.timestamp` still exists for other chain uses.

`ctx.timestamp` is no longer the BIP54 stamp input.
