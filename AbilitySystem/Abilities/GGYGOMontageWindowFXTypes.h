/** @file GGYGOMontageWindowFXTypes.h @brief Montage 作者窗的显式表现配置，保留既有反射身份 */
#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "FX/Tasks/GGYGOAbilityTask_PlayNiagaraEffect.h"
#include "GameplayTagContainer.h"
#include "GGYGOMontageWindowFXTypes.generated.h"

class UNiagaraSystem;

/** FX 使用公共 Task 签出的同一作者声明；保留已有 C++ 类型名及窗口坐标字段。 */
using FGGYGOMontageFXWindowDeclaration = FGGYGOMontageWindowDeclaration;

/** 未接表现的段显式关闭；启用时必须具备真实作者窗口和完整播放配置。 */
UENUM(BlueprintType)
enum class EGGYGOComboFXMode : uint8
{
	Disabled,
	MontageWindow
};

/** 段的表现配置；窗口来自 Montage，Niagara 组件及尾粒子释放由 FX Task 执行。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOComboStepFX
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	EGGYGOComboFXMode Mode = EGGYGOComboFXMode::Disabled;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	TObjectPtr<UNiagaraSystem> System = nullptr;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX", meta = (Categories = "Event.Montage"))
	FGameplayTag BeginEventTag;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX", meta = (Categories = "Event.Montage"))
	FGameplayTag EndEventTag;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	EGGYGONiagaraEffectSpace Space = EGGYGONiagaraEffectSpace::Unspecified;
	/** None 明确使用原 Mesh 根；非空必须存在，不回落到根。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	FName SocketName = NAME_None;
	/** 相对原 Mesh/socket。World 在真实开始事件中只组合一次；Attached 交原挂点跟随。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	FTransform LocalTransform = FTransform::Identity;
	/** 作者窗关闭、原生自然混出及正常结束的退出选择。
	 *  Deactivate 模式明确将尾粒子移交 Niagara AutoDestroy，后续取消不再追踪该尾粒子。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	EGGYGONiagaraEffectStopMode NormalStopMode = EGGYGONiagaraEffectStopMode::Unspecified;
	/** 换段、取消及失败的退出选择；同样须显式确认尾粒子是否移交。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	EGGYGONiagaraEffectStopMode AbortStopMode = EGGYGONiagaraEffectStopMode::Unspecified;
	/** 远端恢复 Age 的显式模拟步长；零表示尚未配置，不作为合法播放参数。
	 * 固定步长 Niagara 使用资产真实步长，本值仍须由作者明确填写。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX|Remote", meta = (ClampMin = "0.0", Units = "s"))
	float RemoteSeekDeltaSeconds = 0.0f;
	/** 远端单次原生追赶的显式时间预算；未配置时拒绝启用窗口表现。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX|Remote", meta = (ClampMin = "0.0", Units = "s"))
	float RemoteMaxSimTimeSeconds = 0.0f;
};

