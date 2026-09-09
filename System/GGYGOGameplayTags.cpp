/**
 * @file GGYGOGameplayTags.cpp
 * @brief 原生 GameplayTag 定义
 *
 * `UE_DEFINE_GAMEPLAY_TAG` 在模块加载时把 Tag 注册进 `UGameplayTagsManager`，
 * 因此这些 Tag 不需要写进 `Config/DefaultGameplayTags.ini`；
 * 反过来说，同一个 Tag 名不能同时出现在 ini 和本文件中。
 */
#include "System/GGYGOGameplayTags.h"

#include "GameplayTagsManager.h"

namespace GGYGOGameplayTags
{
	// ===== Ability 激活失败原因 =====
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_IsDead, "Ability.ActivateFail.IsDead");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_Cooldown, "Ability.ActivateFail.Cooldown");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_Cost, "Ability.ActivateFail.Cost");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_TagsBlocked, "Ability.ActivateFail.TagsBlocked");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_TagsMissing, "Ability.ActivateFail.TagsMissing");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_Networking, "Ability.ActivateFail.Networking");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_ActivationGroup, "Ability.ActivateFail.ActivationGroup");

	// ===== Ability 行为标记 =====
	UE_DEFINE_GAMEPLAY_TAG(Ability_Behavior_SurvivesDeath, "Ability.Behavior.SurvivesDeath");

	// ===== 伤害与韧性 =====
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_Damage, "Gameplay.Damage");
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_Damage_Immunity, "Gameplay.Damage.Immunity");
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_Damage_SelfDestruct, "Gameplay.Damage.SelfDestruct");
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_PoiseDamage, "Gameplay.PoiseDamage");

	// ===== 状态 =====
	UE_DEFINE_GAMEPLAY_TAG(State_Dead, "State.Dead");
	UE_DEFINE_GAMEPLAY_TAG(State_PoiseBreak, "State.PoiseBreak");

	// ===== SetByCaller 幅度键 =====
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_Damage, "SetByCaller.Damage");
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_Heal, "SetByCaller.Heal");
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_PoiseDamage, "SetByCaller.PoiseDamage");

	// ===== 跨系统消息 Verb =====
	UE_DEFINE_GAMEPLAY_TAG(Message_Damage, "Message.Damage");
	UE_DEFINE_GAMEPLAY_TAG(Message_PoiseBreak, "Message.PoiseBreak");

	// ===== 开发期作弊 =====
	UE_DEFINE_GAMEPLAY_TAG(Cheat_GodMode, "Cheat.GodMode");
	UE_DEFINE_GAMEPLAY_TAG(Cheat_UnlimitedHealth, "Cheat.UnlimitedHealth");

	/**
	 * 按字符串查找 Tag。精确匹配优先；只有显式要求时才退化为子串搜索。
	 * 子串搜索会遍历全部已注册 Tag，开销高，只用于控制台指令和调试工具，不要放进每帧路径。
	 */
	FGameplayTag FindTagByString(const FString& TagString, bool bMatchPartialString)
	{
		const UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		FGameplayTag Tag = Manager.RequestGameplayTag(FName(*TagString), /*ErrorIfNotFound=*/false);

		if (!Tag.IsValid() && bMatchPartialString)
		{
			FGameplayTagContainer AllTags;
			Manager.RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/true);

			for (const FGameplayTag& TestTag : AllTags)
			{
				if (TestTag.ToString().Contains(TagString))
				{
					UE_LOG(LogTemp, Display, TEXT("在查找 '%s' 时找到了部分匹配的 Tag '%s'。"), *TagString, *TestTag.ToString());
					Tag = TestTag;
					break;
				}
			}
		}

		return Tag;
	}
}
