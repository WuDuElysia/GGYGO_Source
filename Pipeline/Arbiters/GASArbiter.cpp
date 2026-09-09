/**
 * @file GASArbiter.cpp
 * @brief GAS 状态仲裁器实现
 *
 * 仲裁顺序（每帧执行，在 `FArbiterPipeline::Process` 清零 bBlock* 之后）：
 *   1. 死亡 → 全封锁并提前返回
 *   2. 眩晕 → 封锁全部动作
 *   3. 逐个 Restriction.* Tag → 设置对应 bBlock 标记
 *
 * 本函数只把 false 改成 true，不负责清零 —— 那是 ArbiterPipeline 的职责。
 */
#include "Pipeline/Arbiters/GASArbiter.h"

#include "AbilitySystemComponent.h"
#include "Data/Runtime/RuntimeData.h"
#include "GameplayTagContainer.h"
#include "System/GGYGOGameplayTags.h"

void FGASArbiter::Init(UAbilitySystemComponent* InASC)
{
	ASC = InASC;

	// 这里曾经用 FGameplayTag::RequestGameplayTag 预解析七个 Tag 并缓存成员，
	// 目的是避免每帧字符串查找。改用原生 Tag 后这层缓存没有意义了：
	// 原生 Tag 是模块加载时注册的全局变量，直接读即可。
}

void FGASArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	if (!ASC)
	{
		return;
	}

	// ============================================================
	// 优先级 1：死亡 —— 最高优先级，直接返回不再检查其它
	// ============================================================
	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dead))
	{
		RuntimeData.Arbiter.bBlockMove = true;
		RuntimeData.Arbiter.bBlockAttack = true;
		RuntimeData.Arbiter.bBlockDodge = true;
		return;
	}

	// ============================================================
	// 优先级 2：眩晕 —— 封锁动作但不阻断输入采集
	// ============================================================
	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Stunned))
	{
		RuntimeData.Arbiter.bBlockMove = true;
		RuntimeData.Arbiter.bBlockAttack = true;
		RuntimeData.Arbiter.bBlockDodge = true;
	}

	// ============================================================
	// 优先级 3：Restriction.* 逐项翻译
	// 这些 Tag 由 GE 施加（受击硬直、无敌帧、演出锁定等），
	// 与上面的状态 Tag 叠加生效，不互相覆盖。
	// ============================================================
	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantMove))
	{
		RuntimeData.Arbiter.bBlockMove = true;
	}

	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantAttack))
	{
		RuntimeData.Arbiter.bBlockAttack = true;
	}

	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantDodge))
	{
		RuntimeData.Arbiter.bBlockDodge = true;
	}

	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantJump))
	{
		// RuntimeData 没有独立的跳跃阻断字段，暂时借用 bBlockMove。
		// 这是个已知的不精确之处：它会连带禁掉地面移动。
		// 移动层重建后由 CMC 直接查 Restriction.CantJump，不再有这个问题。
		RuntimeData.Arbiter.bBlockMove = true;
	}

	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantInput))
	{
		// RuntimeData 没有 bBlockInput 字段，因此这个 Tag 当前**没有任何实际效果**。
		// 输入屏蔽的正确落点是 ASC 的 TAG_GGYGO_Gameplay_AbilityInputBlocked
		// （见 UGGYGOAbilitySystemComponent::ProcessAbilityInput），
		// 以及移动层重建后 CMC 对移动输入的拒收。
	}
}
