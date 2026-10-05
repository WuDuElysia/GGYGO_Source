/** @file GGYGOComboTypes.h @brief 普攻段配置和单请求窗口状态 */
#pragma once

#include "CoreMinimal.h"
#include "Combat/HitDetection/GGYGOMeleeTraceShape.h"
#include "GGYGOComboTypes.generated.h"

class UAnimMontage;

/** 每段一个 Main→End Montage；时间窗口只在 Montage 上配置。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOComboStep
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FName MainSection = TEXT("Main");
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FName EndSection = TEXT("End");
	/** 原 Montage 动作曲线所在 Slot；必须显式配置，不选择第一个或默认 Slot。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|Motion")
	FName MotionSlotName = NAME_None;
	/** 原动画厘米位移的显式倍率；Avatar 实际缩放由 CMC 仅应用一次。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|Motion", meta = (ClampMin = "0.001"))
	float MotionTranslationScale = 1.0f;
	/** INDEX_NONE 表示终段；首版仅允许指向后面的段，避免意外循环。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	int32 NextStepIndex = INDEX_NONE;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.01"))
	float PlayRate = 1.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float Damage = 20.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float PoiseDamage = 10.0f;
	/** 本段唯一运行查询配置；空/非法配置明确失败，不从历史字段补齐。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FGGYGOMeleeTraceShape TraceShape;
	/** 保留旧资产/Struct Pin 的成员身份和序列化数据；不参与运行，须显式配置 TraceShape。
	 *  保留成员不代表旧 Make/Set Members/Break 图已迁移；迁移核验前不得删除或使用丢值标志。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ToolTip = "历史存值，不参与运行；请显式配置 TraceShape。"))
	FName TraceStartSocket;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ToolTip = "历史存值，不参与运行；请显式配置 TraceShape。"))
	FName TraceEndSocket;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "1.0", ToolTip = "历史存值，不参与运行；请在 TraceShape 中显式配置固定世界半径。"))
	float TraceRadius = 20.0f;
};

/** 一个动作段最多保留一个请求；动画时间负责开关窗，游戏时间负责请求有效期。 */
struct FGGYGOComboWindowState
{
	bool bOpen = false;
	bool bClosed = false;
	int32 PendingRequestId = 0;
	double ExpiresAt = 0.0;

	void Reset() { *this = FGGYGOComboWindowState(); }
	void Close() { bOpen = false; bClosed = true; PendingRequestId = 0; }
	bool Store(int32 RequestId, double Now, double Lifetime)
	{
		if (bClosed || RequestId <= 0) { return false; }
		PendingRequestId = RequestId;
		ExpiresAt = Now + FMath::Max(0.0, Lifetime);
		return true;
	}
	bool HasExpired(double Now) const { return PendingRequestId > 0 && Now > ExpiresAt; }
	int32 Consume(double Now)
	{
		if (HasExpired(Now)) { PendingRequestId = 0; }
		if (!bOpen || bClosed) { return 0; }
		const int32 Result = PendingRequestId;
		PendingRequestId = 0;
		return Result;
	}
};
