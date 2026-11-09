# Special-target selection guard

This automatic fix contains an out-of-bounds read in the native randomized special-target selection path. The supported function stores at most 12 target identifiers. When randomized selection sees a signed count below zero or above 12, the component returns the function's native failure value: a `Vec3` whose three components are positive zero.

## Preserved behavior

Non-random selection always calls the original function without consulting the list count. Random selection also calls the original function for an empty list and every valid count from 1 through 12. These paths retain native default-target lookup, random selection, target-state checks, virtual calls, return pointer and output value.

The guard reads the same signed count field used by the native function only after the random-selection flag is true. Its comparisons use the full 32-bit signed range without arithmetic that can overflow. Values such as signed `0xA6A5A4A3`, `INT_MIN`, 13 and `INT_MAX` therefore take the zero-result path without division, remainder calculation or list access. The count and owner object are not modified.

The component binds only when the shipped function's exact entry bytes match the supported executable. Its passive observer reports forwarded calls, rejected counts and the last rejected signed value.

## Limits and verification boundary

This component contains the unsafe consumer. It does not identify or repair the producer of an invalid count, establish object lifetime, or fix a use-after-free. The owner and output pointers must still be readable and writable exactly as the original function requires. The guard does not catch access violations, swallow exceptions, allocate replacement list storage or intercept the general heap.

Static recovery establishes the Win32 `this`/output/flag ABI, the signed count field, the 12-entry storage and the native zero-vector failure result. The production-DLL fixture covers forwarding, signed boundary values, output bits, unchanged owner bytes and original-call suppression. Native in-client proof is pending and must be recorded separately before release; the fixture alone is not gameplay or runtime acceptance.
