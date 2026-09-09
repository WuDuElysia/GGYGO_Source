/**
 * @file ActionArbiter.h
 * @brief 动作优先级仲裁器
 *
 * 读取 RuntimeData 意图 → 调 ASC TryActivateAbility → 写 ActionGranted。
 * 处理攻击、闪避等需要 GAS 批准的动作请求。
 *
 * 抗性规则（参考 BBB-Nexus）：
 * - 当前状态有"抗性"值，请求有"优先级"值
 * - 优先级 > 抗性 → 允许执行
 * - 攻击抗性 = 40，闪避抗性 = 80
 * - 其他状态抗性 = 0（无条件接受）
 *
 * 设计说明：
 * 同一个按键"先按在攻击上还是闪避上"的冲突由第一层意图处理器解决。
 * 本仲裁器只处理 GAS 层面的批准（是否有足够的资源/状态允许释放）。
 */
#pragma once

#include "Pipeline/Interfaces/IArbiter.h"
#include "StateMachine/CharacterStateType.h"

class UAbilitySystemComponent;
class FGYGOStateManager;

class FActionArbiter : public IArbiter
{
public:
	/**
	 * 注入 ASC 和 StateManager 指针
	 * @param InASC AbilitySystemComponent
	 * @param InSM  StateManager（用于预判状态切换可行性）
	 */
	void Init(UAbilitySystemComponent* InASC, FGYGOStateManager* InSM);

	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	/** ASC 指针（不拥有） */
	UAbilitySystemComponent* ASC = nullptr;

	/** StateManager 指针（不拥有，用于状态预判检查） */
	FGYGOStateManager* SM = nullptr;

	/**
	 * 获取指定状态的抗性值
	 * 高抗性状态（翻滚=100）几乎不可被打断
	 * @param State 当前状态
	 * @return 抗性值（0~100）
	 */
	static int32 GetStateResistance(ECharacterStateType State);

	/**
	 * 获取指定动作的优先级
	 * 高优先级动作可以打断低抗性状态
	 * @param State 目标状态
	 * @return 优先级（0~100）
	 */
	static int32 GetActionPriority(ECharacterStateType State);
};
