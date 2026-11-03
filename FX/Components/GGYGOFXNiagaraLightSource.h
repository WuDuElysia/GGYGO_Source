/** @file GGYGOFXNiagaraLightSource.h @brief 只读原已完成CPU实例的Native Point灯，不持模拟或跨帧缓存。 */
#pragma once

#include "CoreMinimal.h"
#include "Character/GGYGOCharacterLightSources.h"

class UNiagaraSystem;
class FNiagaraSystemInstance;

namespace GGYGOFXNiagaraLightSource
{
	bool HasPointLightRenderer(const UNiagaraSystem& System);
	/** 真正无颜色材质的纯灯NS可声明NoCustomField；其它缺材质不能借此放行。 */
	bool HasOnlyPointLightRenderers(const UNiagaraSystem& System);
	/** 调用者须先收束原并行工作、返栈核原实例；返回后不保留DataSet指针。 */
	EGGYGOCharacterLightSourceReadiness ReadCompletedInstance(FNiagaraSystemInstance& Instance,
		const FTransform& ComponentToWorld, FGGYGOCharacterPointLightFrame& OutFrame, FString& OutError);
}
