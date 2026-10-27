# AI player passport — AI-3.2 (M.A.X.R.SI, branch `main`)

*Русский текст — ниже.*

The AI player is named after its **generation**: generation N is the N-th "champion" — a frozen version that beat the
previous champion in a tournament. Builds between champions get a minor number (3.1, 3.2 …). Every release of
M.A.X.R.SI ships one AI version, and this passport says what it is made of.

| | |
|---|---|
| Version | **AI-3.2** — brain `rule` in branch `main` after release `si-0.1` (AI-3.1) |
| Generation | 3 (champion `base3`, frozen 26.10.2026) + navy + aircraft and infiltrators (not a champion yet: D = 0.505 against `base3` on the new engine) |
| Made of | hand-written rules only: strategy → operations → micro; deterministic |
| Neural networks | none yet (planned: own small tactics and strategy nets trained on AI games) |
| LLM | not used by default; optional advisor brain `llm` (a local model via an OpenAI-compatible API) |
| Learned from | nothing learned; rules tuned by AI-vs-AI tournaments; 0 human games so far |
| Plays | through fog of war, by the same rules as a human; "Turn based" mode only; joins a normal network game as a player (`maxr_ai_client`) |
| Tested on | 5 remake maps (Mushroom, Lava, Delta, Donuts, Three Isles), 5 generated maps up to 250×250 (`Gen_*`) and 2 generated sea maps with bases at the coast (`Gen_Coast`, `Gen_Gulf`) |

**Can:** economy (mines on resources, factories by income), research and gold upgrades, army operations (gather → march
→ search → strike → retreat), unit roles (scouts, anti-air, repair, artillery behind tanks), memory of the enemy and threat
map, several passes per turn (move, shoot, finish off), base defence against raids (several radars, anti-air ring, guards
against infiltrators, mines, ring of connectors), navy (shipyard, corvettes, gunboats, missile ships, submarines, sea
landing, dock), coastal missile defence, choosing land / sea / air emphasis from the map; **aircraft** (AWAC around the
enemy base out of anti-air range, bombers "hit and leave", fighters against enemy aircraft, air transport for
infiltrators and mobile anti-air); **infiltrators** (training on alien units, capturing aliens and enemy vehicles, disabling
enemy mines, power plants, factories, radars and anti-air; the raid "AWAC + infiltrator + bombers"); APC under water.

**Cannot yet:** eco-spheres and points victory, resupply of ships at sea, clan choice, simultaneous turns.

**Engine of this build** (branch `main`, also offered upstream): fixes 17–32 — disabled units do not fire and do not work
(a constructor pauses), scan and sentry maps, server checks of the landing (one credit per started 5 units of cargo), of
big buildings, of transfers and of four more actions, non-latin paths and user names, the path search, a timer race; a
detector searches by itself; a vehicle and an action wait for the step of an invisible unit; a draw at the same best score;
a stolen vehicle is noticed at the next turn of its owner; disabled aliens recover.

**Lineage**

| Generation | Frozen | Strength |
|---|---|---|
| 1 — `base` | 24.10.2026 | rules with economy, research and roles of that day |
| 2 — `base2` | 26.10.2026 | against gen 1: 200 wins of 200 (D = 0.909) |
| 3 — `base3` | 26.10.2026 | against gen 2: D = 0.565 (93 wins, 62 losses, 45 draws) |
| 3.1 — `rule` | 27.10.2026 | against gen 3: D = 0.489 — equal on land, adds a navy |
| 3.2 — `rule` | 27.10.2026 | against gen 3: D = 0.505 on 10 maps, 0.514 with the 2 sea maps — adds aircraft and infiltrators |

D is the share of strength in a tournament on both sides of each map (0.5 — equal; ≥ 0.53 — stronger).

---

# Паспорт ИИИ — ИИИ-3.2 (M.A.X.R.SI, ветка `main`)

ИИИ (ИИ Игрока) называется по **поколению**: поколение N — N-й «чемпион», замороженная версия, которая обыграла
прежнего чемпиона в турнире. Сборки между чемпионами получают дробный номер (3.1, 3.2 …). В каждом выпуске
M.A.X.R.SI — одна версия ИИИ, и этот паспорт говорит, из чего она собрана.

| | |
|---|---|
| Версия | **ИИИ-3.2** — мозг `rule` в ветке `main` после выпуска `si-0.1` (ИИИ-3.1) |
| Поколение | 3 (чемпион `base3`, заморожен 26.10.2026) + флот + авиация и коммандос (пока не чемпион: D = 0.505 против `base3` на новом движке) |
| Из чего | только правила, написанные вручную: стратегия → операции → микро; детерминирован |
| Нейросети | пока нет (в плане — свои маленькие сети тактики и стратегии, обученные на партиях ИИИ) |
| LLM | по умолчанию нет; по желанию — мозг-советник `llm` (локальная модель через API, совместимый с OpenAI) |
| На чём учился | ничему не учился; правила настроены турнирами ИИИ против ИИИ; партий с человеком — 0 |
| Как играет | сквозь туман войны, по тем же правилам, что человек; только режим «Turn based»; входит в обычную сетевую игру как игрок (`maxr_ai_client`) |
| Где проверен | 5 карт ремейка (Mushroom, Lava, Delta, Donuts, Three Isles), 5 сгенерированных карт до 250×250 (`Gen_*`) и 2 сгенерированные морские карты с базами у берега (`Gen_Coast`, `Gen_Gulf`) |

**Умеет:** хозяйство (шахты на ресурсах, фабрики по доходу), исследования и улучшения за золото, операции армии
(сбор → поход → поиск → удар → отход), роли машин (скауты, ПВО, ремонт, артиллерия за танками), память о враге и карта
угроз, несколько заходов за ход (подъехал, выстрелил, добил), оборона базы от налёта (несколько радаров, кольцо ПВО,
охрана от диверсантов, мины, соединители кольцом), флот (верфь, корветы, канонерки, ракетные корабли, подлодки, морской
десант, док), ракетная оборона берега, выбор упора «суша / море / воздух» по карте; **авиация** (AWAC облетает базу врага
вне ПВО, бомбардировщики «ударил — ушёл», истребители против авиации врага, авиатранспорт для коммандос и подвижной ПВО);
**коммандос** (тренировка на технике пришельцев, захват пришельцев и машин врага, выключение шахт, электростанций, заводов,
радаров и зениток врага; налёт «AWAC + коммандос + бомбардировщики»); БМП под водой.

**Пока не умеет:** эко-сферы и победа по очкам, снабжение кораблей в море, выбор клана, одновременные ходы.

**Движок этой сборки** (ветка `main`, предложено и авторам): правки 17–32 — выключенный не стреляет и не работает
(конструктор ставит стройку на паузу), карты обзора и дозора, сервер проверяет высадку (кредит за каждые начатые 5 единиц
груза), большие здания, передачу и ещё четыре действия, пути и имя пользователя с русскими буквами, поиск пути, гонка
таймера; сыщик ищет своим зрением; машина и действие ждут шага невидимого; ничья при равенстве очков; угон замечают в
начале хода хозяина; выключенные пришельцы просыпаются.

**Родословная** — таблица выше: поколение 1 (`base`, 24.10.2026) → 2 (`base2`, 200 побед из 200 над первым) → 3 (`base3`,
D = 0.565 над вторым) → 3.1 (`rule`, на равных с третьим на суше, плюс флот) → 3.2 (`rule`, D = 0.505 против третьего на
10 картах и 0.514 с двумя морскими, плюс авиация и коммандос). D — доля силы в турнире за обе стороны каждой карты
(0.5 — поровну; от 0.53 — сильнее).
