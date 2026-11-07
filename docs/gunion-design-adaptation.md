# Missing server definition adaptations

`tools.gunion.design_adaptation` prepares explicit design values when an owner
has authorized adapting a recovered client to a different server. It is separate
from source recovery and from the administrative host policy. It does not connect
to a database, reserve identifiers, register resources, install content or grant
activation authority.

The caller supplies a fixed-definition snapshot by its exact SHA-256 and passes
`user-approved-missing-server-design-adaptation`. A snapshot is reference data,
not evidence of the missing source server's balance. The result retains source
values and puts every added value in `design_values`, with a rule, reference
digest and template identifiers in `design_provenance`. Existing source values,
including zero, always win. A nonfinite recovered value, or a value rejected by
the destination's supported range, must first be explicitly excluded by a source
adapter that preserves its original bytes and rejection reason.

For a missing `FatalAttackIncrease`, candidates must have the same grade and
weapon type. Relative differences in HP, attack, defense and movement rank the
candidates, with a small grade-subtype penalty. The lower median of up to five
nearest values chooses an observed local value. Template selection uses exact
rational comparisons and identifier tie breaking. Callers should exclude imported
definitions from the reference candidate set.

For an absent weapon `AttackSpeed`, candidates must match attack style, range
type and hit type and have a positive finite speed. Matching combo count,
ammunition behavior and range rank candidates. The lower median of up to five
nearest values is used. Neither algorithm extrapolates outside the selected
local values or changes damage, animation, range or ammunition.

Affiliation recovery and structural special-attack capability use their own
source-backed adapters. This module cannot add a passive skill or silently replace
an unknown skill with zero. Server-only skill rules and broader balance changes
need separate explicit definitions and validation.

Run the offline contract checks with:

```text
python -m unittest discover -s tests -p test_gunion_design_adaptation.py -v
```

These checks validate data selection and preservation. Actual registration,
native execution and gameplay need the destination protocol's acceptance checks.
