/** @file GGYGOComboTypes.h @brief 普攻段配置和单请求窗口状态 */
#pragma once

#include "CoreMinimal.h"
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
	/** INDEX_NONE 表示终段；首版仅允许指向后面的段，避免意外循环。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	int32 NextStepIndex = INDEX_NONE;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.01"))
	float PlayRate = 1.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float Damage = 20.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float PoiseDamage = 10.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FName TraceStartSocket;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FName TraceEndSocket;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "1.0"))
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
