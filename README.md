![maxr_icon](data/maxr.ico)
# M.A.X.R.SI

**SI** stands for **Super Intelligence**.

A fork of [M.A.X.R.](https://github.com/maxr-dot-org/maxr) (Mechanized Assault & eXploration Reloaded) with a
**computer opponent** — the AI player — and engine fixes. All credit for the game goes to the M.A.X.R. authors
([data/AUTHORS](data/AUTHORS)); the fork is made by Alexey Tkachev together with Claude (Anthropic). License — GPLv2,
as upstream ([License.md](License.md)).

## What is new

- **AI player** `SI-R5.1-T` ([passport](si/AI_PASSPORT.md)): plays a whole game through fog of war — chooses its clan
  by the map, re-examines all its orders every turn (after loading a saved game it decides anew at once), economy with
  eco-spheres (a draw at the turn limit becomes a win on points), a smart surveyor, research,
  army operations with hit-and-run tactics, base defence against early rushes and raids, navy with resupply at sea and
  sea mines, aircraft (AWAC, bombers, fighters, air transport) and infiltrators.
  It joins a normal network game as one more player (`maxr_ai_client`), so you can play against it alone or with
  friends. "Turn based" mode only for now; in the lobby it says in the chat why it is not ready and what to press.
- **Speaking names of AI versions:** `SI-<type><generation>.<build>-<training>[+<advisor>]` — `SI-R5.1-T` is rules (R),
  generation 5 (the 5th champion), build 1, tuned by tournaments (T); the frozen champions are `SI-R1` … `SI-R5`, with
  an LLM advisor — `SI-R5.1-T+Lgemma4-12b`; the former AI-3.1 and AI-3.2 are `SI-R3.1-T` and `SI-R3.2-T`. The name is
  the player name in the lobby and the greeting in the game chat (with the link to the passport), and it is written to
  the journal and the metrics of every game. The full key — in the [passport](si/AI_PASSPORT.md).
- **Engine fixes**, also offered upstream: free metal from factories, use of freed memory when a factory repeats its
  queue, endless turning of a shooter, crash after a cluster
  attack, factory started with nothing to build, workers counted twice, alien units outside the map, hidden units not
  detected after unloading, mines laid next to a detector, detectors waking up blind, no reaction fire on unloading,
  disabled units still scanning; faster path search and scan map checksums. Second batch (fixes 17–32, to be offered
  upstream): disabled units do not fire and do not work (a disabled constructor pauses its building), scan and sentry maps, server checks of the
  landing (one credit per started 5 units of cargo), of big buildings, of transfers and of four more actions, non-latin
  paths and user names, the path search, a timer race at the end of a game; a detector searches by itself, a vehicle
  and an action wait for the step of an invisible unit; disabled aliens recover; two rules of the fork: a draw at the
  same best score (as in M.A.X.) and a stolen vehicle noticed at the next turn of its owner. New: a resync restores the
  client model in full — the cargo of transports and depots, the units of a live client, the unit versions after
  research (fix 33); a saved game without GUI info no longer stops the dedicated server (fix 36); the self destruction
  of a land or sea mine no longer crashes the game (fix 37). Fixes 34–35 and 38–46 (not offered upstream yet): the game
  in a normal window scaled to the whole window, an interface scale "Auto" or 100–400% (4K — 300%), Russian strings of
  the menus and the game; the unit window shows a started building at once; resinstaller installs the hit sounds; the
  factory window keeps "Done" unavailable while the queue is empty; a warning when a network save is loaded as a single
  game; the key "f" only centres, "Colour" is "o"; with the AI in the lobby only "Turn based" can be chosen; a network
  client out of sync for 3 s takes the model from the server and logs the parts of its checksum, and it creates the
  players after the unit data (no "OUT OF SYNC" before the last landing).
- **Seven large generated maps** up to 250×250 (`Gen_*`): continents, archipelago, straits, lakes, mixed and two sea
  maps with the bases at the coast (`Gen_Coast` — no way by land, `Gen_Gulf`).
- **Sixteen more maps, 112×112 and 250×250** (`Hooks1`, `Isles2`, `Ring3`, `Sea4`, `Lake5`, `Moat6`, `Atolls7`, `Twins8`):
  flanking by sea, driving around from different sides, little land and much water, many islands for outposts; layouts
  and landing points — CC0 1.0 (`data/maps/MAPS_LICENSE.md`).
- `ai_headless` — AI against AI without a window (deterministic, for tournaments and training data).

## Play on Windows

Download `M.A.X.R.SI-…-windows-x64.zip` from [Releases](../../releases), unzip it to a folder **without non-Latin
letters in the path**, run `Играть против ИИИ.cmd` ("Play against the AI"), start a game and choose "Turn based" (the
AI tells in the lobby chat what to switch, if it is not ready).

## Build

As upstream (see below); extra targets: `ai_headless`, `maxr_ai_client`. Branches: `main` — M.A.X.R.SI, `develop` —
a copy of upstream `develop`.

## По-русски

**SI** — Super Intelligence (Супер Интеллект).

M.A.X.R.SI — форк ремейка M.A.X.R. с **компьютерным противником (ИИИ)** и исправлениями движка. ИИИ играет целую
партию сквозь туман войны и входит в обычную сетевую игру как ещё один игрок — можно играть против него одному или с
друзьями (пока только режим «По очереди»; если ИИИ не готов, он пишет в чат лобби почему и что нажать). Скачать —
[Releases](../../releases), распаковать в папку **без русских букв в пути**, запустить «Играть против ИИИ.cmd». Что
умеет ИИИ — [паспорт](si/AI_PASSPORT.md).

В ветке `main` — ИИИ `SI-R5.1-T`, поколение 5, сборка 1: пересматривает приказы каждый ход, после загрузки сохранения
решает заново; по силе — как `SI-R5` (чемпион `base5`: против прежнего чемпиона `SI-R4` D = 0.738 по вердикту игры и
0.678 по материалу). Сам выбирает клан по карте (обычно Musashi — танки; враг за водой и моря у базы нет — The
Chosen, авиация); тактика заходов: раненая машина, которой не пережить ход врага, стреляет и отходит, дальнобойного врага
не ждёт у края его огня, урон «из ниоткуда» — ищет подлодку или диверсанта, по выключенным соседям бьёт наугад; ранний
раш отбивает танк-защитник; армаду за водой бьют бомбардировщики с подлётом вне досягаемости ПВО; осторожный геолог и
зенитки у дальних шахт; у свежей верфи — сначала морские мины. С поколения 4 — эко-сферы и победа по очкам, умный геолог,
снабжение флота в море, AWAC в укрытии. **Имена версий говорящие:**
`SI-<тип><поколение>.<сборка>-<обучение>[+<советник>]` — `SI-R5.1-T`: правила (R), поколение 5, сборка 1, настроен
турнирами (T); замороженные чемпионы — `SI-R1` … `SI-R5`, с советником-LLM — `SI-R5.1-T+Lgemma4-12b`; прежние «ИИИ-3.1» и
«ИИИ-3.2» — `SI-R3.1-T` и `SI-R3.2-T`. Имя видно в лобби (имя игрока), в приветствии в чате партии (со ссылкой на
паспорт), в журнале и метриках каждой партии; расшифровка — в паспорте. Движок — правки 01–16, вторая пачка 17–32 и
новые 33–46: выключенный не стреляет и не работает (конструктор ставит стройку на паузу), карты обзора и дозора,
сервер проверяет высадку (кредит за каждые начатые 5 единиц груза), большие здания, передачу и ещё четыре действия, пути
и имя пользователя с русскими буквами, поиск пути, гонка таймера; сыщик ищет своим зрением; машина и действие ждут шага
невидимого; выключенные пришельцы просыпаются; правила форка — ничья при равенстве очков и угон, замеченный в начале
хода хозяина; пересинхронизация восстанавливает модель клиента целиком (33); сохранение без сведений окна не
останавливает выделенный сервер (36); самоуничтожение мины не роняет игру (37); игра в обычном окне и масштаб интерфейса
«Авто» или 100–400% (34, 40), русские строки меню и партии (35), окно юнита сразу показывает стройку (38), звуки
попаданий (39), на заводе «Готово» недоступна при пустой очереди (41), предупреждение при одиночной загрузке сетевой
партии (42), «Лёгкий завод» (43), «f» — только центр, «Цвет» — «o» (44), в лобби с ИИИ — только «По очереди» (45),
клиент при рассинхроне дольше 3 с сам берёт модель сервера и создаёт игроков после данных юнитов (46). Карты — семь
больших сгенерированных, из них две морские с базами у берега; ещё 16 карт 112×112 и 250×250 (обход с воды, объезд с разных сторон, мало суши, острова под форпосты) — расположение суши и точки высадки под CC0 1.0. Авторам M.A.X.R. предложены правки 01–12, 15 и 16,
остальные — на очереди.

---

*Upstream README follows.*

# MAXR

## Introduction

Mechanized Assault & eXploration Reloaded

A turn based strategy game based on [M.A.X.](https://en.wikipedia.org/wiki/Mechanized_Assault_%26_Exploration) created by fans as a community to make it's unique multiplayer experience possible on modern networks and systems.

# Building MAXR

## Supported Platforms

* Mac OS X ![Mac-OS-badge](https://github.com/MAX-Reloaded/maxr/actions/workflows/macos.yml/badge.svg)
* MS Windows ![Premake5-windows-badge](https://github.com/MAX-Reloaded/maxr/actions/workflows/premake5-windows.yml/badge.svg)
* Linux ![Ubuntu-badge](https://github.com/MAX-Reloaded/maxr/actions/workflows/ubuntu.yml/badge.svg)

Other UNIX-like operating systems may work too out of the box. Chances are huge if there's a build of SDL2 and CMake for the system.

You can find more information about how to build maxr in our Wiki:  
[How to build](https://github.com/maxr-dot-org/maxr/wiki/How-to-build)

# Playing with original graphics and sounds

Due the copyright we are not allowed to provide any game content
from the original Mechanized Assault and Exploration (c) 1996 Interplay

If you do own an original Mechanized Assault & Exploration CD
(c) 1996 Interplay you can however make use of the tool [resinstaller](resinstaller)
and install the original gfx automagically from your existing M.A.X. copy.

Resinstaller also takes care of sounds, videos, maps and music.

If you don't own an original M.A.X. CD but you want to play with the
original look'n'feel you can still buy it at Gog.com  
https://www.gog.com/game/m_a_x_m_a_x_2

## Additional important files

Please find additional files in `./data`:

- [ABOUT](data/ABOUT)
- [AUTHORS](data/AUTHORS)
- [MANUAL](data/MANUAL)
