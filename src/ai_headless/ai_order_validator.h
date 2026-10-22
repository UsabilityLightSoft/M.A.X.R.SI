/***************************************************************************
 * ai_order_validator.h — валидатор приказов (PLAN_AI этап 2).
 *
 * Мягкая предпроверка легальности приказа ПЕРЕД отправкой серверу.
 * Сервер всё равно отклоняет нелегальное; задача валидатора — отклонить
 * очевидно нелегальное с ПРИЧИНОЙ (критично для LLM-советника, этап 5).
 ***************************************************************************/

#ifndef ai_headless_ai_order_validatorH
#define ai_headless_ai_order_validatorH

#include <string>

class cModel;

namespace ai
{

struct cOrder;

struct sValidationResult
{
	bool ok = true;
	std::string reason; // причина отклонения, когда ok == false
};

sValidationResult validateOrder (const cModel& model, const cOrder& order);

} // namespace ai

#endif // ai_headless_ai_order_validatorH
