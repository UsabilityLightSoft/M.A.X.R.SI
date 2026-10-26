# AI player passport — AI-3.1 (M.A.X.R.SI 0.1)

*Русский текст — ниже.*

The AI player is named after its **generation**: generation N is the N-th "champion" — a frozen version that beat the
previous champion in a tournament. Builds between champions get a minor number (3.1, 3.2 …). Every release of
M.A.X.R.SI ships one AI version, and this passport says what it is made of.

| | |
|---|---|
| Version | **AI-3.1** — brain `rule` at commit `5af9f6a7` (release `si-0.1`) |
| Generation | 3 (champion `base3`, frozen 26.10.2026) + navy (not a champion yet: D = 0.489 against `base3`) |
| Made of | hand-written rules only: strategy → operations → micro; deterministic |
| Neural networks | none yet (planned: own small tactics and strategy nets trained on AI games) |
| LLM | not used by default; optional advisor brain `llm` (a local model via an OpenAI-compatible API) |
| Learned from | nothing learned; rules tuned by AI-vs-AI tournaments; 0 human games so far |
| Plays | through fog of war, by the same rules as a human; "Turn based" mode only; joins a normal network game as a player (`maxr_ai_client`) |
| Tested on | 5 remake maps (Mushroom, Lava, Delta, Donuts, Three Isles) and 5 generated maps up to 250×250 (`Gen_*`) |

**Can:** economy (mines on resources, factories by income), research and gold upgrades, army operations (gather → march
→ search → strike → retreat), unit roles (scouts, anti-air, repair, artillery behind tanks), memory of the enemy and threat
map, several passes per turn (move, shoot, finish off), base defence against raids (several radars, anti-air ring, guards
against infiltrators, mines, ring of connectors), navy (shipyard, corvettes, gunboats, missile ships, submarines, sea
landing, dock), coastal missile defence, choosing land / sea / air emphasis from the map.

**Cannot yet:** aircraft (bombers, AWAC, air transport), own infiltrator raids, eco-spheres and points victory, clan
choice, simultaneous turns.

**Lineage**

| Generation | Frozen | Strength |
|---|---|---|
| 1 — `base` | 24.10.2026 | rules with economy, research and roles of that day |
| 2 — `base2` | 26.10.2026 | against gen 1: 200 wins of 200 (D = 0.909) |
| 3 — `base3` | 26.10.2026 | against gen 2: D = 0.565 (93 wins, 62 losses, 45 draws) |
| 3.1 — `rule` | 27.10.2026 | against gen 3: D = 0.489 — equal on land, adds a navy |

D is the share of strength in a tournament on both sides of each map (0.5 — equal; ≥ 0.53 — stronger).

---

# Паспорт ИИИ — ИИИ-3.1 (M.A.X.R.SI 0.1)

ИИИ (ИИ Игрока) называется по **поколению**: поколение N — N-й «чемпион», замороженная версия, которая обыграла
прежнего чемпиона в турнире. Сборки между чемпионами получают дробный номер (3.1, 3.2 …). В каждом выпуске
M.A.X.R.SI — одна версия ИИИ, и этот паспорт говорит, из чего она собрана.

| | |
|---|---|
| Версия | **ИИИ-3.1** — мозг `rule`, коммит `5af9f6a7` (выпуск `si-0.1`) |
| Поколение | 3 (чемпион `base3`, заморожен 26.10.2026) + флот (пока не чемпион: D = 0.489 против `base3`) |
| Из чего | только правила, написанные вручную: стратегия → операции → микро; детерминирован |
| Нейросети | пока нет (в плане — свои маленькие сети тактики и стратегии, обученные на партиях ИИИ) |
| LLM | по умолчанию нет; по желанию — мозг-советник `llm` (локальная модель через API, совместимый с OpenAI) |
| На чём учился | ничему не учился; правила настроены турнирами ИИИ против ИИИ; партий с человеком — 0 |
| Как играет | сквозь туман войны, по тем же правилам, что человек; только режим «Turn based»; входит в обычную сетевую игру как игрок (`maxr_ai_client`) |
| Где проверен | 5 карт ремейка (Mushroom, Lava, Delta, Donuts, Three Isles) и 5 сгенерированных карт до 250×250 (`Gen_*`) |

**Умеет:** хозяйство (шахты на ресурсах, фабрики по доходу), исследования и улучшения за золото, операции армии
(сбор → поход → поиск → удар → отход), роли машин (скауты, ПВО, ремонт, артиллерия за танками), память о враге и карта
угроз, несколько заходов за ход (подъехал, выстрелил, добил), оборона базы от налёта (несколько радаров, кольцо ПВО,
охрана от диверсантов, мины, соединители кольцом), флот (верфь, корветы, канонерки, ракетные корабли, подлодки, морской
десант, док), ракетная оборона берега, выбор упора «суша / море / воздух» по карте.

**Пока не умеет:** авиация (бомбардировщики, AWAC, авиатранспорт), свои налёты коммандос, эко-сферы и победа по очкам,
выбор клана, одновременные ходы.

**Родословная** — таблица выше: поколение 1 (`base`, 24.10.2026) → 2 (`base2`, 200 побед из 200 над первым) → 3 (`base3`,
D = 0.565 над вторым) → 3.1 (`rule`, на равных с третьим на суше, плюс флот). D — доля силы в турнире за обе стороны
каждой карты (0.5 — поровну; от 0.53 — сильнее).
