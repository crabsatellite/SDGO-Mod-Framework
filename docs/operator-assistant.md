# Operator supplies and manager

[简体中文（完整说明）](operator-assistant.zh-CN.md)

The **副官与掉落** (operators) panel in SDGO Mod Manager configures operator names, native or uploaded
portraits, weighted item drops, and either permanent free access for every player or registration as a
shop item. Shop listing, pricing and sales belong to the [shop catalog](shop-catalog.md).

Select the game client and the host's `StoreServer.ini`; the panel imports operator and item definitions
read-only. Connection credentials stay on the host. The Manager bundles what it needs; players receive
only the client mod/data update and use their normal launcher. The command-line tools need Python,
Pillow and pyodbc.

## Default supply officers

A fresh full import includes five permanent free supply officers for blueprints, experience packs,
cosmetics, upgrade kits and capsule coins. They keep the original branch probabilities and quantity
ranges, but each native drop now samples independently (the old script shared one roll every 30 s).
Item pools use typed server references and matching client registrations; blueprint aliases are
deduplicated by recipe and invalid outputs/materials are excluded.

The five use fixed Chinese names and skip the naming prompt; existing names are backed up first, and
other operators keep the native naming flow. Existing holdings are upgraded in place to permanent use,
keeping operator IDs, slots and equipped references. All five can equip mission parts through paired
client/server flags; the original flag is restored when the default override is disabled. Players need
the matching client DLL.

## Free access and products

- `all_players`: permanent access is granted on normal login or when the operator page opens; rentals
  are upgraded. A full 100-slot operator inventory is never cleared to make room.
- `product`: registers the operator item and description only. It does not grant, list or price anything.

Saving writes `product-registrations.json` (`provider=operator_catalog`, keys `operator:<ID>`) for the shop
module; see the Chinese guide for the format.

## Developer notes

The client mod hooks the operator GDT load and inserts new records through verified native calls, using
the native CRT allocator so the game frees them normally. Fixed names are rendered through the game's
own code-page conversion import only for verified default-officer tokens, without taking over the shop
label hook. The server adapter manages `sdgo_Operator*` tables and wraps the native operator read/drop
procedures, keeping backups and installed-definition hashes and rolling back if it finds outside changes.
Drops are sampled and delivered by the server; client configuration cannot choose its own rewards.
