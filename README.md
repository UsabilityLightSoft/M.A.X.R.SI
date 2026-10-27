![maxr_icon](data/maxr.ico)
# M.A.X.R.SI

**SI** stands for **Super Intelligence**.

A fork of [M.A.X.R.](https://github.com/maxr-dot-org/maxr) (Mechanized Assault & eXploration Reloaded) with a
**computer opponent** — the AI player — and engine fixes. All credit for the game goes to the M.A.X.R. authors
([data/AUTHORS](data/AUTHORS)); the fork is made by Alexey Tkachev together with Claude (Anthropic). License — GPLv2,
as upstream ([License.md](License.md)).

## What is new

- **AI player** AI-3.2 ([passport](si/AI_PASSPORT.md)): plays a whole game through fog of war — economy, research,
  army operations, base defence, navy, aircraft (AWAC, bombers, fighters, air transport) and infiltrators. It joins a
  normal network game as one more player (`maxr_ai_client`), so you can play against it alone or with friends. "Turn
  based" mode only for now.
- **Engine fixes**, also offered upstream: free metal from factories, use of freed memory when a factory repeats its
  queue, endless turning of a shooter, crash after a cluster
  attack, factory started with nothing to build, workers counted twice, alien units outside the map, hidden units not
  detected after unloading, mines laid next to a detector, detectors waking up blind, no reaction fire on unloading,
  disabled units still scanning; faster path search and scan map checksums. Second batch (fixes 17–32): disabled units
  do not fire and do not work (a disabled constructor pauses its building), scan and sentry maps, server checks of the
  landing (one credit per started 5 units of cargo), of big buildings, of transfers and of four more actions, non-latin
  paths and user names, the path search, a timer race at the end of a game; a detector searches by itself, a vehicle
  and an action wait for the step of an invisible unit; disabled aliens recover; two rules of the fork: a draw at the
  same best score (as in M.A.X.) and a stolen vehicle noticed at the next turn of its owner.
- **Seven large generated maps** up to 250×250 (`Gen_*`): continents, archipelago, straits, lakes, mixed and two sea
  maps with the bases at the coast (`Gen_Coast` — no way by land, `Gen_Gulf`).
- `ai_headless` — AI against AI without a window (deterministic, for tournaments and training data).

## Play on Windows

Download `M.A.X.R.SI-…-windows-x64.zip` from [Releases](../../releases), unzip it to a folder **without non-Latin
letters in the path**, run `Играть против ИИИ.cmd` ("Play against the AI"), start a game and choose "Turn based".

## Build

As upstream (see below); extra targets: `ai_headless`, `maxr_ai_client`. Branches: `main` — M.A.X.R.SI, `develop` —
a copy of upstream `develop`.

## По-русски

**SI** — Super Intelligence (Супер Интеллект).

M.A.X.R.SI — форк ремейка M.A.X.R. с **компьютерным противником (ИИИ)** и исправлениями движка. ИИИ играет целую
партию сквозь туман войны и входит в обычную сетевую игру как ещё один игрок — можно играть против него одному или с
друзьями (пока только режим «Turn based»). Скачать — [Releases](../../releases), распаковать в папку **без русских букв
в пути**, запустить «Играть против ИИИ.cmd». Что умеет ИИИ — [паспорт](si/AI_PASSPORT.md).

В ветке `main` сейчас — ИИИ-3.2: к хозяйству, армии, обороне и флоту добавлены авиация (AWAC, бомбардировщики,
истребители, авиатранспорт) и коммандос; чемпион по-прежнему поколение 3. Движок — правки 01–16 и вторая пачка 17–32:
выключенный не стреляет и не работает (конструктор ставит стройку на паузу), карты обзора и дозора, сервер проверяет
высадку (кредит за каждые начатые 5 единиц груза), большие здания, передачу и ещё четыре действия, пути и имя
пользователя с русскими буквами, поиск пути, гонка таймера; сыщик ищет своим зрением; машина и действие ждут шага
невидимого; выключенные пришельцы просыпаются; правила форка — ничья при равенстве очков и угон, замеченный в начале
хода хозяина. Карты — семь больших сгенерированных, из них две морские с базами у берега. Исправления движка
предложены авторам M.A.X.R.

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
