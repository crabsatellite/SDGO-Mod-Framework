# Friend unit gifts (optional)

[简体中文](friend-unit-gift.zh-CN.md) · [Back to the feature index](../README.md) · [Friend list fix](friend-list-fix.md)

Offer an owned unit to a mutually accepted friend. The gift transfers the same unit instance, preserving its level, progression, skills, parts, appearance and original acquisition date.

The feature is off by default. Server owners turn on **启用好友机体赠送** (enable friend unit gifts) under
**好友机体赠送** in SDGO Mod Manager and adjust eligibility there. The client shows the offer entry when the connected server supports and enables it. The gift preview reuses the native unit-acquisition card, showing its original model, weapons, skills and level. Unit selection, the optional note and confirmation use existing native controls and assets.

## Send and receive

Client component 0.1.6 fixes a crash when reopening Storage after its old native controls have been released. Closing invalidates the cached gift-page controls; reopening binds the new page. Restart the client after updating.

1. Right-click an accepted friend in the home screen's top-right **USER LIST** or the messenger, then choose the unit-gift action. Online and offline recipients are supported.
2. The action opens native unit management. Double-click the owned instance, review its name and level in the native unit-information card, and optionally write a note. Review the server's preflight result, then confirm.
3. The unit enters pending storage and is no longer usable by the sender. The recipient may be offline.
4. The recipient opens unit gifts in Storage, using its existing unit-entry layout. Click the unit icon for the sender, timestamps and note in the native information card, then accept or return the offer. Notes stay in the card rather than the Storage row.

The menu follows the right-click position and uses the game's blue button background. The information card
keeps the native unit model, name, level and skills; its lower-right area provides a single panel for the
note and confirmation controls using existing game assets.

During gift selection, the left and right arrows move one page at a time. Page dots select a page in the
current group, and arrows also cross group boundaries. The first and last pages stay in bounds. Double-click
an owned instance on any page to open its information card. Cancelling the card lets you browse and choose again.
Update the client component and restart the client for these controls to take effect.

Acceptance requires a free recipient slot. A capacity refusal leaves the offer pending. Returns prefer the sender's original slot, then another free slot. If the sender is full, the unit remains stored until the sender can take it back.

Ordinary rewards retain their native handling. Claim All does not accept these offers, and deleting an ordinary gift note is not a unit return.

## Eligibility

The default rules retain the legacy requirements: at least 20 battles, at least one player kill, and at least two distinct owned units in each of C, CS, CR, B, BS, BR, A, AS, AR, S, SS and SR. Server owners can adjust these progression thresholds in Manager.

Ownership and unit-state checks remain mandatory. The sender cannot gift to themselves, and the offer requires an accepted, unblocked friendship. Locked, active, assist, expired, forbidden, pending, or saved-fleet/leader units cannot be offered. The server rechecks the exact instance when confirming.

Equipped skill parts remain attached to the unit. The client reads the selected instance's existing details from the server before previewing; wait for that read to finish. Preview and cancellation leave ownership unchanged.

## Provenance and configuration

Accepted units provide a provenance entry in their details, showing the sender, server-recorded gift time and note. This metadata belongs to the instance and does not replace the shared description for that unit type. The original acquisition date remains separate.

Install matching `friend_unit_gift_client` and Agent/World `friend_unit_gift_server` components through the existing runtime distribution. The server uses native database sessions and automatically maintains its owned schema. Players need no database credentials or companion service.

Turning the switch off in Manager (and restarting) stops new offers while keeping existing accept, return, takeback and provenance operations. Resolve stored units before uninstalling the server components.

After a timeout or disconnect, an operation may remain unconfirmed. Query its original result after reconnecting and check authoritative inventory/storage; do not assume rollback or send another offer. Acceptance and return produce one final outcome.

Unsupported servers retain native friend, mail, shop-gift and storage handling.

The friend menu reuses the existing blue menu strip and its transparent normal,
hover and pressed button states. The native popup routine positions the expanded
menu at the right-click location; background capability polling does not reset
that position.

## Developer verification

The native friend-gift runner supports `--phase picker-pagination` with the existing prepared profiles and
unit binding. It exercises page dots, adjacent arrows, group boundaries, later-page double-clicks and
cancellation through scoped virtual pointer input. This phase checks the original instance without confirming
an offer. See [native testing](game-test.md) for the isolated environment and account requirements.
