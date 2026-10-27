# AI player passport — SI-R4.0-T (M.A.X.R.SI)

*Русский текст — ниже.*

The AI player has a **speaking name**: `SI-<type><generation>.<build>-<training>[+<advisor>]`, in Latin letters (game
lobby, file names, GitHub). Generation N is the N-th "champion" — a frozen version that beat the previous champion in a
tournament; the build counts improvements after it. The name is shown as the player name in the game lobby, in the
greeting in the game chat, in the journal (event `aiVersion`) and in `metrics.json` of every game.

| Part | Values |
|---|---|
| Type | `R` — rules (an algorithm); `N` — an own network on the PC; `H` — hybrid: an own network decides, the rules execute; `L` — a language model on the PC; `C` — an online model; `X` — not an AI version: raiders and test scenarios (`SI-X-red1`, `SI-X-red2`, `SI-X-scenario`) or an external program (`SI-X-external`) |
| Generation.build | generation — the number of the champion (beat the previous one, D ≥ 0.53); build — improvements between champions |
| Training — stages in order, each with its number of games | `T` — tuned by tournaments, did not learn by itself; `I40k` — imitated the AI on 40 thousand games; `S300k-g12` — self-play: 300 thousand games, 12 league generations; `P1k` — learned on 1 thousand human games; `F` — fine-tuning of a language model |
| Advisor (if any) | `+L<model>` (a local model, LM Studio) / `+C<model>` (online): `+Lgemma4-12b` |
| `~x` at the end | experiment switches of the rules are set (`MAXR_NAVY=0` and others): not the version of this passport; the journal lists them |

Examples: now `SI-R4.0-T`; with the advisor — `SI-R4.0-T+Lgemma4-12b`; the frozen champions — `SI-R1` … `SI-R4`; a
future tactics network after imitation — `SI-H5.0-I40k`.

| | |
|---|---|
| Version | **SI-R4.0-T** — brain `rule`: generation 4 (champion `base4`, frozen 27.10.2026; `rule` plays the same, move for move), build 0, tuned by tournaments |
| Made of | hand-written rules only: strategy → operations → micro; deterministic |
| Neural networks | none yet (planned: own small tactics and strategy nets trained on AI games) |
| LLM | not used by default; optional advisor brain `llm` (a local model via an OpenAI-compatible API) — `SI-R4.0-T+L<model>` |
| Learned from | nothing learned; rules tuned by AI-vs-AI tournaments; 0 human games so far |
| Plays | through fog of war, by the same rules as a human; "Turn based" mode only; joins a normal network game as a player (`maxr_ai_client`); in the lobby it says in the chat why it is not ready and what to press |
| Tested on | 5 remake maps (Mushroom, Lava, Delta, Donuts, Three Isles), 5 generated maps up to 250×250 (`Gen_*`) and 2 generated sea maps with bases at the coast (`Gen_Coast`, `Gen_Gulf`) |

**Can:** economy (mines on resources, factories by income), research and gold upgrades, army operations (gather → march
→ search → strike → retreat), unit roles (scouts, anti-air, repair, artillery behind tanks), memory of the enemy and threat
map, several passes per turn (move, shoot, finish off), base defence against raids (several radars, anti-air ring, guards
against infiltrators, mines, ring of connectors), navy (shipyard, corvettes, gunboats, missile ships, submarines, sea
landing, dock), coastal missile defence, choosing land / sea / air emphasis from the map; aircraft (AWAC, bombers "hit
and leave", fighters, air transport); infiltrators; APC under water. **New in generation 4:** eco-spheres and the points
victory (builds and guards eco-spheres by the victory condition of the game, so a draw at the turn limit becomes a win on
points); a smart surveyor (rings around the base in strips of 3, follows a found vein, keeps away from the enemy base,
escapes scouts over water); resupply of the fleet at sea (a cargo ship rearms the ships, empty ships go to it or to the
dock); an early shipyard when there is no way by land; sea mines at the own shipyard; the AWAC hides after scouting
(losses 77% → 22%) and scouts early when the enemy is not found; infiltrators train up to rank 3 and then go to work
instead of endless training on aliens.

**Cannot yet:** clan choice, simultaneous turns.

**Engine of this build** (also offered upstream): fixes 01–33, 36, 37. New since `SI-R3.2-T`: 33 — a resync restores the
client model in full (the cargo of transports and depots, the units of a live client, the unit versions after research);
36 — a saved game without GUI info (all autosaves of AI network games) no longer stops the game thread of the dedicated
server; 37 — the self destruction of a land or sea mine no longer crashes the game. Earlier: disabled units do not fire
and do not work (a constructor pauses), scan and sentry maps, server checks of the landing (one credit per started 5 units
of cargo), of big buildings, of transfers and of four more actions, non-latin paths and user names, the path search, a
timer race; a detector searches by itself; a vehicle and an action wait for the step of an invisible unit; a draw at the
same best score; a stolen vehicle is noticed at the next turn of its owner; disabled aliens recover.

**Lineage**

| Name | Was called | Brain | Frozen | Strength |
|---|---|---|---|---|
| `SI-R1` | generation 1 | `base` | 24.10.2026 | rules with economy, research and roles of that day |
| `SI-R2` | generation 2 | `base2` | 26.10.2026 | against `SI-R1`: 200 wins of 200 (D = 0.909) |
| `SI-R3` | generation 3 | `base3` | 26.10.2026 | against `SI-R2`: D = 0.565 (93 wins, 62 losses, 45 draws) |
| `SI-R3.1-T` | AI-3.1 (release `si-0.1`) | `rule` | — | against `SI-R3`: D = 0.489 — equal on land, adds a navy |
| `SI-R3.2-T` | AI-3.2 | `rule` | — | against `SI-R3`: D = 0.505 on 10 maps, 0.514 with the 2 sea maps — adds aircraft and infiltrators |
| `SI-R4` = `SI-R4.0-T` | — | `base4` = `rule` | 27.10.2026 | against `SI-R3`: D = 0.736 on 10 maps, 0.752 with the 2 sea maps by the game verdict (points first); by material — 0.610 and 0.616 |

D is the share of strength in a tournament on both sides of each map (0.5 — equal; ≥ 0.53 — stronger). `SI-R3` builds
no eco-spheres, so against it the verdict by points is too generous; the verdict by material (as before generation 4) is
the strict measure.

---

# Паспорт ИИИ — SI-R4.0-T (M.A.X.R.SI)

ИИИ (ИИ Игрока) носит **говорящее имя**: `SI-<тип><поколение>.<сборка>-<обучение>[+<советник>]`, латиницей (лобби
игры, имена файлов, GitHub). Поколение N — N-й «чемпион», замороженная версия, которая обыграла прежнего чемпиона в
турнире; сборка — улучшения после него. Имя видно в лобби игры (имя игрока), в приветствии в чате партии, в журнале
(событие `aiVersion`) и в `metrics.json` каждой партии.

| Часть | Значения |
|---|---|
| Тип | `R` — правила (алгоритм); `N` — своя сеть на ПК; `H` — гибрид: своя сеть решает, правила исполняют; `L` — языковая модель на ПК; `C` — модель онлайн; `X` — не версия ИИИ: налётчики и сценарии проверки (`SI-X-red1`, `SI-X-red2`, `SI-X-scenario`) или внешняя программа (`SI-X-external`) |
| Поколение.сборка | поколение — номер чемпиона (обыграл прежнего, D ≥ 0.53); сборка — улучшения между чемпионами |
| Обучение — этапы по порядку, у каждого число партий | `T` — настроен турнирами, сам не учился; `I40k` — подражал ИИИ на 40 тыс. партий; `S300k-g12` — самоигра: 300 тыс. партий, 12 поколений лиги; `P1k` — учился на 1 тыс. партий людей; `F` — дообучение языковой модели |
| Советник (если есть) | `+L<модель>` (модель на ПК, LM Studio) / `+C<модель>` (онлайн): `+Lgemma4-12b` |
| `~x` в конце | заданы выключатели опытов правил (`MAXR_NAVY=0` и другие): это не версия из паспорта; какие — в журнале |

Примеры: сейчас `SI-R4.0-T`; с советником — `SI-R4.0-T+Lgemma4-12b`; замороженные чемпионы — `SI-R1` … `SI-R4`; будущая
сеть-тактик после подражания — `SI-H5.0-I40k`.

| | |
|---|---|
| Версия | **SI-R4.0-T** — мозг `rule`: поколение 4 (чемпион `base4`, заморожен 27.10.2026; `rule` играет так же, ход в ход), сборка 0, настроен турнирами |
| Из чего | только правила, написанные вручную: стратегия → операции → микро; детерминирован |
| Нейросети | пока нет (в плане — свои маленькие сети тактики и стратегии, обученные на партиях ИИИ) |
| LLM | по умолчанию нет; по желанию — мозг-советник `llm` (локальная модель через API, совместимый с OpenAI) — `SI-R4.0-T+L<модель>` |
| На чём учился | ничему не учился; правила настроены турнирами ИИИ против ИИИ; партий с человеком — 0 |
| Как играет | сквозь туман войны, по тем же правилам, что человек; только режим «По очереди»; входит в обычную сетевую игру как игрок (`maxr_ai_client`); в лобби говорит в чат, почему не готов и что нажать |
| Где проверен | 5 карт ремейка (Mushroom, Lava, Delta, Donuts, Three Isles), 5 сгенерированных карт до 250×250 (`Gen_*`) и 2 сгенерированные морские карты с базами у берега (`Gen_Coast`, `Gen_Gulf`) |

**Умеет:** хозяйство (шахты на ресурсах, фабрики по доходу), исследования и улучшения за золото, операции армии
(сбор → поход → поиск → удар → отход), роли машин (скауты, ПВО, ремонт, артиллерия за танками), память о враге и карта
угроз, несколько заходов за ход (подъехал, выстрелил, добил), оборона базы от налёта (несколько радаров, кольцо ПВО,
охрана от диверсантов, мины, соединители кольцом), флот (верфь, корветы, канонерки, ракетные корабли, подлодки, морской
десант, док), ракетная оборона берега, выбор упора «суша / море / воздух» по карте; авиация (AWAC, бомбардировщики
«ударил — ушёл», истребители, авиатранспорт); коммандос; БМП под водой. **Новое в поколении 4:** эко-сферы и победа по
очкам (строит и охраняет эко-сферы по условию победы партии — ничья на пределе ходов становится победой по очкам); умный
геолог (кольца вокруг базы полосой 3, доразведка найденной жилы, к базе врага не ходит, от скаутов уходит по воде);
снабжение флота в море (грузовое судно перезаряжает корабли, пустые идут к нему или в док); ранняя верфь, когда пути по
суше нет; морские мины у своей верфи; AWAC после разведки прячется (потери 77% → 22%) и раньше идёт на разведку, если
враг не найден; коммандос тренируются до звания 3 и идут в дело, а не тренируются на пришельцах без конца.

**Пока не умеет:** выбор клана, одновременные ходы.

**Движок этой сборки** (предложено и авторам): правки 01–33, 36, 37. Новое после `SI-R3.2-T`: 33 — пересинхронизация
восстанавливает модель клиента целиком (груз транспортов и хранилищ, юниты живого клиента, версии юнитов после
исследований); 36 — сохранение без сведений окна (так у всех автосохранений сетевых партий ИИИ) больше не останавливает
поток игры выделенного сервера; 37 — самоуничтожение наземной или морской мины больше не роняет игру. Прежние: выключенный
не стреляет и не работает (конструктор ставит стройку на паузу), карты обзора и дозора, сервер проверяет высадку (кредит
за каждые начатые 5 единиц груза), большие здания, передачу и ещё четыре действия, пути и имя пользователя с русскими
буквами, поиск пути, гонка таймера; сыщик ищет своим зрением; машина и действие ждут шага невидимого; ничья при равенстве
очков; угон замечают в начале хода хозяина; выключенные пришельцы просыпаются.

**Родословная** — таблица выше: `SI-R1` (`base`, 24.10.2026) → `SI-R2` (`base2`, 200 побед из 200 над первым) → `SI-R3`
(`base3`, D = 0.565 над вторым) → `SI-R3.1-T` (прежнее имя «ИИИ-3.1», выпуск `si-0.1`: на равных с третьим на суше, плюс
флот) → `SI-R3.2-T` (прежнее «ИИИ-3.2»: D = 0.505 против третьего на 10 картах и 0.514 с двумя морскими, плюс авиация и
коммандос) → `SI-R4` = `SI-R4.0-T` (`base4` = `rule`, 27.10.2026: против третьего D = 0.736 на 10 картах и 0.752 с двумя
морскими по вердикту игры — сначала очки; по материалу — 0.610 и 0.616). D — доля силы в турнире за обе стороны каждой
карты (0.5 — поровну; от 0.53 — сильнее). `SI-R3` эко-сфер не строит, поэтому против него вердикт по очкам щедрее;
строгое мерило — вердикт по материалу, как до поколения 4.
