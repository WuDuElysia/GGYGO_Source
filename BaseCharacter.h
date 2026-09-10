/**
 * @file BaseCharacter.h
 * @brief 兼容层角色基类 —— 仅为已有蓝图资产而存在
 *
 * ## 这个类是什么
 * 一个转发壳。它继承 `AGGYGOCharacterBase` 但自己不拥有任何系统：
 * 没有 ASC、没有 AttributeSet、不 Tick。实际功能全部来自父类的三个组件
 * （PawnExtension / Health / CMC）。它保留的只有一批 `UFUNCTION` getter，
 * 转发到 `UGGYGOCharacterMovementComponent`。
 *
 * ## 为什么需要它
 * `BP_Player` / `BP_Miyabi` 等蓝图资产以本类为父类，而 `Content/` 目录
 * 不在版本控制内 —— 一旦删除本类，那些蓝图会损坏且无法用 git 回退。
 * 维持一层多余继承的代价远小于重建蓝图。
 *
 * ## 如何摆脱它
 * 把蓝图的父类逐个改成 `AGGYGOCharacterBase`，全部改完后本文件与
 * `PlayerCharacter` 即可删除。父类上没有下面这些同名 getter，
 * 改父类时需要把蓝图里的调用换成从 `GetGGYGOMovementComponent()` 取值 ——
 * 每个 getter 的注释都给出了对应的替代调用。
 *
 * ## 不要在这里加东西
 * 本类是待拆除的兼容层。新功能应加在 `AGGYGOCharacterBase` 或组件上 ——
 * 加在这里会让蓝图更难改父类，等于把兼容层的寿命拖得更长。
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
	// 转发到 UGGYGOCharacterMovementComponent 的 getter
	// ============================================================

	/** 当前水平速度（cm/s）。替代调用：`GetGGYGOMovementComponent()->GetHorizontalSpeed()`。 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetCurrentSpeed() const;

	/**
	 * 速度相对角色朝向的角度（度，0 为正前，+90 为正右）。静止时返回 0。
	 * 替代调用：`GetGGYGOMovementComponent()->GetLocalVelocityAngle()`。
	 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	float GetMoveAngle() const;

	/** 是否在移动（水平速度超过 10 cm/s）。替代调用：`GetGGYGOMovementComponent()->IsMovingHorizontally()`。 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsMoving() const;

	/** 是否站在地面上。替代调用：`GetGGYGOMovementComponent()->IsMovingOnGround()`。 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool IsGrounded() const;

	/** 当前步态。替代调用：`GetGGYGOMovementComponent()->GetResolvedGait()`。 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	EGGYGOGait GetResolvedGait() const;

	/**
	 * 接收 `CanYaw` AnimNotify。
	 *
	 * 目前是空实现：转身相位机尚未在 CMC 内建立，没有消费方。
	 */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	void NotifyCanYaw();
};
