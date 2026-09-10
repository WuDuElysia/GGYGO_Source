/**
 * @file GGYGOGameplayTags.h
 * @brief 项目 GameplayTag 的统一原生声明
 *
 * ## 为什么用原生 Tag
 * 另一种常见做法是用 `namespace + static const FName` 存字符串，
 * 运行时靠 `FGameplayTag::RequestGameplayTag` 解析，再手工缓存一份。
 * 那条链路有三个问题：每次解析都是字符串查找；Tag 拼错只能在运行时发现；
 * 缓存结构需要人工维护，加一个 Tag 要改三处（FName 常量、缓存字段、Init 里的赋值）。
 *
 * 原生 Tag 在模块加载时注册，编译期就能引用符号，写错编译不过，也不需要缓存。
 *
 * ## 与 ini 的关系
 * 本文件声明的 Tag **不能**同时出现在 `Config/DefaultGameplayTags.ini` 里，否则重复定义。
 * 反过来说，蓝图资产引用 Tag 是按名字解析的，原生注册的 Tag 蓝图同样找得到，
 * 所以把 ini 里的 Tag 迁到这里不会断蓝图引用。
 *
 * ## 就近声明的例外
 * 有三个 Tag 与特定类强绑定，声明在各自头文件里而不是这里，这是刻意的（Lyra 同样做法）：
 * - `TAG_GGYGO_Gameplay_AbilityInputBlocked` → `AbilitySystem/GGYGOAbilitySystemComponent.h`
 * - `TAG_GGYGO_Ability_SimpleFailureMessage` → `AbilitySystem/Abilities/GGYGOAbilityFailureMessages.h`
 * - `TAG_GGYGO_Ability_PlayMontageFailureMessage` → 同上
 */
#pragma once

#include "NativeGameplayTags.h"

class FString;

namespace GGYGOGameplayTags
{
	/**
	 * 按字符串查找已注册的 Tag。
	 * @param TagString           完整 Tag 名，例如 "Ability.ActivateFail.Cost"。
	 * @param bMatchPartialString 为 true 时在精确匹配失败后退化为子串搜索，仅用于调试与作弊指令。
	 * @return 命中的 Tag；未命中时返回无效 Tag，调用方须自行判断 `IsValid()`。
	 */
	GGYGO_API FGameplayTag FindTagByString(const FString& TagString, bool bMatchPartialString = false);

	// ============================================================
	// 一、InputTag —— 输入到能力的映射键
	//
	// `UGGYGOInputConfig` 把 UInputAction 映射到这些 Tag，
	// `UGGYGOAbilitySet` 授予能力时把 Tag 写进 AbilitySpec 的动态源标签，
	// ASC 的 `AbilityInputTagPressed` 用**精确匹配**找到对应能力。
	//
	// Move / Look 走 `BindNativeAction` 直连，不进 GAS（与 Lyra 一致）。
	// ============================================================

	/**
	 * InputTag 的根标签。
	 *
	 * 本身不作为具体输入使用，存在的意义是让"这个 Tag 是不是一个输入标签"
	 * 可以用一次层级匹配回答，而不必比较字符串前缀。
	 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag);

	/** 移动轴。直连 `AddMovementInput`，不激活任何能力。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Move);
	/** 鼠标视角。直连 Controller。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Look_Mouse);
	/** 手柄右摇杆视角。与鼠标分开是因为需要不同的灵敏度曲线。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Look_Stick);
	/** 冲刺。当前由步态权威消费，不是能力。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Sprint);
	/** 强制步行。同上。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_ForceWalk);

	/** 轻攻击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Attack_Light);
	/** 重攻击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Attack_Heavy);
	/** 闪避。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Dodge);
	/** 技能。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Skill);
	/** 大招。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Ultimate);
	/** 切换到下一个队伍角色。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_SwitchCharacter_Next);
	/** 切换到上一个队伍角色。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_SwitchCharacter_Prev);
	/** 跳跃。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Jump);
	/** 交互。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Interact);

	// ============================================================
	// 二、Intent —— 意图层产出的语义动作
	//
	// 与 InputTag 的区别：一个 InputTag 可以按上下文解释成多个 Intent。
	// 例如 `InputTag.Attack.Light` 在地面是 `Intent.Attack.Light`、
	// 空中是 `Intent.Attack.Air`、长按变 `Intent.Attack.Charged`。
	//
	// 意图层只产出这些 Tag 与对应的 `AbilityInputTag`，不决定最终执行什么。
	// ============================================================

	/** 地面轻攻击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Attack_Light);
	/** 地面重攻击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Attack_Heavy);
	/** 蓄力攻击（长按达到阈值）。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Attack_Charged);
	/** 空中攻击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Attack_Air);
	/** 定向闪避（有方向输入）。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Dodge_Directional);
	/** 后跳（无方向输入）。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Dodge_Back);
	/** 技能。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Skill);
	/** 大招。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_Ultimate);
	/** 切人。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Intent_SwitchCharacter);

	// ============================================================
	// 三、AbilityGroup —— 组仲裁的组身份
	//
	// 配在 GA 资产的 `GroupTag` 上。组规则（Coexist / SingleInstance /
	// SingleInstanceQueued）配在独立 DataAsset 上，不在 Tag 里表达。
	// ============================================================

	/** 普攻组。建议规则 `SingleInstanceQueued`，让连段排队而不是互相打断。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_Attack);
	/** 技能组。建议规则 `SingleInstance`。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_Skill);
	/** 大招组。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_Ultimate);
	/** 闪避组。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_Dodge);
	/** 受击反应组。这一组更适合先到先得（`bNewcomerWinsOnTie = false`）。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_HitReact);
	/** 被动组。建议规则 `Coexist`。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_Passive);
	/** 队伍级能力组（切人、连携），归 PlayerState 上的队伍 ASC。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(AbilityGroup_Team);

	// ============================================================
	// 四、Ability —— 能力自身的资产标签与失败原因
	// ============================================================

	/** 近战攻击能力。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Melee);
	/** 闪避能力。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Dodge);
	/** 技能。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Skill);
	/** 大招。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_UltraSkill);
	/** 引导中（持续施法期间持有）。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_IsChanneling);

	/** 标记该能力在拥有者死亡时不被清除，用于死亡演出本身等能力。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Behavior_SurvivesDeath);

	// 以下由 `CanActivateAbility` 与 Tag 需求检查写入 `OptionalRelevantTags`，
	// 再交给失败反馈（文本 / Montage）消费。

	/** 角色已死亡。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_IsDead);
	/** 冷却中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_Cooldown);
	/** 资源不足。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_Cost);
	/** 被阻断 Tag 拦截。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_TagsBlocked);
	/** 缺少必需 Tag。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_TagsMissing);
	/** 网络角色不满足条件。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_Networking);
	/** 被组仲裁拒绝（优先级不足或跨组 Exclusive 压制），重试没有意义。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_ActivationGroup);

	/**
	 * 被组仲裁拒绝，但**值得重试** —— 组规则是 `SingleInstanceQueued` 且组内正有实例在跑。
	 *
	 * 与上一个分开是为了让意图层能只凭失败原因决定去留：
	 * 收到这个 Tag 就把请求留在输入缓冲里等组空出，收到上一个就直接丢弃。
	 * 否则意图层得反查 GA 的 GroupTag 再查配置表，等于把仲裁逻辑抄第二遍。
	 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_ActivationGroupQueued);

	// ============================================================
	// 五、State —— 状态身份
	//
	// 由能力或 GE 施加。**注意**：locomotion 相关的状态（Idle / Run* / Walk*）
	// 在移动层重建后应改由 CMC 数据推导，而不是靠 Tag 表达；
	// 这里保留它们是为了不断开现有 AnimBP 与蓝图资产的引用。
	// ============================================================

	/** 待机。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Idle);
	/** 移动中（笼统状态，旧状态机用）。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Moving);
	/** 跑步启动。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_RunStart);
	/** 跑步循环。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_RunLoop);
	/** 跑步停止。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_RunEnd);
	/** 行走循环。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_WalkLoop);
	/** 冲刺循环。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_SprintLoop);
	/** 空中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_InAir);
	/** 攻击中。攻击 GA 激活期间持有，ZZZAnim 读它切上半身层。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Attacking);
	/** 闪避中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dodging);
	/** 受击硬直。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_HitStun);
	/** 眩晕。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Stunned);
	/**
	 * 正在死亡：生命归零、死亡演出进行中。
	 *
	 * 与 `State_Dead` 分开是因为这段区间的规则不同 ——
	 * 此时角色仍在场、仍需要播动画、仍可能被追打，但不该再响应输入或被再次击杀。
	 * 只有一个 Tag 的话就无法表达"演出中"与"已收尾"的差别。
	 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dying);
	/** 已死亡，演出已收尾。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);
	/** 韧性被击破，处于破韧硬直中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_PoiseBreak);
	/** 交互中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Interacting);

	// ============================================================
	// 六、Restriction —— 功能限制
	//
	// 由 GE 施加，消费方直接查 Tag，不经过中间的 bool 标记翻译层。
	// `CantMove` 由 CMC 在 `GetMaxSpeed()` 里查询；
	// 攻击/闪避的限制由 GA 的 `ActivationBlockedTags` 表达。
	// ============================================================

	/** 不能移动。攻击类 GA 激活期间施加，防止 locomotion 位移与动作位移叠加。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantMove);
	/** 不能攻击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantAttack);
	/** 不能闪避。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantDodge);
	/** 不能跳跃。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantJump);
	/** 禁止全部输入。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantInput);
	/** 禁止视角控制。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantLookInput);
	/** 禁止交互。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_CantInteract);
	/**
	/**
	 * 免疫伤害。**不要在新代码里用这个**，用 `Gameplay_Damage_Immunity`。
	 *
	 * 两个 Tag 语义重复，而 `UGGYGOHealthSet` 的免疫判定只认那一个。
	 * 本 Tag 仅因被现有蓝图 GE 资产（如 GE_Invincible）引用而存在，
	 * 删除会断引用；它对伤害计算没有任何实际效果。
	 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Restriction_ImmuneDamage);

	// ============================================================
	// 七、Gameplay —— 伤害与韧性
	// ============================================================

	/** 普通伤害来源标记。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage);
	/** 免疫普通伤害。闪避无敌帧施加此 Tag，`UGGYGOHealthSet` 认这一个。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage_Immunity);
	/** 自毁 / 处死类伤害，绕过免疫与开发期保命规则。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage_SelfDestruct);

	/**
	 * 死因标记：掉出世界。
	 *
	 * 只标记死法，不影响伤害计算（免疫穿透由 `Gameplay_Damage_SelfDestruct` 负责）。
	 * 用途是让死亡 Cue 与死亡消息能区分表现 —— 掉出世界不该播倒地动画。
	 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage_FellOutOfWorld);
	/** 削韧来源标记。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_PoiseDamage);

	// ============================================================
	// 八、Cooldown —— 冷却
	// ============================================================

	/** 闪避冷却。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Evade);
	/** 攻击冷却。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Attack);
	/** 技能冷却。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Skill);
	/** 切人冷却。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_SwitchCharacter);

	// ============================================================
	// 九、GameplayEvent —— 驱动 Ability 逻辑的事件
	//
	// 通过 `SendGameplayEventToActor` 发给特定 ASC，
	// GA 内用 `WaitGameplayEvent` 接收。与下面的 Message 不是一回事。
	// ============================================================

	/** 完美闪避成立。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_PerfectEvade);
	/** 受到伤害。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_TakeDamage);
	/** 死亡。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Death);
	/** 复活。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Respawn);
	/** 攻击判定窗口开启。由 Montage 上的 AnimNotify 发出。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Montage_HitWindowBegin);
	/** 攻击判定窗口关闭。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Montage_HitWindowEnd);
	/** 连段窗口开启，此时按键可衔接下一段。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Montage_ComboWindow);
	/** 可取消点，此后允许被其他动作打断。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Montage_CancelPoint);
	/** 命中确认。由命中判定组件发出。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_HitConfirm);

	// ============================================================
	// 十、GameplayCue —— 表现层触发键
	//
	// 由 GE 或 GA 触发，`GameplayCueNotify` 消费。
	// 命中类 Cue 按表面材质分流，材质 Tag 来自 `UGGYGOPhysicalMaterialWithTags`。
	// ============================================================

	/** 命中肉体。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Hit_Flesh);
	/** 命中金属。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Hit_Metal);
	/** 命中石质。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Hit_Stone);
	/** 被格挡。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Hit_Blocked);
	/** 暴击。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Hit_Critical);
	/** 破韧。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_PoiseBreak);
	/** 闪避残影。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Dodge_Afterimage);
	/** 切人特效。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_SwitchCharacter);

	// ============================================================
	// 十一、SurfaceType —— 命中表面
	//
	// 配在 `UGGYGOPhysicalMaterialWithTags` 资产上，
	// 命中时被并入 GE 的目标 Tag，供 Cue 分流与材质减伤使用。
	// ============================================================

	/** 肉体。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SurfaceType_Flesh);
	/** 金属。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SurfaceType_Metal);
	/** 石质。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SurfaceType_Stone);
	/** 木质。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SurfaceType_Wood);

	// ============================================================
	// 十二、InitState —— 组件初始化协调
	//
	// `IGameFrameworkInitStateInterface` 的状态链。
	// 名称与 Lyra 保持一致，便于对照其源码。
	// ============================================================

	/** 已生成。有有效 Pawn 即可进入。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InitState_Spawned);
	/** 数据就绪。要求有 PawnData，且有权威或本地控制时已被 possess。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InitState_DataAvailable);
	/** 数据已初始化。要求**所有** feature 都到达 DataAvailable。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InitState_DataInitialized);
	/** 可以玩了。此后才允许绑定输入。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(InitState_GameplayReady);

	// ============================================================
	// 十三、SetByCaller —— GE 幅度键
	// ============================================================

	/** 伤害数值。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_Damage);
	/** 治疗数值。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_Heal);
	/** 削韧数值。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_PoiseDamage);
	/** 硬直时长。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_StunDuration);

	// ============================================================
	// 十四、Message —— 跨系统广播 Verb
	//
	// 配合 `UGameplayMessageSubsystem` 发给**只读观察者**（UI、音频、统计）。
	// 与 Event 的区别：Event 驱动 Ability 逻辑且有明确目标，Message 只是通知且无目标。
	// ============================================================

	/** 伤害消息。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Message_Damage);
	/** 破韧消息。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Message_PoiseBreak);
	/** 击杀消息。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Message_Elimination);

	// ============================================================
	// 十五、Cheat —— 开发期作弊，仅非 Shipping 生效
	// ============================================================

	/** 免疫一切普通伤害。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cheat_GodMode);
	/** 生命值不会低于 1。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cheat_UnlimitedHealth);
}
