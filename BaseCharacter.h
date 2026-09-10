/**
 * @file BaseCharacter.h
 * @brief 旧角色基类 —— 过渡壳，只为兼容已有蓝图而存在
 *
 * ## 这个类现在是什么
 * 一个**空壳**。它继承 `AGGYGOCharacterBase`，自己不再拥有任何系统：
 * 没有 ASC、没有 AttributeSet、没有运行时组件、不 Tick。
 * 全部实际功能由父类的三个组件（PawnExtension / Health / CMC）提供。
 *
 * 它保留的只有一批 `UFUNCTION` getter，全部转发到新的 `UGGYGOCharacterMovementComponent`。
 *
 * ## 为什么不直接删掉
 * 这个类是 `BP_Player` / `BP_Miyabi` 等蓝图资产的父类，而 `Content/` 目录
 * **不在版本控制内**。直接删除会让那些蓝图损坏，且无法用 git 回退。
 * 保留一个转发壳的代价（一层多余继承）远小于重建蓝图的代价。
 *
 * ## 迁移方式
 * 把蓝图的父类逐个改成 `AGGYGOCharacterBase`，全部改完后本文件与
 * `PlayerCharacter` 就可以删除。蓝图里调用的 getter 在父类上没有同名函数，
 * 改父类时需要把那些节点换成从 `GetGGYGOMovementComponent()` 取值 ——
 * 所以本类的 getter 全部标注了对应的替代调用。
 *
 * ## 已失效的旧字段
 * `DefaultAbilities` / `DefaultEffects` / `CharacterConfig` 三个蓝图可配字段被删除。
 * 前两个对应的 `GiveDefaultAbilities()` / `ApplyDefaultEffects()` 在旧实现里
 * **从未被任何代码调用**（全模块零调用方），所以配了也没生效，删除不改变行为。
 * 能力授予现在走 `UGGYGOPawnData::AbilitySets`。
 * `CharacterConfig` 的唯一读取方是已退役的移动 Pipeline，移动参数现在走
 * `UGGYGOPawnData::MovementSet`。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "Character/GGYGOCharacterBase.h"
#include "CoreMinimal.h"

#include "BaseCharacter.generated.h"

class UObject;
struct FFrame;

UCLASS(meta = (DeprecatedNode, DeprecationMessage = "请把蓝图父类改为 AGGYGOCharacterBase，本类仅为兼容已有蓝图保留。"))
class GGYGO_API ABaseCharacter : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	ABaseCharacter();

	// ============================================================
	// 兼容用 getter —— 全部转发到 UGGYGOCharacterMovementComponent
	//
	// 这些函数在旧实现里读的是 FRuntimeData，那个数据模型已随 Pipeline 退役。
	// 取值语义尽量保持一致，不一致处逐个注明。
	// ============================================================

	/** 当前水平速度（cm/s）。替代调用：`GetGGYGOMovementComponent()->GetHorizontalSpeed()`。 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetCurrentSpeed() const;

	/**
	 * 速度相对角色朝向的角度（度，0 为正前，+90 为正右）。
	 * 替代调用：`GetGGYGOMovementComponent()->GetLocalVelocityAngle()`。
	 *
	 * 旧实现在静止时返回 0，这里保持一致。
	 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetMoveAngle() const;

	/** 是否在移动（水平速度超过 10 cm/s）。替代调用：`GetGGYGOMovementComponent()->IsMovingHorizontally()`。 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsMoving() const;

	/**
	 * 是否站在地面上。替代调用：`GetGGYGOMovementComponent()->IsMovingOnGround()`。
	 *
	 * **行为已修正**：旧实现读 `RuntimeData.Movement.bIsGrounded`，
	 * 而那个字段没有任何运行时写入方，恒为 true。现在返回真实值。
	 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsGrounded() const;

	/**
	 * 当前步态。替代调用：`GetGGYGOMovementComponent()->GetResolvedGait()`。
	 *
	 * **返回类型已变**：从旧的 `EMovementGait` 改为 `EGGYGOGait`。
	 * 两者取值语义相同（None / Walk / Run），但蓝图里若直接比较枚举需要重连。
	 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	EGGYGOGait GetResolvedGait() const;

	/**
	 * 接收 `CanYaw` AnimNotify。
	 *
	 * 当前是空实现：消费方（TurnBack 相位机）随移动 Pipeline 退役，
	 * 阶段 6 在 CMC 内重建后这里改为转发给 CMC。
	 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	void NotifyCanYaw();
};
