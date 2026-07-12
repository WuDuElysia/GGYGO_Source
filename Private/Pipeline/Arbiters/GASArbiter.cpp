/**
 * @file GASArbiter.cpp
 * @brief GAS 状态仲裁器实现（Phase 7 扩展版）
 *
 * 仲裁逻辑（每帧执行，在 ArbiterPipeline::Process 的 reset 之后）：
 *   1. 检查 State.Dead → 全封锁
 *   2. 检查 State.Stunned → 封锁动作
 *   3. 检查 Restriction.* Tag → 逐位设置 bBlock*
 *
 * 注意：ArbiterPipeline 在此函数之前已将所有 bBlock* 重置为 false。
 * 此处只负责将 Tag 翻译为 true。
 */
#include "Pipeline/Arbiters/GASArbiter.h"
#include "Data/RuntimeData.h"
#include "AbilitySystemComponent.h"
#include "GameplayTagContainer.h"

void FGASArbiter::Init(UAbilitySystemComponent* InASC)
{
	ASC = InASC;

	// 缓存所有 Tag，避免每帧字符串查找开销
	Tag_Dead     = FGameplayTag::RequestGameplayTag(GGYGOTags::State::Dead);
	Tag_Stunned  = FGameplayTag::RequestGameplayTag(GGYGOTags::State::Stunned);
	Tag_CantMove    = FGameplayTag::RequestGameplayTag(GGYGOTags::Restriction::CantMove);
	Tag_CantAttack  = FGameplayTag::RequestGameplayTag(GGYGOTags::Restriction::CantAttack);
	Tag_CantDodge   = FGameplayTag::RequestGameplayTag(GGYGOTags::Restriction::CantDodge);
	Tag_CantJump    = FGameplayTag::RequestGameplayTag(GGYGOTags::Restriction::CantJump);
	Tag_CantInput   = FGameplayTag::RequestGameplayTag(GGYGOTags::Restriction::CantInput);
}

void FGASArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	if (!ASC) return;

	// ============================================================
	// 优先级 1：死亡状态 → 全封锁
	// ============================================================
	if (ASC->HasMatchingGameplayTag(Tag_Dead))
	{
		RuntimeData.bBlockMove   = true;
		RuntimeData.bBlockAttack = true;
		RuntimeData.bBlockDodge  = true;
		RuntimeData.bBlockInput  = true;
		return; // 死亡是最高优先级，不再检查其他
	}

	// ============================================================
	// 优先级 2：眩晕状态 → 封锁动作（保留输入处理）
	// ============================================================
	if (ASC->HasMatchingGameplayTag(Tag_Stunned))
	{
		RuntimeData.bBlockMove   = true;
		RuntimeData.bBlockAttack = true;
		RuntimeData.bBlockDodge  = true;
		// 眩晕不阻断输入处理，只是禁止动作执行
		// （与 GE_BlockAll 的 LimitFlags 不同，这里只设动作标记）
	}

	// ============================================================
	// 优先级 3：Restriction.* Tag → 逐位设置（通用限制层）
	//    这些 Tag 由 GE 施加（如 HitStun 的 GE_BlockCombat 带 CantMove/CantAttack）
	//    与状态 Tag 叠加生效：Stunned + CantMove = 还是 true（无冲突）
	// ============================================================

	if (ASC->HasMatchingGameplayTag(Tag_CantMove))
		RuntimeData.bBlockMove = true;

	if (ASC->HasMatchingGameplayTag(Tag_CantAttack))
		RuntimeData.bBlockAttack = true;

	if (ASC->HasMatchingGameplayTag(Tag_CantDodge))
		RuntimeData.bBlockDodge = true;

	if (ASC->HasMatchingGameplayTag(Tag_CantJump))
	{
		// CantJump 目前没有独立的 RuntimeData 字段，
		// 通过 bBlockMove 间接禁止跳跃（MotionDriver 层判断）
		RuntimeData.bBlockMove = true;
	}

	if (ASC->HasMatchingGameplayTag(Tag_CantInput))
		RuntimeData.bBlockInput = true;
}
