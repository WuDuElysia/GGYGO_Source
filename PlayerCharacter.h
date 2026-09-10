/**
 * @file PlayerCharacter.h
 * @brief 旧玩家角色 —— 过渡壳，只为兼容已有蓝图而存在
 *
 * 与 `ABaseCharacter` 同理：本类存在的唯一理由是 `BP_Player` 之类的蓝图
 * 以它为父类，而 `Content/` 不在版本控制内，删掉无法回退。
 *
 * ## 输入走 APawn 的标准接口
 * 回调直接调用 `AddMovementInput` 与 `AddControllerYawInput` /
 * `AddControllerPitchInput`，输入因此进入 CMC 的 `Acceleration`，
 * 被 `FSavedMove_Character` 保存并获得网络预测。
 * 自建输入缓冲则做不到这一点，CMC 看不见的输入无法参与预测。
 *
 * ## 不要在这里扩展输入
 * 输入绑定的正式归属是 `UGGYGOHeroComponent`，用 InputTag 把输入与 GA 解耦。
 * 本类只让现有蓝图仍能操作角色，硬编码的回调不应继续增加。
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

UCLASS(meta = (DeprecatedNode, DeprecationMessage = "输入绑定将由 UGGYGOHeroComponent 接管，本类仅为兼容已有蓝图保留。"))
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
	 * **当前无效果**。步态只有 Walk / Run 两档、靠持续行走时长升档，
	 * 没有"按住冲刺"这一档。绑定入口保留，语义待定。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_Sprint;

	/** 强制步行（Ctrl），Digital。**当前无效果**，理由同 `IA_Sprint`。 */
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
