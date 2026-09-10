/**
 * @file PlayerCharacter.h
 * @brief 旧玩家角色 —— 过渡壳，只为兼容已有蓝图而存在
 *
 * 与 `ABaseCharacter` 同理：保留它是因为 `BP_Player` 之类的蓝图以它为父类，
 * 而 `Content/` 不在版本控制内，删掉无法回退。
 *
 * ## 与旧实现的区别
 * 输入不再经过 `ABaseCharacter` 的输入门面转发给纯 C++ `FInputPipeline`
 * （那条链路随移动 Pipeline 退役），改为直接调用 `APawn` 的标准输入接口：
 * `AddMovementInput` 与 `AddControllerYawInput` / `AddControllerPitchInput`。
 *
 * 这样输入就进入了 CMC 的 `Acceleration`，从而被 `FSavedMove_Character` 保存，
 * 具备网络预测能力 —— 旧链路把输入存在自己的双缓冲里，CMC 完全看不到。
 *
 * ## 正式方案
 * 输入绑定最终应由 `UGGYGOHeroComponent` 承担（阶段 7），
 * 用 InputTag 把输入与 GA 解耦，而不是在角色类里硬编码回调。
 * 本类只是让现有蓝图在过渡期仍能操作角色。
 */
#pragma once

#include "BaseCharacter.h"
#include "CoreMinimal.h"
#include "InputActionValue.h"

#include "PlayerCharacter.generated.h"

class UInputAction;
class UInputComponent;
class UObject;
struct FFrame;

UCLASS(meta = (DeprecatedNode, DeprecationMessage = "输入绑定将由 UGGYGOHeroComponent 接管（阶段 7），本类仅为兼容已有蓝图保留。"))
class GGYGO_API APlayerCharacter : public ABaseCharacter
{
	GENERATED_BODY()

public:
	APlayerCharacter();

protected:
	virtual void BeginPlay() override;

	/** UE 框架在 Possess 时自动调用。 */
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// ===== 摄像机配置 =====

	/** 摄像机俯仰角下限。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float PitchMin = -40.f;

	/** 摄像机俯仰角上限。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float PitchMax = 60.f;

	// ===== Enhanced Input 资产（在 BP_Player 里配置） =====

	/** 移动输入（WASD / 左摇杆），Axis2D。 */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_Move;

	/** 视角输入（鼠标 / 右摇杆），Axis2D。 */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_Look;

	/**
	 * 冲刺（Shift），Digital。
	 *
	 * **当前无效果**。新步态体系只有 Walk / Run 两档，升档靠持续行走时长，
	 * 没有"按住冲刺"这一档 —— 旧配置里的 `SprintSpeed` / `SprintMultiplier`
	 * 也从未被任何代码读取过。保留绑定入口，语义待阶段 7 输入层重建时确定。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_Sprint;

	/** 强制步行（Ctrl），Digital。**当前无效果**，同上。 */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_ForceWalk;

	// ===== 输入回调 =====

	/** 把 2D 摇杆输入按摄像机水平朝向解析成世界方向，交给 CMC。 */
	void OnMoveInput(const FInputActionValue& Value);

	/** 视角输入。 */
	void OnLookInput(const FInputActionValue& Value);

	/** 冲刺。当前空实现，见 `IA_Sprint` 说明。 */
	void OnSprintInput(const FInputActionValue& Value);

	/** 强制步行。当前空实现，见 `IA_ForceWalk` 说明。 */
	void OnForceWalkInput(const FInputActionValue& Value);
};
