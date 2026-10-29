# License of the maps Gen_*, Hooks1, Isles2, Ring3, Sea4, Lake5, Moat6, Atolls7, Twins8

Twenty-three maps of the M.A.X.R.SI fork (`.wrl` and `.json`):

- seven large generated maps: `Gen_Archipelago`, `Gen_Coast`, `Gen_Continents`, `Gen_Gulf`, `Gen_Lakes`, `Gen_Mixed`,
  `Gen_Straits`;
- sixteen maps, each in two sizes — 112×112 and 250×250: `Hooks1_112`, `Hooks1_250`, `Isles2_*`, `Ring3_*`, `Sea4_*`,
  `Lake5_*`, `Moat6_*`, `Atolls7_*`, `Twins8_*`.

The license:

- The layout of land and water and the landing points (`.json`) are dedicated to the public domain under
  **CC0 1.0 Universal**: https://creativecommons.org/publicdomain/zero/1.0/
- The tile graphics and the palette inside the `.wrl` files are taken from `Delta.wrl` of M.A.X.R. and stay under the
  GNU GPL v2, like the rest of the game data (`data/COPYING`).

Objects, version 2 (29.10.2026): the only cells where nothing can be built are single bushes and wrecks — about 0.6%
of the land, spread evenly over every island; both landing points have the same number of them within 12 and 20 cells.
This is so on all 23 maps. Version 1 (dense jungles and ruins) is in the git history of this folder.

| Map | For |
|---|---|
| Gen_Continents, Gen_Archipelago, Gen_Straits, Gen_Lakes, Gen_Mixed | large generated maps: continents, archipelago, straits, lakes, mixed |
| Gen_Coast, Gen_Gulf | sea maps with the bases at the coast (`Gen_Coast` — no way by land) |
| Hooks1, Sea4, Moat6, Twins8 | flanking by sea |
| Ring3, Lake5 | driving around from different sides |
| Atolls7 | little land, much water |
| Isles2 | many islands for outposts |

## Лицензия карт

Двадцать три карты форка M.A.X.R.SI: семь больших сгенерированных `Gen_*` и шестнадцать карт Hooks1, Isles2, Ring3,
Sea4, Lake5, Moat6, Atolls7, Twins8, каждая в двух размерах — 112×112 и 250×250.

- Расположение суши и воды и точки высадки (`.json`) — общественное достояние, **CC0 1.0 Universal**.
- Графика плиток и палитра внутри `.wrl` взяты из `Delta.wrl` M.A.X.R. и остаются под GNU GPL v2, как остальные данные игры.

Объекты, версия 2 (29.10.2026): строить нельзя только на одиночных кустах и обломках — около 0.6% суши, поровну на каждом
острове; у обеих точек высадки их одинаково в радиусе 12 и 20 клеток. Так на всех 23 картах. Версия 1 (густые джунгли и
руины) — в истории git этой папки.

Назначение: большие сгенерированные — материки, архипелаг, проливы, озёра, смешанная (Gen_Continents, Gen_Archipelago,
Gen_Straits, Gen_Lakes, Gen_Mixed); морские с базами у берега — Gen_Coast (пути по суше нет), Gen_Gulf; обход с воды — Hooks1, Sea4, Moat6, Twins8; объезд с разных сторон — Ring3, Lake5; мало суши, много воды —
Atolls7; много островов под форпосты — Isles2.
