# Friend unit gifts (optional)

[简体中文](friend-unit-gift.zh-CN.md) · [Back to the feature index](../README.md) · [Friend list fix](friend-list-fix.md)

Offer an owned unit to a mutually accepted friend. The gift transfers the same unit instance, preserving its level, progression, skills, parts, appearance and original acquisition date.

The feature is off by default. Server owners turn on **启用好友机体赠送** (enable friend unit gifts) under
**好友机体赠送** in SDGO Mod Manager and adjust eligibility there. The client shows the offer entry when the connected server supports and enables it. The interface reuses existing client assets.

## Send and receive

1. Right-click a friend in the game and choose the unit-gift action.
2. Select an owned unit and optionally write a personal note. Review the server's preflight result, then confirm.
3. The unit enters pending storage and is no longer usable by the sender. The recipient may be offline.
4. The recipient opens the unit-gift view in Storage and accepts or returns the offer.

Acceptance requires a free recipient slot. A capacity refusal leaves the offer pending. Returns prefer the sender's original slot, then another free slot. If the sender is full, the unit remains stored until the sender can take it back.

Ordinary rewards retain their native handling. Claim All does not accept these offers, and deleting an ordinary gift note is not a unit return.

## Eligibility

The default rules retain the legacy requirements: at least 20 battles, at least one player kill, and at least two distinct owned units in each of C, CS, CR, B, BS, BR, A, AS, AR, S, SS and SR. Server owners can adjust these progression thresholds in Manager.

Ownership and unit-state checks remain mandatory. The sender cannot gift to themselves, and the offer requires an accepted, unblocked friendship. Locked, active, assist, expired, forbidden, pending, or saved-fleet/leader units cannot be offered. The server rechecks the exact instance when confirming.

## Provenance and configuration

Accepted units provide a provenance entry in their details, showing the sender, server-recorded gift time and note. This metadata belongs to the instance and does not replace the shared description for that unit type. The original acquisition date remains separate.

Install matching `friend_unit_gift_client` and Agent/World `friend_unit_gift_server` components through the existing runtime distribution. The server uses native database sessions and automatically maintains its owned schema. Players need no database credentials or companion service.

Turning the switch off in Manager (and restarting) stops new offers while keeping existing accept, return, takeback and provenance operations. Resolve stored units before uninstalling the server components.

After a timeout or disconnect, an operation may remain unconfirmed. Query its original result after reconnecting and check authoritative inventory/storage; do not assume rollback or send another offer. Acceptance and return produce one final outcome.

Unsupported servers retain native friend, mail, shop-gift and storage handling.
