# AI player passport — SI-R5.3-T (M.A.X.R.SI)

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

Examples: now `SI-R5.3-T`; with the advisor — `SI-R5.3-T+Lgemma4-12b`; the frozen champions — `SI-R1` … `SI-R5`; a
future tactics network after imitation — `SI-H6.0-I40k`.

| | |
|---|---|
| Version | **SI-R5.3-T** — brain `rule`: generation 5 (champion `base5`, frozen 28.10.2026), build 3 — a bulldozer clears the rubble that blocks building sites and gathers metal from the battlefield for repairs and reloads (03.11.2026); build 2 — the surveyor explores near the map centre too, the builder does not leave its building because of a light threat (28.10.2026); build 1 — re-examines all its orders every turn; as strong as `SI-R5.2-T`; tuned by tournaments |
| Made of | hand-written rules only: strategy → operations → micro; deterministic |
| Neural networks | none yet (planned: own small tactics and strategy nets trained on AI games) |
| LLM | not used by default; optional advisor brain `llm` (a local model via an OpenAI-compatible API) — `SI-R5.3-T+L<model>` |
| Learned from | nothing learned; rules tuned by AI-vs-AI tournaments; 0 human games so far |
| Plays | through fog of war, by the same rules as a human; "Turn based" mode only; joins a normal network game as a player (`maxr_ai_client`); in the lobby it says in the chat why it is not ready and what to press |
| Tested on | 5 remake maps (Mushroom, Lava, Delta, Donuts, Three Isles), 5 generated maps up to 250×250 (`Gen_*`) and 2 generated sea maps with bases at the coast (`Gen_Coast`, `Gen_Gulf`) |

**Can:** choosing its clan by the map, economy (mines on resources, factories by income), eco-spheres and the points
victory (a draw at the turn limit becomes a win on points), research and gold upgrades, army operations (gather → march
→ search → strike → retreat), unit roles (scouts, anti-air, repair, artillery behind tanks), memory of the enemy and threat
map, several passes per turn (move, shoot, finish off), base defence against raids (several radars, anti-air ring, guards
against infiltrators, mines, ring of connectors), a smart surveyor (rings around the base, follows a found vein, keeps
away from the enemy), navy (shipyard, corvettes, gunboats, missile ships, submarines, sea landing, dock, resupply at sea,
sea mines at the own shipyard), coastal missile defence, choosing land / sea / air emphasis from the map; aircraft (AWAC
that hides after scouting, bombers "hit and leave", fighters, air transport); infiltrators; APC under water. **New in
generation 5:** the clan — by the map and its own landing, like a human in the game preparation (usually Musashi: tanks
are the core of its army; the enemy most likely beyond water and no sea at the base — The Chosen, aircraft); tactics of
the passes — a wounded unit that would not survive the enemy turn shoots and pulls back; against an enemy that outranges
it — close in and shoot this turn or leave its reach; damage "from nowhere" — it looks for a submarine or an infiltrator
and fires blind at the cells next to its disabled units; an early rush is met by a defender tank, and the factory keeps
its energy; against an army beyond water with no way across — bombers from an approach point out of reach of the enemy
anti-air, the enemy sea transport first; the surveyor keeps away from where the enemy may be, anti-air at far mines; a
fresh shipyard builds a minelayer first; an eco-sphere waits for its housing; a column in narrow passages. **New in
build 5.1:** every turn, the first one after loading a saved game too, it re-examines each unit still driving a path of
the previous turns: the order is still needed, the path is safe and there is no clearly better task — it drives on;
otherwise it decides for the unit anew (a new path, a shot, a retreat, getting away from a threat) or stops it. After
loading, it gives orders at the first turn instead of finishing the old paths. Against `SI-R5` — equal (D 0.510 in the
full tournament, 0.482 and 0.516 on the large maps), fewer turn-backs and wasted paths, fewer units lost on an old path. **New in build 5.2:** the surveyor of a base near
the centre of a large map explores in rings around the base instead of standing (idle 0.6% of its turns instead of 49.8%,
4.4 mines by turn 100 instead of 1.3); on a cramped map with the enemy very close it stays as before (there a standing
surveyor leaves the metal to an early army). A builder does not leave its building because of a threat that cannot
kill it within two enemy turns (in a saved game of a human the big factory is built from turn 13 instead of running
away from one scout). Against `SI-R5` — D 0.516 in the full tournament (0.515 by material), 0.497 on the large maps,
0.629 with the landings near the map centre: not weaker than `SI-R5.1-T`. **New in build 5.3:** a bulldozer of the base
clears the rubble that blocks a building site — a mine on a found resource at once (before the other needs of a small
factory, besides anti-air) — and, in a long fight, gathers metal from the battlefield: to a hungry repairman or an ammo
truck nearby (they repair and reload neighbours with it) or to the base depot; a small factory builds 1–3 bulldozers
when it has nothing else to do. Against `SI-R5` — D 0.522 in the full tournament (0.520 by material, ±0.017): not weaker
than `SI-R5.2-T` (0.516). With the experience off (`MAXR_BULLDOZER=0`) the games are byte for byte the same as without it
(10/10 control games against the frozen etalons). In the game of this build: the map zoom goes
out until the whole map is on the screen; the cursor follows the interface scale and is about one cell in a game,
Windows no longer enlarges it by its pointer size, "Settings" → "Cursor" sets its size (Auto, 50–200%); the ready mark
in the lobby is clicked in a cell around it; on 23 maps (`Gen_*` and the 16 maps 112 and 250) nothing can be built only
on single bushes and wrecks, equally at both bases; fixes 10b, 15b, 50 and 51.

**Cannot yet:** simultaneous turns.

**Engine of this build:** fixes 01–51 (01–12, 15 and 16 are also offered upstream). Recent: 51 — the checksum of a map
from 129 to 255 cells covers all its cells on every platform (the size bytes of the header were read as signed `char`:
on x86 a 250×250 map was summed over 108 bytes of 187 500); 50 — the same checksum
of the bytes from 128 on every platform (Linux on ARM has an unsigned `char` and went out of sync with the other
platforms at once); 49 — the ready mark in the lobby is clicked in a cell around it; 48 — the hot spots of the attack,
steal and disable cursors in their centre, of the scroll arrows at their tip; 47 — the map zoom out to the whole map;
10b, 15b — fixes 10 and 15 reworked after the review upstream (a search for stealth units after an upgrade only when the
scan range grows; the fast checksum of the scan maps is an overload in `crc.h`); 46 — a network client out of
sync for 3 s takes the model from the server and logs the parts of its checksum, and it creates the players after the
unit data (no "OUT OF SYNC" before the last landing); 45 — with the AI in the lobby only "Turn based" can be chosen; 41–44
— the factory window keeps "Done" unavailable while the queue is empty, a warning when a network save is loaded as a
single game, "Light plant" in Russian, the key "f" only centres; 38 — the unit window shows a started building at once;
39 — resinstaller installs the hit sounds; 34, 35, 40 — the game in a normal window, an interface scale "Auto" or
100–400%, Russian strings of the menus and the game; 33 — a resync restores the client model in full (the cargo of
transports and depots, the units of a live client, the unit versions after research); 36 — a saved game without GUI info
(all autosaves of AI network games) no longer stops the game thread of the dedicated server; 37 — the self destruction of
a land or sea mine no longer crashes the game. Earlier: disabled units do not fire
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
| `SI-R4` = `SI-R4.0-T` | — | `base4` | 27.10.2026 | against `SI-R3`: D = 0.736 on 10 maps, 0.752 with the 2 sea maps by the game verdict (points first); by material — 0.610 and 0.616 |
| `SI-R5` = `SI-R5.0-T` | — | `base5` | 28.10.2026 | against `SI-R4`: D = 0.738 on 10 maps, 0.713 with the 2 sea maps by the game verdict; by material — 0.678 and 0.649; mostly from the clan choice (`SI-R4` does not choose a clan) |
| `SI-R5.1-T` | — | `rule` | — | against `SI-R5`: D = 0.510 in the full tournament, 0.482 and 0.516 on the large maps — equal; re-examines its orders every turn |
| `SI-R5.2-T` | — | `rule` | — | against `SI-R5`: D = 0.516 in the full tournament (0.515 by material), 0.497 on the large maps, 0.629 with the landings near the map centre — not weaker than `SI-R5.1-T`; the surveyor explores near the map centre, the builder keeps building under a light threat |
| `SI-R5.3-T` | — | `rule` | — | against `SI-R5`: D = 0.522 in the full tournament (0.520 by material) — not weaker than `SI-R5.2-T`; a bulldozer clears rubble from building sites and brings metal from the battlefield for repairs and reloads |

D is the share of strength in a tournament on both sides of each map (0.5 — equal; ≥ 0.53 — stronger). `SI-R3` builds
no eco-spheres, so against it the verdict by points is too generous; the verdict by material (as before generation 4) is
the strict measure.

---

# Паспорт ИИИ — SI-R5.3-T (M.A.X.R.SI)

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

Примеры: сейчас `SI-R5.3-T`; с советником — `SI-R5.3-T+Lgemma4-12b`; замороженные чемпионы — `SI-R1` … `SI-R5`; будущая
сеть-тактик после подражания — `SI-H6.0-I40k`.

| | |
|---|---|
| Версия | **SI-R5.3-T** — мозг `rule`: поколение 5 (чемпион `base5`, заморожен 28.10.2026), сборка 3 — бульдозер расчищает обломки, что мешают стройке, и собирает металл с поля боя — на ремонт и перезарядку (03.11.2026); сборка 2 — геолог разведывает и у центра карты, стройщик не бросает стройку из-за лёгкой угрозы (28.10.2026); сборка 1 — пересматривает приказы каждый ход; по силе — как `SI-R5.2-T`; настроен турнирами |
| Из чего | только правила, написанные вручную: стратегия → операции → микро; детерминирован |
| Нейросети | пока нет (в плане — свои маленькие сети тактики и стратегии, обученные на партиях ИИИ) |
| LLM | по умолчанию нет; по желанию — мозг-советник `llm` (локальная модель через API, совместимый с OpenAI) — `SI-R5.3-T+L<модель>` |
| На чём учился | ничему не учился; правила настроены турнирами ИИИ против ИИИ; партий с человеком — 0 |
| Как играет | сквозь туман войны, по тем же правилам, что человек; только режим «По очереди»; входит в обычную сетевую игру как игрок (`maxr_ai_client`); в лобби говорит в чат, почему не готов и что нажать |
| Где проверен | 5 карт ремейка (Mushroom, Lava, Delta, Donuts, Three Isles), 5 сгенерированных карт до 250×250 (`Gen_*`) и 2 сгенерированные морские карты с базами у берега (`Gen_Coast`, `Gen_Gulf`) |

**Умеет:** выбор клана по карте, хозяйство (шахты на ресурсах, фабрики по доходу), эко-сферы и победа по очкам (ничья на
пределе ходов становится победой по очкам), исследования и улучшения за золото, операции армии (сбор → поход → поиск →
удар → отход), роли машин (скауты, ПВО, ремонт, артиллерия за танками), память о враге и карта угроз, несколько заходов
за ход (подъехал, выстрелил, добил), оборона базы от налёта (несколько радаров, кольцо ПВО, охрана от диверсантов, мины,
соединители кольцом), умный геолог (кольца вокруг базы, доразведка найденной жилы, к врагу не ходит), флот (верфь,
корветы, канонерки, ракетные корабли, подлодки, морской десант, док, снабжение в море, морские мины у своей верфи),
ракетная оборона берега, выбор упора «суша / море / воздух» по карте; авиация (AWAC, который прячется после разведки,
бомбардировщики «ударил — ушёл», истребители, авиатранспорт); коммандос; БМП под водой. **Новое в поколении 5:** клан —
по карте и своей точке высадки, как человек при подготовке партии (обычно Musashi: танки — основа его армии; враг по
догадке за водой и моря у базы нет — The Chosen, авиация); тактика заходов — раненая машина, которой не пережить ход
врага, стреляет и отходит; против врага, который бьёт дальше, — подъехать и выстрелить в этот ход или уйти из его
досягаемости; урон «из ниоткуда» — ищет подлодку или диверсанта и бьёт наугад по клеткам рядом со своими выключенными
юнитами; ранний раш встречает танк-защитник, фабрика не отдаёт свою энергию; против армии за водой без переправы —
бомбардировщики с точки подлёта вне досягаемости ПВО врага, первая цель — его морской транспорт; геолог обходит места, где
может быть враг, зенитки у дальних шахт; у свежей верфи — сначала минёр; эко-сфера ждёт своё жильё; колонна в узостях.
**Новое в сборке 5.1:** каждый ход, в том числе первый после загрузки сохранения, пересматривает каждую машину, что едет
по пути из прошлых ходов: приказ ещё нужен, путь безопасен, лучшего дела нет — едет дальше; иначе решает за неё заново
(новый путь, выстрел, отход, уход от угрозы) или останавливает. После загрузки даёт приказы в первый же ход, а не
досиживает старые пути. Против `SI-R5` — на равных (D 0.510 в полном турнире, 0.482 и 0.516 на больших картах);
разворотов и пути впустую меньше, на старом пути гибнет меньше машин. **Новое в сборке 5.2:** геолог базы у центра
большой карты разведывает кольцами вокруг базы, а не стоит (простой — 0.6% его ходов вместо 49.8%, шахт к 100-му ходу 4.4
вместо 1.3); на тесной карте при враге совсем рядом — как раньше (там стоящий геолог оставляет металл ранней армии).
Стройщик не бросает стройку из-за угрозы, которая не убьёт его за два хода врага (в сохранённой партии человека большая
фабрика строится с 13-го хода, а не стройщик уходит от одного скаута). Против `SI-R5` — D 0.516 в полном турнире (0.515 по
материалу), 0.497 на больших картах, 0.629 при высадке у центра карты: не слабее `SI-R5.1-T`. **Новое в сборке 5.3:**
бульдозер у базы расчищает обломки, что мешают стройке, — шахту на найденном ресурсе сразу (раньше прочих нужд малой
фабрики, кроме ПВО) — а в затяжном бою собирает металл с поля боя: голодному ремонтнику или грузовику боеприпасов рядом
(те чинят и перезаряжают соседей этим металлом) или в склад базы; малая фабрика строит 1–3 бульдозера, когда ей больше
нечего строить. Против `SI-R5` — D 0.522 в полном турнире (0.520 по материалу, ±0.017): не слабее `SI-R5.2-T` (0.516).
С выключенным опытом (`MAXR_BULLDOZER=0`) партии побайтно как без него (контроль 10/10 против замороженных эталонов).
В игре этой сборки: масштаб
карты — до «вся карта на экране»; курсор следует масштабу интерфейса и в партии — около клетки, Windows больше не
увеличивает его по «Размеру указателя», размер — «Настройки» → «Курсор» («Авто», 50–200 %); точка готовности в лобби
нажимается ячейкой вокруг неё; на 23 картах (`Gen_*` и 16 карт 112 и 250) строить нельзя только на одиночных кустах и
обломках, поровну у обеих баз; правки 10b, 15b, 50 и 51.

**Пока не умеет:** одновременные ходы.

**Движок этой сборки:** правки 01–51 (01–12, 15 и 16 предложены и авторам). Последние: 51 — сумма карты от 129 до 255
клеток — по всем её клеткам на всех платформах (байты размера в заголовке читались знаковым `char`: на x86 карта 250×250
суммировалась по 108 байтам из 187 500); 50 — сумма байт от 128
одинакова на всех платформах (на Linux ARM `char` беззнаковый, и партия с другой платформой расходилась сразу); 49 — точка
готовности в лобби нажимается ячейкой вокруг неё; 48 — горячие точки курсоров прицела, кражи, отключения — в центре,
стрелок прокрутки — на острие; 47 — масштаб карты до «вся карта на экране»; 10b, 15b — правки 10 и 15 доработаны по
замечаниям авторов (поиск скрытых после модернизации — только если обзор вырос; быстрая сумма карт обзора — перегрузкой в
`crc.h`); 46 — сетевой клиент при
рассинхроне дольше 3 с сам берёт модель у сервера и пишет в журнал части своей суммы, а игроков создаёт после данных юнитов
(нет «OUT OF SYNC» до высадки последнего игрока); 45 — в лобби с ИИИ можно выбрать только «По очереди»; 41–44 — на заводе
«Готово» недоступна, пока очередь пуста, предупреждение при одиночной загрузке сетевой партии, «Лёгкий завод», клавиша
«f» — только центр; 38 — окно юнита сразу показывает стройку; 39 — resinstaller ставит звуки попаданий; 34, 35, 40 —
игра в обычном окне, масштаб интерфейса «Авто» или 100–400%, русские строки меню и партии; 33 — пересинхронизация
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
коммандос) → `SI-R4` = `SI-R4.0-T` (`base4`, 27.10.2026: против третьего D = 0.736 на 10 картах и 0.752 с двумя
морскими по вердикту игры — сначала очки; по материалу — 0.610 и 0.616) → `SI-R5` = `SI-R5.0-T` (`base5`,
28.10.2026: против четвёртого D = 0.738 на 10 картах и 0.713 с двумя морскими по вердикту игры, 0.678 и 0.649 по
материалу; прирост — в основном от выбора клана: `SI-R4` клан не выбирает) → `SI-R5.1-T` (`rule`, 28.10.2026: пересмотр
приказов каждый ход; против пятого D = 0.510 в полном турнире, 0.482 и 0.516 на больших картах — на равных) →
`SI-R5.2-T` (`rule`, 28.10.2026: геолог разведывает и у центра карты, стройщик не бросает стройку из-за лёгкой угрозы;
против пятого D = 0.516 в полном турнире, 0.497 на больших картах, 0.629 при высадке у центра) → `SI-R5.3-T` (`rule`,
03.11.2026: бульдозер расчищает обломки под стройку и везёт металл с поля боя на ремонт и перезарядку; против пятого
D = 0.522 в полном турнире, 0.520 по материалу). D — доля
силы в турнире за обе стороны каждой карты (0.5 — поровну; от 0.53 — сильнее). `SI-R3` эко-сфер не строит, поэтому
против него вердикт по очкам щедрее; строгое мерило — вердикт по материалу, как до поколения 4.
