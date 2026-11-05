"""Explicit native record profiles used by the owner-side importer."""

from .skill_target_1132 import (
    PROFILE_ID,
    recover_source_record,
    target_profile,
    validate_host_witness,
    validate_projected_record,
    validate_relocated_record,
)
from .funnel_target_732 import (
    PROFILE_ID as FUNNEL_PROFILE_ID,
    prepare_record as prepare_funnel_record,
    validate_record as validate_funnel_record,
)

__all__ = [
    "PROFILE_ID",
    "recover_source_record",
    "target_profile",
    "validate_host_witness",
    "validate_projected_record",
    "validate_relocated_record",
    "FUNNEL_PROFILE_ID",
    "prepare_funnel_record",
    "validate_funnel_record",
]
