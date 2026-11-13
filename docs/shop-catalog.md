# Shop catalog manager

[简体中文（完整说明）](shop-catalog.zh-CN.md)

![Shop catalog: create an offer, adjust its price and save the draft](shop-catalog-demo.webp)

Five editable offer presets are built in and listed on first binding an empty workspace.
Owners manage their own native shop offers in the **商城商品** (shop) panel of SDGO Mod Manager:
add, search, edit, list/unlist, retire and restore. Players keep buying in the native in-game shop.
Chinese option names and rental labels come from the separate [shop labels fix](shop-labels.md).

## Installed default presets

![Five default presets and their editable sale options](shop-default-presets.webp)

After the server component is installed, first binding an empty shop workspace and WorldServer in Manager
automatically creates and lists five editable presets: a permanent unit, a rental unit, an operator, a blueprint
and a consumable. No separate example import is needed. Each inherits an existing visible single-item offer's
exact price, GB/MB and validity/quantity, retaining that offer's own native eligibility/gifting template with independently allocated identities. Source offers remain intact;
no private owner assets or IDs ship in the defaults. A missing valid source rejects the complete initialization
before product SQL is applied.

| Built-in offer name | Source selected in the destination shop |
| --- | --- |
| 预设·永久机体 (permanent unit) | An existing permanent-unit offer, retaining price, currency and permanent validity |
| 预设·租赁机体 (rental unit) | An existing rental-unit offer, retaining price, currency and rental days |
| 预设·副官 (operator) | An existing operator offer, retaining price, currency and quantity/validity |
| 预设·设计图 (blueprint) | An existing blueprint offer, retaining price, currency and quantity/validity |
| 预设·消耗品 (consumable) | An existing ETC offer, retaining price, currency and quantity/validity |

The module bundles these selection rules and initialization, rather than five fixed content assets.
Actual items and prices depend on each owner's existing stock; no maintainer-private content is required.

Edit, unlist or retire these offers in the ordinary shop panel, then save and apply. The catalog retains its
initialization version, so updates and later bindings preserve those changes. Existing nonempty catalogs are
preserved to avoid implicitly publishing author drafts. A failed initial application retains assigned identities
and reviewable drafts; use the ordinary Apply action after inspection. A configured client also receives matching
display and acquisition data through the normal owner update workflow.
Players use the original shop UI. ETC items such as coins can require the original additional quantity/extension
confirmation before the purchase is committed.

### Follow a preset to edit or add an offer

1. Install the server shop component. In Manager's **商城商品** panel, choose an empty shop workspace and
   the `WorldServer` folder, then click **保存位置设置** (save locations). The connection comes from your own
   `WorldServer.ini`; after successful initialization, all five presets appear as listed.
   An optional selected client must already have the shop labels and shop localization Mods installed.
2. Select a preset and inspect its actual item, price, currency and quantity/validity. Edit this offer,
   click **保存商品** (save offer), then **应用到游戏** (apply to game).
3. To sell a different item, select the already-registered target in the native item dropdown, enter a
   Chinese offer name and click **新增商品** (add offer). Follow the same type's preset for the sale options;
   review price, currency, validity and listing before saving and applying. The tool allocates identities.
4. Distribute matching display resources through the server owner's client package or update.
   Updated players reopen the original shop to view and purchase the offer on those terms.

These steps require no script or example ZIP. A nonempty catalog does not receive extra presets;
use step 3 to add offers there. If a source type is missing, check that server's item registrations
and native single-item templates first.

### First server component installation

When using the standalone server shop package, close the target WorldServer and run from the extracted
package root. This command uses its included empty catalog example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Server\WorldServer" -ConfigFile .\payload\shop_catalog\catalog.example.ini
```

The installer writes it to the target's `catalog.ini`; first binding in Manager generates the defaults.
For a first installation while WorldServer is running, add `-DeferActivation`: it only adds a previously
absent catalog directory, leaving loaded modules untouched. DLL activation waits for the next normal restart;
the helper can apply offers immediately.

## Everyday management

Set the existing shop workspace, the `WorldServer` folder and optionally a local client folder (for
display labels). Pick a native item and click **新增商品** (add); edit name, option label, price, GB/MB
currency, quantity or rental days and listing, then **保存商品** (save). Changes are drafts until
**应用到游戏** (apply to game). Retired offers keep their IDs and purchase history. JSON import/export
contains no credentials or local paths.

After a successful apply, the selected client receives acquisition data for actually visible unit and blueprint offers, distinguishing purchases from rentals and removing unlisted sources. Distribute that data with the client update; players with [unit acquisition sources](acquisition-sources.md) enabled can see it in the encyclopedia. This integration is optional and does not affect shop purchases when the acquisition Mod is absent.

## Example: sell an already registered Training Unit

The example below uses Manager directly. Developers who need exchangeable JSON drafts can use the optional
[four item classes and five authoring templates](../examples/shop/README.md). That tool generates unlisted drafts;
it is separate from the automatically listed built-in presets and is not required for ordinary installation.

Assume the fictional unit and its permanent item category are already applied to the same database, with matching
client models/icons. Refresh the item catalog, select that category, create “训练机使用权” with label “永久”,
price `1000 GB`, `validity=1`, and save it unlisted. Manager allocates the product ID, SKU and native template.
A unit ID is not an item-category ID, and another workspace's product identity must not be copied.

Export JSON, change this product's `price` to `1200`, and reimport in the same workspace while retaining
`catalog_id`, `key`, `product_id`, `item_id`, `template_id` and `category_id`. Review the draft, explicitly list it
and apply. Distribute client display resources separately. JSON import neither grants a unit nor installs its model.

This is same-catalog configuration exchange, not a universal product installer. Another `catalog_id` or missing
target item/template is refused: create a matching offer in that target instead of deleting identity fields or
overwriting ledgers. Missing old offers in a complete JSON import are retired with their IDs preserved; do not
reduce an export to only the product being edited. Imported operators likewise need their category applied before
an unlisted shop draft can be created. Registered imported units/operators/blueprints use existing native single-item
templates; shop configuration does not create new game resources or multi-item bundles.

## Application and limits

A listing claim needs product/subitem readback and the matching native client's real list and purchase preview.
Configuration validation, SQL or image-preview success alone does not prove that chain. GameTest's
`pages.shop.catalog` stock scope opens/cancels buy/gift previews and compares wallets/inventory; it does not
purchase or deliver. See [the native testing guide](game-test.md) and do not describe a read-only preview as a
completed purchase test.
The separate `OwnerPurchaseAssertions` scope selects `pages.shop.purchase`: one original confirmation,
real transport, post-original replies and exact native wallet debit. Independently compare actual inventory
before/after in isolated SQL and verify persistence with another login; an ACK or debit alone is insufficient.

The WorldServer module (`548A86E3-0010C000`) starts disabled until configured and is installed once with
the standalone `Install.ps1`. It updates only its own single-item offers in a transaction and keeps native
listing, billing and delivery; accounts, balances, inventory and history are untouched. Prices must be
positive; GB is native currency 1 (GP), MB native currency 2 (GC). Only the Manager publishes the
configuration; the WorldServer component applies nothing at startup.

The apply tool preserves native computed columns and their UDFs. It writes the
underlying subitems and checks the effective parent quantity, validity, price
and currency in the same transaction, without changing the database layout.

New product names and sale-option labels require Chinese text. Existing owner data remains intact;
fill missing names before editing or publishing. The separate [shop localization Mod](shop-localization.md)
provides translated descriptions and shared preview names. Its **Update Chinese search** action refreshes
only the guarded search procedure and aliases; it does not apply catalog drafts or prices.
