# Newcomer protection

An optional, independent Battle-server Mod for a server owner's selected players. It works in missions and PvP without requiring players to maintain their own protection settings.

In Mod Manager, open **Newcomer protection**, select a Battle service, and search for players by name. Add the players to the local roster, enable protection and save. Repeat for the Battle services that should apply the policy, then restart those services. Health and ordinary weapon damage each default to **2×** and accept multipliers from 1 to 10.

The public package is disabled by default and contains an empty roster. Player identities and the owner's settings stay in each server's local `config.ini`; they are not part of the Manager programme, public releases or player content profiles. The panel's selected World INI is used only to read player names.

Protection scales the native maximum and current HP after the room's enhancement and life-budget rules. Deck units and normal respawns use the protected HP. Ordinary weapon hits retain the original per-hit input limit before applying the configured multiplier, then continue through native shields, deaths, kills and settlement. Special attacks, defense, movement and the number of lives keep their original rules. Other players and AI are unaffected. No inventory, unit definition or player database record is changed.

Changing a roster or multiplier requires restarting the corresponding Battle service. Removing the player or disabling the Mod restores native behavior on the next server start; no database rollback is needed.
