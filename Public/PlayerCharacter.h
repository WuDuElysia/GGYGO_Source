/**
 * @file PlayerCharacter.h
 * @brief 玩家角色 - 输入绑定和摄像机控制
 *
 * 负责接收 Enhanced Input 回调，将原始输入传给 InputPipeline。
 * 不再直接操作移动系统，所有输入只写 InputPipeline。
 */

#pragma once

#include "CoreMinimal.h"
#include "BaseCharacter.h"
#include "InputActionValue.h"
#include "PlayerCharacter.generated.h"

class UInputAction;

UCLASS()
class GGYGO_API APlayerCharacter : public ABaseCharacter
{
	GENERATED_BODY()

public:
	APlayerCharacter();

protected:
	virtual void BeginPlay() override;

	// ============================================================
	// 摄像机配置（蓝图可调）
	// ============================================================

	/** 摄像机俯仰角下限 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float PitchMin = -40.f;

	/** 摄像机俯仰角上限 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float PitchMax = 60.f;

	// ============================================================
	// Enhanced Input 绑定
	// IA 资产在蓝图子类（BP_Player）里配置
	// ============================================================

	/** 移动输入（WASD / 左摇杆），值类型 Axis2D */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	UInputAction* IA_Move;

	/** 视角输入（鼠标 / 右摇杆），值类型 Axis2D */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	UInputAction* IA_Look;

	/** 冲刺（Shift），值类型 Digital/Bool */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	UInputAction* IA_Sprint;

	/** 强制步行（Ctrl），值类型 Digital/Bool */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	UInputAction* IA_ForceWalk;

	// ============================================================
	// 输入回调
	// ============================================================

	/** 移动输入回调，写入 InputPipeline */
	void OnMoveInput(const FInputActionValue& Value);

	/** 移动输入松开回调，通知 InputPipeline 清零 */
	void OnMoveCompleted(const FInputActionValue& Value);

	/** 视角输入回调，写入 InputPipeline */
	void OnLookInput(const FInputActionValue& Value);

	/** 冲刺按住/松开 */
	void OnSprintInput(const FInputActionValue& Value);

	/** 强制步行按住/松开 */
	void OnForceWalkInput(const FInputActionValue& Value);

	// ============================================================
	// 输入绑定入口
	// ============================================================

	/** UE 框架在 Possess 时自动调用 */
	virtual void SetupPlayerInputComponent(
		UInputComponent* PlayerInputComponent) override;
};
