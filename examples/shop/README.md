# Shop import templates: four item classes, five authoring examples

[简体中文](README.zh-CN.md) · [Shop operations](../../docs/shop-catalog.md)

This directory contains neutral recipes and executable authoring code, not a server's offers, unit assets or
allocated IDs. `templates.json` covers permanent units, rental units, operators, blueprints and consumables.
The builder selects already-visible, validly priced native single-item options in the target snapshot and retains
their price, GB/MB currency and validity/quantity. The existing shop SDK allocates fresh offer identities.
Every added example has `enabled=false`. Generation executes no SQL, grants nothing and changes no live sale plan.

Independent recipes are also available for [permanent units](recipes/unit-permanent.json),
[rental units](recipes/unit-rental.json), [operators](recipes/operator.json), [blueprints](recipes/blueprint.json)
and [consumables](recipes/consumable.json). Use `--recipes examples/shop/recipes/blueprint.json` for just one;
otherwise generate the five cumulative steps. A newly imported item without a visible priced source needs explicitly
authored/reviewed terms in Shop. This conservative example never guesses its price or lists it automatically.

## Generate target-compatible import files

Use the existing framework source tools to read your own non-player configuration snapshot. Paths below are
placeholders. The input `shop.json` must be the current catalog of the destination workspace:

```powershell
python tools/shop_catalog.py snapshot --server-ini "D:\Server\WorldServer\WorldServer.ini" --output "D:\PrivateShop\snapshot.json"
python examples/shop/build_examples.py --snapshot "D:\PrivateShop\snapshot.json" --catalog "D:\MyShopWorkspace\shop.json" --output "D:\PrivateShop\new-examples"
```

No original-client download, database restore or account creation is involved. Snapshots and resolved outputs
contain target definitions and stay in the owner's workspace. Only neutral recipes, code and documentation belong
in framework source. Output must be new. A missing item class refuses the whole generation instead of guessing
IDs/prices, creating free offers or overwriting authored output.

## Output

```text
new-examples/
  examples.json
  01-unit-permanent/
  02-unit-rental/
  03-operator/
  04-blueprint/
  05-consumable/
  *.shop-authoring.zip
```

Each directory contains a complete `shop.json`, native `catalog.ini`, source-option `selection.json`, hash-bound
`shop-example.json`, and a README. This authoring manifest is not the player installer's `manifest.json`.
Every step preserves the prior catalog and adds one disabled example; later files also retain earlier examples.
Import the last file for all five drafts, or inspect them in order. After further owner edits, regenerate from
the latest workspace catalog into a new output. Do not reimport stale catalogs or delete allocated identities to
reuse a sample key; existing example keys are refused.

## Import and explicitly publish

1. Unzip the author archive and import its `shop.json` using Shop's **导入配置** in the same workspace, not Game
   Features' **导入配置包**.
2. Confirm existing offers are unchanged, all examples disabled, and terms match `selection.json`.
3. For a listing test, explicitly enable one example in an isolated environment, save/apply, and read back the real
   product, subitem, price and currency.
4. Reopen the native shop; inspect its name, icon, price and validity, open buy/gift previews, then cancel.
   This proves list/preview behavior, not charging/delivery. A real purchase needs a separate isolated test.
5. Unlist/apply and verify disappearance, retaining identities and player history.

Manager separately exports the player display-update ZIP after application. That installs client programmes and
display data; it neither lists offers nor executes server SQL. The author archive is not a player runtime package.

Production can retain templates/disabled drafts without adding live products. Updating template files requires no
price, odds, reward or runtime-switch changes and no service restart. For a native-preview crash, record framework
version, product/option identities, exact trigger and logs; inspect target item/client-resource pairing instead of
clearing shop/player databases or disabling every Mod as a substitute for diagnosis.
