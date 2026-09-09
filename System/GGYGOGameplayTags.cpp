/**
 * @file GGYGOGameplayTags.cpp
 * @brief 原生 GameplayTag 定义
 *
 * `UE_DEFINE_GAMEPLAY_TAG` 在模块加载时把 Tag 注册进 `UGameplayTagsManager`。
 * 因此这些 Tag 不需要写进 `Config/DefaultGameplayTags.ini`；
 * 反过来说，同一个 Tag 名不能同时出现在 ini 和本文件中，否则重复定义。
 */
#include "System/GGYGOGameplayTags.h"

#include "GameplayTagsManager.h"

namespace GGYGOGameplayTags
{
	// ===== 一、InputTag =====
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Move, "InputTag.Move");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Look_Mouse, "InputTag.Look.Mouse");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Look_Stick, "InputTag.Look.Stick");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Sprint, "InputTag.Sprint");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_ForceWalk, "InputTag.ForceWalk");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Attack_Light, "InputTag.Attack.Light");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Attack_Heavy, "InputTag.Attack.Heavy");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Dodge, "InputTag.Dodge");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Skill, "InputTag.Skill");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Ultimate, "InputTag.Ultimate");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_SwitchCharacter_Next, "InputTag.SwitchCharacter.Next");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_SwitchCharacter_Prev, "InputTag.SwitchCharacter.Prev");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Jump, "InputTag.Jump");
	UE_DEFINE_GAMEPLAY_TAG(InputTag_Interact, "InputTag.Interact");

	// ===== 二、Intent =====
	UE_DEFINE_GAMEPLAY_TAG(Intent_Attack_Light, "Intent.Attack.Light");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Attack_Heavy, "Intent.Attack.Heavy");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Attack_Charged, "Intent.Attack.Charged");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Attack_Air, "Intent.Attack.Air");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Dodge_Directional, "Intent.Dodge.Directional");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Dodge_Back, "Intent.Dodge.Back");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Skill, "Intent.Skill");
	UE_DEFINE_GAMEPLAY_TAG(Intent_Ultimate, "Intent.Ultimate");
	UE_DEFINE_GAMEPLAY_TAG(Intent_SwitchCharacter, "Intent.SwitchCharacter");

	// ===== 三、AbilityGroup =====
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_Attack, "AbilityGroup.Attack");
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_Skill, "AbilityGroup.Skill");
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_Ultimate, "AbilityGroup.Ultimate");
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_Dodge, "AbilityGroup.Dodge");
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_HitReact, "AbilityGroup.HitReact");
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_Passive, "AbilityGroup.Passive");
	UE_DEFINE_GAMEPLAY_TAG(AbilityGroup_Team, "AbilityGroup.Team");

	// ===== 四、Ability =====
	UE_DEFINE_GAMEPLAY_TAG(Ability_Melee, "Ability.Melee");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Dodge, "Ability.Dodge");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Skill, "Ability.Skill");
	UE_DEFINE_GAMEPLAY_TAG(Ability_UltraSkill, "Ability.UltraSkill");
	UE_DEFINE_GAMEPLAY_TAG(Ability_IsChanneling, "Ability.IsChanneling");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Behavior_SurvivesDeath, "Ability.Behavior.SurvivesDeath");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_IsDead, "Ability.ActivateFail.IsDead");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_Cooldown, "Ability.ActivateFail.Cooldown");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_Cost, "Ability.ActivateFail.Cost");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_TagsBlocked, "Ability.ActivateFail.TagsBlocked");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_TagsMissing, "Ability.ActivateFail.TagsMissing");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_Networking, "Ability.ActivateFail.Networking");
	UE_DEFINE_GAMEPLAY_TAG(Ability_ActivateFail_ActivationGroup, "Ability.ActivateFail.ActivationGroup");

	// ===== 五、State =====
	UE_DEFINE_GAMEPLAY_TAG(State_Idle, "State.Idle");
	UE_DEFINE_GAMEPLAY_TAG(State_Moving, "State.Moving");
	UE_DEFINE_GAMEPLAY_TAG(State_RunStart, "State.RunStart");
	UE_DEFINE_GAMEPLAY_TAG(State_RunLoop, "State.RunLoop");
	UE_DEFINE_GAMEPLAY_TAG(State_RunEnd, "State.RunEnd");
	UE_DEFINE_GAMEPLAY_TAG(State_WalkLoop, "State.WalkLoop");
	UE_DEFINE_GAMEPLAY_TAG(State_SprintLoop, "State.SprintLoop");
	UE_DEFINE_GAMEPLAY_TAG(State_InAir, "State.InAir");
	UE_DEFINE_GAMEPLAY_TAG(State_Attacking, "State.Attacking");
	UE_DEFINE_GAMEPLAY_TAG(State_Dodging, "State.Dodging");
	UE_DEFINE_GAMEPLAY_TAG(State_HitStun, "State.HitStun");
	UE_DEFINE_GAMEPLAY_TAG(State_Stunned, "State.Stunned");
	UE_DEFINE_GAMEPLAY_TAG(State_Dead, "State.Dead");
	UE_DEFINE_GAMEPLAY_TAG(State_PoiseBreak, "State.PoiseBreak");
	UE_DEFINE_GAMEPLAY_TAG(State_Interacting, "State.Interacting");

	// ===== 六、Restriction =====
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantMove, "Restriction.CantMove");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantAttack, "Restriction.CantAttack");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantDodge, "Restriction.CantDodge");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantJump, "Restriction.CantJump");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantInput, "Restriction.CantInput");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantLookInput, "Restriction.CantLookInput");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_CantInteract, "Restriction.CantInteract");
	UE_DEFINE_GAMEPLAY_TAG(Restriction_ImmuneDamage, "Restriction.ImmuneDamage");

	// ===== 七、Gameplay =====
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_Damage, "Gameplay.Damage");
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_Damage_Immunity, "Gameplay.Damage.Immunity");
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_Damage_SelfDestruct, "Gameplay.Damage.SelfDestruct");
	UE_DEFINE_GAMEPLAY_TAG(Gameplay_PoiseDamage, "Gameplay.PoiseDamage");

	// ===== 八、Cooldown =====
	UE_DEFINE_GAMEPLAY_TAG(Cooldown_Evade, "Cooldown.Evade");
	UE_DEFINE_GAMEPLAY_TAG(Cooldown_Attack, "Cooldown.Attack");
	UE_DEFINE_GAMEPLAY_TAG(Cooldown_Skill, "Cooldown.Skill");
	UE_DEFINE_GAMEPLAY_TAG(Cooldown_SwitchCharacter, "Cooldown.SwitchCharacter");

	// ===== 九、GameplayEvent =====
	UE_DEFINE_GAMEPLAY_TAG(Event_PerfectEvade, "Event.PerfectEvade");
	UE_DEFINE_GAMEPLAY_TAG(Event_TakeDamage, "Event.TakeDamage");
	UE_DEFINE_GAMEPLAY_TAG(Event_Death, "Event.Death");
	UE_DEFINE_GAMEPLAY_TAG(Event_Respawn, "Event.Respawn");
	UE_DEFINE_GAMEPLAY_TAG(Event_Montage_HitWindowBegin, "Event.Montage.HitWindowBegin");
	UE_DEFINE_GAMEPLAY_TAG(Event_Montage_HitWindowEnd, "Event.Montage.HitWindowEnd");
	UE_DEFINE_GAMEPLAY_TAG(Event_Montage_ComboWindow, "Event.Montage.ComboWindow");
	UE_DEFINE_GAMEPLAY_TAG(Event_Montage_CancelPoint, "Event.Montage.CancelPoint");
	UE_DEFINE_GAMEPLAY_TAG(Event_HitConfirm, "Event.HitConfirm");

	// ===== 十、GameplayCue =====
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Hit_Flesh, "GameplayCue.Hit.Flesh");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Hit_Metal, "GameplayCue.Hit.Metal");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Hit_Stone, "GameplayCue.Hit.Stone");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Hit_Blocked, "GameplayCue.Hit.Blocked");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Hit_Critical, "GameplayCue.Hit.Critical");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_PoiseBreak, "GameplayCue.PoiseBreak");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Dodge_Afterimage, "GameplayCue.Dodge.Afterimage");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_SwitchCharacter, "GameplayCue.SwitchCharacter");

	// ===== 十一、SurfaceType =====
	UE_DEFINE_GAMEPLAY_TAG(SurfaceType_Flesh, "SurfaceType.Flesh");
	UE_DEFINE_GAMEPLAY_TAG(SurfaceType_Metal, "SurfaceType.Metal");
	UE_DEFINE_GAMEPLAY_TAG(SurfaceType_Stone, "SurfaceType.Stone");
	UE_DEFINE_GAMEPLAY_TAG(SurfaceType_Wood, "SurfaceType.Wood");

	// ===== 十二、InitState =====
	UE_DEFINE_GAMEPLAY_TAG(InitState_Spawned, "InitState.Spawned");
	UE_DEFINE_GAMEPLAY_TAG(InitState_DataAvailable, "InitState.DataAvailable");
	UE_DEFINE_GAMEPLAY_TAG(InitState_DataInitialized, "InitState.DataInitialized");
	UE_DEFINE_GAMEPLAY_TAG(InitState_GameplayReady, "InitState.GameplayReady");

	// ===== 十三、SetByCaller =====
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_Damage, "SetByCaller.Damage");
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_Heal, "SetByCaller.Heal");
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_PoiseDamage, "SetByCaller.PoiseDamage");
	UE_DEFINE_GAMEPLAY_TAG(SetByCaller_StunDuration, "SetByCaller.StunDuration");

	// ===== 十四、Message =====
	UE_DEFINE_GAMEPLAY_TAG(Message_Damage, "Message.Damage");
	UE_DEFINE_GAMEPLAY_TAG(Message_PoiseBreak, "Message.PoiseBreak");
	UE_DEFINE_GAMEPLAY_TAG(Message_Elimination, "Message.Elimination");

	// ===== 十五、Cheat =====
	UE_DEFINE_GAMEPLAY_TAG(Cheat_GodMode, "Cheat.GodMode");
	UE_DEFINE_GAMEPLAY_TAG(Cheat_UnlimitedHealth, "Cheat.UnlimitedHealth");

	FGameplayTag FindTagByString(const FString& TagString, bool bMatchPartialString)
	{
		const UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		FGameplayTag Tag = Manager.RequestGameplayTag(FName(*TagString), /*ErrorIfNotFound=*/false);

		if (!Tag.IsValid() && bMatchPartialString)
		{
			// 子串搜索要遍历全部已注册 Tag，开销高。
			// 只用于控制台指令与调试工具，不要放进每帧路径。
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
