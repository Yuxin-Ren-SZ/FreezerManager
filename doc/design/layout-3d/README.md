# 3D freezer layout: CryoCube F740 demo

This is a standalone, interactive figure of the **CryoCube F740 series ULT
freezer**. It is the first catalogued model for the 3D Layout page proposed in
[#215](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/215) and
[ADR 0002](../../adr/0002-freezer-3d-layout-api.md).

Open `cryocube-f740.html` in a browser. It is one self-contained file with no
network access.

**`cryocube-f740-clean.html`** is the same freezer with nothing else on the page. It has no wobble, see-through,
labels, readouts, notices or page controls. Only the three buttons below remain, plus clicking parts and dragging to turn.
The full page shows the same view with `?clean=1`.

| Door open, middle compartment, rack 4 out | See-through: cascade refrigeration |
|---|---|
| ![Open](cryocube-f740-open.png) | ![See-through](cryocube-f740-see-through.png) |

| Control | What it does |
|---|---|
| **Open door / Close door** | Swings the outer door open on its right-hand hinges. Closing pushes any rack back in, shuts the inner door, then shuts the outer door. |
| **Inner door ▾** | Opens the Top, Middle or Bottom compartment's inner door, one at a time. It opens the outer door first if needed. |
| **Pull rack ▾** | Slides rack 1 to 6 of the open compartment toward you, showing its 8 x 4 boxes. It opens the doors first if needed. |
| Click a part | The handle toggles the door, an inner door toggles its compartment, and a rack pulls out or pushes back. |
| Drag, arrow keys | Turn the freezer all the way round. **Front view** resets the view. |
| **See-through** | Shows the cascade refrigeration system with labels: two compressors, condenser and fans, cascade heat exchanger, driers, expansion tank, evaporator coils, vacuum insulation, frame heater and wiring. |

The **Cabinet** readout is the cabinet temperature, sped up for the demo:
- With only the outer door open, the cabinet holds at -80 °C, because the inner doors keep the cold in.
- With an inner door open as well, it climbs, and the touchscreen alarms above -70 °C.
- After the door closes, the compressors pull it back down.

**Rack out** shows the location path the scene API returns (compartment, then rack).

URL parameters for screenshots: `?door=1`, `?inner=0|1|2`, `?rack=1..6`,
`?pull=0..1`, `?temp=-72`, `?xr=1&labels=1`, `?yaw=200&pitch=40`.

## Geometry (cm, from the Eppendorf data sheet)

| Item | Size |
|---|---|
| Outside (W x D x H) | 109.9 x 98 x 197.3 |
| Inside | 86.5 x 62.1 x 139 (740 L) |
| Compartments | 3, about 45.7 high, separated by 2 shelves |
| Racks | 6 per compartment, 13.5 x 56 x 43, 8 tiers x 4 boxes deep (576 two-inch boxes) |

These match the `FreezerModel` / `CompartmentSpec` seed entry in the ADR.

## Files

- `cryocube-f740.html` is the built, self-contained page to open.
- `cryocube-f740-clean.html` is the clean build (`window.CF_CLEAN = true` set before the figure script).
- `cryocube-f740.src.html` is the figure source for both. It expects the Colorfigures engine `core.js` beside it.
  Rebuild with the Colorfigures skill's `scripts/build.py cryocube-f740.src.html cryocube-f740.html`.
  For the clean file, insert `<script>window.CF_CLEAN = true;</script>` before `<title>` in that output.

The page is drawn with the Colorfigures isometric engine, credited in the page footer.
The production web view will reimplement the renderer under `src/web/` (slice 3 in the ADR).
This demo is a design reference, not shipped code.
