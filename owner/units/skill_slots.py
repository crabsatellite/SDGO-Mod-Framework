"""The supported client's native ordinary-slot classifier, not a skill whitelist."""
# CUnit's native skill-state consumer classifies these IDs into slots 1/2/3;
# other positive IDs go to its separate extra-skill slot. Record kind 13 by
# itself does not choose the actor slot. Keep the original open boundaries.
ORDINARY_RANGES=((1,75000),(3220001,3225000),(3473001,3478000),
                 (3558001,3559000),(3613002,3615999))

def ordinary(identity):
    return type(identity) is int and any(first<=identity<=last for first,last in ORDINARY_RANGES)
