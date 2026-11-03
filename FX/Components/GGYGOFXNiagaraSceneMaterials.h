/** @file GGYGOFXNiagaraSceneMaterials.h @brief 同NS私有MID/UserMaterial与有来源bounds租用；灯采样仍属于原角色宿主。 */
#pragma once

#include "CoreMinimal.h"
#include "NiagaraTypes.h"
#include "UObject/StrongObjectPtr.h"

class UGGYGOCharacterRenderComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UNiagaraComponent;
class UNiagaraSystem;
class UWorld;
class FNiagaraSystemInstanceController;

struct FGGYGOFXSceneMaterialBinding
{
	FNiagaraVariable Parameter;
	TStrongObjectPtr<UMaterialInterface> OriginalMaterial;
	TStrongObjectPtr<UMaterialInstanceDynamic> InstalledMaterial;
};

struct FGGYGOFXSceneBoundsBinding
{
	FName EmitterName;
	FBox SourceBounds = FBox(ForceInit);
	FBox OriginalBounds = FBox(ForceInit);
	FBox InstalledBounds = FBox(ForceInit);
	float WorldMarginCm = 0;
	bool bInstalled = false;
};

/** 此对象本身是本次资源身份；外调前持其shared引用，旧释放不能清同组件的后继租用。 */
struct FGGYGOFXSceneMaterialState
{
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGGYGOCharacterRenderComponent> Host;
	TWeakPtr<FNiagaraSystemInstanceController, ESPMode::ThreadSafe> Controller;
	TArray<FGGYGOFXSceneMaterialBinding> Materials;
	TArray<FGGYGOFXSceneBoundsBinding> Bounds;
	FDelegateHandle ReadinessSubscription;
	uint64 HostHandle = 0;
	uint64 InstanceId = 0;
	bool bInstalled = false;
	bool bActivationPending = false;
	bool bResetWhenReady = false;
	bool bRenderingSuppressed = false;
	bool bOriginalRendering = true;
	bool bUpdatingBounds = false;
	/** 原宿主登记会同步采样其它FX；保护同一登记调用栈，不是就绪缓存。 */
	bool bRegistering = false;
};

enum class EGGYGOFXSceneMaterialPreparation : uint8 { NotRequired, Prepared, Invalid };

namespace GGYGOFXNiagaraSceneMaterials
{
	/** 只认实际Mesh UserParamBinding和公开灯接口；缺必需绑定不借ExplicitMat启动。 */
	EGGYGOFXSceneMaterialPreparation Prepare(UNiagaraComponent& Component,
		TSharedPtr<FGGYGOFXSceneMaterialState>& OutState, FString& OutError);
	bool Install(UNiagaraComponent& Component, FGGYGOFXSceneMaterialState& State,
		TFunctionRef<bool()> IsOriginal, FString& OutError);
	bool UpdateBounds(UNiagaraComponent& Component, FGGYGOFXSceneMaterialState& State,
		TFunctionRef<bool()> IsOriginal, FString& OutError);
	void Restore(UNiagaraComponent& Component, FGGYGOFXSceneMaterialState& State, TFunctionRef<bool()> IsOriginal);
}
