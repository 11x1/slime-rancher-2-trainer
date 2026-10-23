# HUD edit mode (branch feature/hud-edit-mode)

Shared edit mode for the minimap and the Speed Panel (momentum HUD).
- A "Move" button in F8 replaces the position/size sliders. Click: F8 closes, the element gets a long-dashed, padded outline, and you drag it with the mouse.
- Position saved in percent: 0..100% where 100% = screen size minus element size, clamped. Migrate old px settings once.
- Resize by corners (opposite corner stays fixed); Alt resizes from the center. Size also in percent.
- Shift snaps the side gap equal to the top/bottom gap (the inverse when dragging mostly vertically).
- Snap to horizontal screen center, with a long-dashed vertical guide line while snapped.
- Enter or a click outside keeps the change; Esc cancels.
- F8 keeps only style settings (minimap: shape, opacity).
