# AI pilot battle dialogue

AI pilots can react to your kills and defeats, or suggest focus fire as teammates. The feature is off by default and requires your own DeepSeek API key and a running Mod Manager.

## Setup

Add your client in Manager, open **AI pilot battle dialogue**, select the client, save a local API key, enable the feature and start its background service. Restart the game after installing the component. Settings, the prompt and pilot profiles reload after saving.

Keys use Windows current-user DPAPI encryption in the Manager's private workspace and are bound to the saved API endpoint. Save the key again after changing that address. Keys are excluded from shared configuration, owner content, generic Manager releases and AI support handoffs. Each player supplies their own key. Stopping the service stops new requests.

| Setting | Default | Behavior |
|---|---|---|
| Enabled | Off | No API requests while disabled |
| Trigger | Probability | Every eligible event, or an independent probability roll |
| Probability | 35% | Per eligible native event |
| Visibility | Local | Optionally broadcast in the current battle room |
| Cooldown | 8 seconds | Zero allows consecutive events, subject to concurrency and quota |
| Calls per battle | 24 | Failed calls also consume the limit |
| Teammate suggestions | On | Assigned allied AI can suggest focus fire |
| Model | `deepseek-flash` | Fast model, with thinking disabled |
| Language | Simplified Chinese | Editable |

Manager performs HTTP work asynchronously. The game never waits for it. Each client has one outstanding request; busy, expired and over-quota events are skipped. Replies are discarded after leaving the battle, changing scenes or configuration, disabling the feature or reaching the deadline.

## Scope and identity

Enemy AI responds only when you defeat it or it defeats you. Unrelated kills between other players do not use your API. Autonomous allied comments are deterministically assigned among allied humans so clients do not all generate the same comment.

Relationships follow the native battle player records used by the team HUD, with player and slot identities checked. A reply is checked again before dispatch: an opposing AI cannot send a teammate suggestion, and a focus target must still be a living enemy. The prompt explicitly identifies whether the speaker and addressee are allies or opponents.

The original battle text area displays `Rick Dias: line`, using the actual unit name without object numbers or arrows. A room broadcast always names its actual addressee, resolved by the game rather than left to the model. Other players, including opponents, can see it; a suggestion addressed to an AI's ally is not an invitation to every viewer. Local visibility sends no chat packet. Room visibility uses the player's normal room chat connection with a clear unit-name prefix; it does not impersonate a server pilot. Compatible receiving components restore Unicode even with their generation setting off. This is battle-room chat, not lobby, world or clan chat.

## Context and pilot profiles

The model receives the killer, victim, actual unit names, teams, alive roster, observed deaths, life index and elapsed time since entering battle. Normal battles allow repeated respawns. Deathmatch and tag modes use native stock points and unit costs to distinguish an active sortie, waiting to respawn, the last life and elimination. Unconfirmed respawn policies stay unknown. Observed deaths after joining do not constitute a complete earlier battle history.

The selected weapon is observational context, not proof of the fatal weapon. AI currently targeting a unit is also different from confirmed damage contributors. The prompt permits character-appropriate complaints about luck or numerical disadvantage, acknowledgement of teamwork and tactical suggestions, while rejecting invented battle outcomes.

Edit `personas.json` in the same Manager panel to map unit IDs or name fragments to pilots, series and personalities. Explicit IDs win. Unmapped units let the model infer an appropriate original-series voice; mass-produced units can use an ordinary soldier persona without asserting an uncertain named identity. Edit `prompt.txt` to change tone.

Check the service, local key, restart requirement, probability, cooldown and quota when no line appears. HTTP 402 is a provider billing refusal; check the DeepSeek account balance. Enabling the feature submits the selected battle fields to the configured API. Account passwords, keys, database settings and general chat history are excluded.

## Developer notes

`match_ai` exposes read-only main-thread callbacks through `sdgo/match_ai_events.h` and `battle.events.client`. The native component copies bounded event facts; Manager owns the `battle.ai_dialogue` worker. `mods/ai_dialogue/protocol.h` defines the session-, battle-, sequence- and deadline-bound bridge.

Native room chat allows 80 bytes. A narrowly recognized UTF-8 marker carries short room lines. The component reuses `shop_labels`'s `native.text_decoder` extension when available, preserving other text conversions and hook ownership. Without that component it owns the same native conversion entry directly.

The provider adapter follows the [official Chat Completions API](https://api-docs.deepseek.com/api/create-chat-completion/), disables thinking, refuses HTTP redirects, bounds responses and deadlines, and excludes provider error bodies and credentials from diagnostics.
