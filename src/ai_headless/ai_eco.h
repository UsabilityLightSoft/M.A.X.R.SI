/***************************************************************************
 * ai_eco.h — эко-сферы и победа по очкам (Р7, шаг 13, часть 4а; PLAN.md §7 п. 94, 106).
 *
 * Решение Владыки 27.10.2026: ничью превращать в победу по очкам, особенно к пределу ходов.
 * Механика (src/lib, cPlayer::accumulateScore, cModel::deleteUnit): работающая эко-сфера даёт 1 очко в
 * начале хода хозяина; уничтоженная — хозяин теряет все очки, которые она принесла; выключенная — не
 * работает (очков нет); захватить нельзя. Сфере нужны энергия (1) и человек (жилой блок даёт 3). Большое
 * здание, 20 металла.
 *
 * Условие победы и предел ходов ИИИ узнаёт честно — из настроек партии (cGameSettings), как человек в окне
 * настроек; очки и число сфер каждого игрока видны в окне отчётов (cWindowReports) — их берём из модели.
 *
 * Когда строить (решения простые и пишутся в журнал — событие «eco»):
 *  - «по очкам» (победа при N очках) — сферы и есть цель: с первой фабрики, 2 + добыча/15 (≤ 6);
 *  - «по ходам» — всегда, если сферы строит враг (у него очки или сферы): на одну больше, чем у него (≤ 4);
 *    до предела осталось немного (≤ max (60, предел/4) ходов) — 1 сфера (при добыче ≥ 30 — 2), раньше шахт; партия
 *    застыла (с хода 100 за 40 ходов своих потерь не больше 1) и добыча ≥ 30 — 1 сфера из лишнего: после фабрик, на
 *    площадке с соединителями, без остановки исследований и фабрик;
 *  - «до уничтожения» — не строить (очки не решают).
 * Ставятся в глубине базы (кольцами от высадки — ядро, под кольцом ПВО-пушек), охрана — посты пехоты и
 * стены (ai_defense), тревога у базы — как у шахт. Сферы врага — первая цель бомбардировщиков и налётов
 * (ai_air) и обстрела с моря (ai_navy).
 * Опыт «вариант против варианта»: MAXR_ECO=0 — сфер не строить (кроме режима «по очкам»).
 ***************************************************************************/

#ifndef ai_headless_ai_ecoH
#define ai_headless_ai_ecoH

#include "game/data/units/id.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

class cModel;
class cPlayer;

namespace ai
{

class cEcoPlan
{
public:
	static const sID kIdEcoSphere; // эко-сфера (1, 35)

	// раз в ход, до оперативки: условие победы, очки сторон, застой, сколько сфер нужно
	void update (const cModel& model, const cPlayer& player, int income, int turn);

	// сколько сфер держать (построенные + строящиеся + в планах); 0 — не строить
	int spheresWanted() const { return wanted_; }
	// строить раньше шахт и фабрик (враг строит сферы, режим «по очкам», предел близко)
	bool urgent() const { return urgent_; }
	// сферы срочны (те же причины), даже когда нужные уже построены: построенной, но не работающей сфере энергию и людей
	// отдают исследования, переработка, учебный центр, лишняя фабрика
	bool pressing() const { return pressing_; }
	const std::string& why() const { return why_; }
	// условие победы: "turns", "points", "death"
	const char* victoryName() const;

	struct sCounts
	{
		int own = 0;        // работающих сфер
		int ownBuilt = 0;   // построенных (работают или нет)
		int enemy = 0;      // работающих сфер у врагов (окно отчётов)
		int ownScore = 0;
		int enemyScore = 0; // лучший из врагов
		int turnsLeft = -1; // до предела ходов (−1 — предела нет)
		int lostSpheres = 0; // своих сфер потеряно за партию
	};
	const sCounts& counts() const { return counts_; }

	std::vector<nlohmann::json> takeEvents();
	nlohmann::json toJson() const;

private:
	void event (const char* what, nlohmann::json extra);

	int turn_ = 0;
	int wanted_ = 0;
	bool urgent_ = false;
	bool pressing_ = false;
	std::string why_;
	std::string lastWhy_;
	int lastWanted_ = -1;
	int victory_ = 0; // 0 — по ходам, 1 — по очкам, 2 — до уничтожения
	std::map<int, int> losses_; // ход → своих потерь всего (машины + здания) на его начало
	std::vector<unsigned int> spheres_; // свои сферы (id) — гибель в журнал
	bool stall_ = false;
	sCounts counts_;
	std::vector<nlohmann::json> events_;
};

} // namespace ai

#endif // ai_headless_ai_ecoH
