# Sandbox inventory (branch feature/sandbox-inventory)

One shared item-grid component (icons, hover names, search; CS2 inventory-changer look) used by:
- Vacpack editor: fix items not going into slots. The panel spans 2 columns; slots in a row on top, the item grid underneath in the same group. Click an item to fill the selected slot; drag onto a slot also works.
- Spawner (new): toys and other spawnables, placed in front of the player.
- Crafter (new): the refinery's recipes at no cost, read from the game's refinery data.
All stay behind the sandbox confirmation dialog.
