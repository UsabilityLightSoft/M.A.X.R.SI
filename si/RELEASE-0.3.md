# M.A.X.R.SI 0.3 — AI player SI-R5.3-T

A fan fork of the open-source M.A.X. remake ([maxr-dot-org/maxr](https://github.com/maxr-dot-org/maxr)) with a computer opponent built in. License: GPLv2.

## New in 0.3

- **Sound no longer turns off forever after a failed audio start.** If the audio device was busy or unplugged at one start, the game used to latch sound off permanently and the preferences dialog had no way to bring it back. Now the failure is not saved: the next start simply tries again.
- **AI player «SI-R5.3-T»** (generation 5, build 3): a bulldozer at the base and with the army clears the rubble that blocks building sites and gathers metal from the battlefield for repairs and reloads. A new engine order «Clear» drives it. Strength as `SI-R5.2-T` (against `SI-R5`: full D 0.522 / material 0.520; champion `base5` beats the previous champion `SI-R4` D = 0.738 by the game verdict / 0.678 by material).
- Carried over from 0.2: 23 maps (16 new + 7 generated, CC0 layouts), engine fixes up to 51, 11 own CC0 sounds.

## Files

| File | Size | SHA-256 |
|---|---|---|
| M.A.X.R.SI-0.3-windows-x64.zip | 31 209 385 | 05d32ca74e3e34ef320dea2e94d27afe1037f112e59cdd5e78b0f8238776f4e3 |

Unpack and run `maxr.exe` (portable mode). How to play against the AI: «КАК ИГРАТЬ ПРОТИВ ИИИ.html» inside. The rest of the sounds and music are installed from your own copy of M.A.X. with the shortcut «Поставить звуки и музыку из M.A.X..cmd».

Source: tag `si-0.3` of this repository. Licenses: GPLv2 (code), CC0 (map layout, our sounds), CC BY 4.0 (emblem) — see `data/maps/MAPS_LICENSE.md`, `data/sounds/SOUNDS_LICENSE.md`, `licenses/`.

Thanks to the M.A.X.R. team for the remake and for merging the first batch of our fixes.

---

# M.A.X.R.SI 0.3 — ИИ-противник SI-R5.3-T

Форк открытого ремейка M.A.X. ([maxr-dot-org/maxr](https://github.com/maxr-dot-org/maxr)) со встроенным компьютерным противником. Лицензия: GPLv2.

## Что нового с 0.2

- **Звук больше не пропадает навсегда после сбоя аудиоустройства.** Если устройство было занято или отключено при запуске, раньше игра навсегда выключала звук, и в настройках не было способа вернуть его. Теперь сбой не сохраняется: следующий запуск просто пробует снова.
- **ИИ-противник «SI-R5.3-T»** (поколение 5, сборка 3): бульдозер у базы и с армией расчищает обломки, что мешают стройке, и собирает металл с поля боя — на ремонт и перезарядку. Им управляет новый приказ движка «Clear». По силе — как `SI-R5.2-T` (против `SI-R5`: full D 0.522 / по материалу 0.520; чемпион `base5` бьёт прежнего чемпиона `SI-R4` D = 0.738 по вердикту игры / 0.678 по материалу).
- Перешло из 0.2: 23 карты (16 новых + 7 сгенерированных, расположение CC0), правки движка до 51, 11 собственных звуков CC0.

## Файлы

| Файл | Размер | SHA-256 |
|---|---|---|
| M.A.X.R.SI-0.3-windows-x64.zip | 31 209 385 | 05d32ca74e3e34ef320dea2e94d27afe1037f112e59cdd5e78b0f8238776f4e3 |

Распакуйте и запустите `maxr.exe` (переносной режим). Как играть против ИИИ — «КАК ИГРАТЬ ПРОТИВ ИИИ.html» внутри. Остальные звуки и музыка ставятся из вашей копии M.A.X. ярлыком «Поставить звуки и музыку из M.A.X..cmd».

Исходники: метка `si-0.3` этого репозитория. Лицензии: GPLv2 (код), CC0 (расположение карт, наши звуки), CC BY 4.0 (эмблема) — см. `data/maps/MAPS_LICENSE.md`, `data/sounds/SOUNDS_LICENSE.md`, `licenses/`.

Спасибо команде M.A.X.R. за ремейк и за слияние первой пачки наших правок.
