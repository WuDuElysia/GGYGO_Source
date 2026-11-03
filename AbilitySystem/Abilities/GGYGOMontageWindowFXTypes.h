/** @file GGYGOMontageWindowFXTypes.h @brief Montage 作者窗的显式表现配置，保留既有反射身份 */
#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "FX/GGYGOFXFieldSettings.h"
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

/** 段的表现配置；同一作者窗和原 Task 驱动完整独立单元，各单元保留远端发布身份。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOComboStepFX
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	EGGYGOComboFXMode Mode = EGGYGOComboFXMode::Disabled;
	/** 原资产的单 System 模式；与非空 SystemUnits 互斥，保留序列化身份。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	TObjectPtr<UNiagaraSystem> System = nullptr;
	/** 仅供原单 System 模式；独立单元分别填写自己的场材质合同。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX|Field")
	FGGYGOFXFieldSettings FieldSettings;
	/** 完整有序制作单元；每项独立编译及登记场材质，共用本窗口和退出策略。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo|FX")
	TArray<FGGYGONiagaraEffectUnit> SystemUnits;
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

