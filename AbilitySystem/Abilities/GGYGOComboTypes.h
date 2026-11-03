/** @file GGYGOComboTypes.h @brief 玩家动作段配置和单请求窗口状态 */
#pragma once

#include "CoreMinimal.h"
#include "Combat/HitDetection/GGYGOMeleeTraceShape.h"
#include "AbilitySystem/Abilities/GGYGOMontageWindowFXTypes.h"
#include "GameplayTagContainer.h"
#include "GGYGOComboTypes.generated.h"

class UAnimMontage;

/** 一个作者点权限的模式：AuthoredNotify 由同名原生 Montage Notify 的原实例事实开门；
 *  Disabled 明确不开该门。攻击接续与移动打断各持一个该模式字段，互不推导、互不代开。 */
UENUM(BlueprintType)
enum class EGGYGOActionInterruptionMode : uint8
{
	AuthoredNotify,
	Disabled
};

/** 真正起段时的基础方向策略；同段重放只复用原段接受值，不重新执行本策略。 */
UENUM(BlueprintType)
enum class EGGYGOComboStepFacingPolicy : uint8
{
	CurrentFacing,
	QualifiedInput
};

/** 每段一个 Main→End Montage；同一 CMC 资源执行完整自然轨迹。
 *  攻击接续点与移动打断点是两个不同名的作者 Notify，各自开门、各自消费；Section 不隐式开门。 */
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
	/** 原 Montage 的 Main→End 动作曲线所在 Slot；必须显式配置，不选择第一个或默认 Slot。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|Motion")
	FName MotionSlotName = NAME_None;
	/** 原动画厘米位移的显式倍率；Avatar 实际缩放由 CMC 仅应用一次。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|Motion", meta = (ClampMin = "0.001"))
	float MotionTranslationScale = 1.0f;
	/** CMC 在原播放成功后的动作准入点一次采样／执行；动作自身的源 Yaw 仍完整保留。
	 * QualifiedInput 中真实无方向保持当前朝向，未评价／未就绪不是无方向。
	 * 默认 CurrentFacing 保留既有资产行为，角色作者显式选择逐段输入方向。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|Motion")
	EGGYGOComboStepFacingPolicy StartFacingPolicy = EGGYGOComboStepFacingPolicy::CurrentFacing;
	/** 已过攻击接续点后一次真实新攻击的接续目标；INDEX_NONE 显式关闭攻击接续。
	 *  可回前段，但无新请求不自动推进或循环。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	int32 NextStepIndex = INDEX_NONE;
	/** 攻击接续点模式；只授予 NextStepIndex 的一次真实新攻击，不授予移动打断。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	EGGYGOActionInterruptionMode InterruptionMode = EGGYGOActionInterruptionMode::AuthoredNotify;
	/** 攻击接续的原生 Montage Notify 单点；缺失/重名/不可达明确拒绝。
	 *  Disabled 必须显式填 None 并关闭 NextStepIndex，不以缺点配置自动关闭。
	 *  普通 Hit/Combo Window 不授予打断权限，新播放独立重新关门。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FName InterruptionNotifyName = TEXT("Event.Montage.CancelPoint");
	/** 移动打断点模式；默认 Disabled，旧序列化数据不会因此获得移动打断。
	 *  只由本字段与 MovementInterruptionNotifyName 决定，不从攻击点、ComboWindow 或 Section 推导。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	EGGYGOActionInterruptionMode MovementInterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	/** 移动打断的原生 Montage Notify 名；Disabled 必须为 None，AuthoredNotify 必须与攻击点不同名。
	 *  作者时间必须等于当前 Montage 按 EndSection 名解析的 Section::GetTime（Main 真实末端）；
	 *  原生触发可按 TriggerTimeOffset 提前，但只有原实例真实位置到达该末端后才开门。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FName MovementInterruptionNotifyName = NAME_None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.01"))
	float PlayRate = 1.0f;
	/** 旧绝对GE模式的显式数值；保留序列化身份，倍率模式不读取或换算此字段。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float Damage = 20.0f;
	/** 独立伤害倍率；-1表示未配置，只能用于明确的旧绝对GE模式。倍率模式要求有限非负值。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float DamageMultiplier = -1.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float PoiseDamage = 10.0f;
	/** 本段唯一运行查询配置；空/非法配置明确失败，不从历史字段补齐。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	FGGYGOMeleeTraceShape TraceShape;
	/** 真实命中成立后发送的本段 Cue；None 显式不发送，不从 GA 历史字段补齐。
	 *  声音、参数及组合表现由对应 Cue 资产配置，不作为 Main 开始时的技能声。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|Hit", meta = (Categories = "GameplayCue.Hit"))
	FGameplayTag HitCueTag;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	FGGYGOComboStepFX FX;
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
