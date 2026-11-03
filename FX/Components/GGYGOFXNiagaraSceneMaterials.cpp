/** @file GGYGOFXNiagaraSceneMaterials.cpp @brief 消费原生UserMaterial与Shader公开输入，不选灯、不持第二模拟或时钟。 */
#include "FX/Components/GGYGOFXNiagaraSceneMaterials.h"

#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "Misc/ScopeExit.h"

namespace
{
	bool NeedsSceneInputs(UMaterialInterface& Material)
	{
		FLinearColor Vector;
		float Scalar;
		return Material.GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("KeyLightDirectionWS")), Vector)
			|| Material.GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("KeyLightColor")), Vector)
			|| Material.GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("SceneLightStrength")), Scalar)
			|| Material.GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("KeyLightVisibility")), Scalar);
	}

	bool FreezeWorldMargin(UMaterialInstanceDynamic& Material, float& OutMargin, FString& OutError)
	{
		OutMargin = 0;
		float Cap;
		if (!Material.GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("UE_OutlineMaxOffsetCm")), Cap)) { return true; }
		float Outline, Width, Unit;
		if (!FMath::IsFinite(Cap) || Cap < 0
			|| !Material.GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("_Outline")), Outline)
			|| !Material.GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("_OutlineWidth")), Width)
			|| !Material.GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("UE_OutlineWidthCm")), Unit)
			|| !FMath::IsFinite(Outline) || !FMath::IsFinite(Width) || !FMath::IsFinite(Unit) || Unit < 0)
		{ OutError = TEXT("场景材质的世界轮廓界限接口无效"); return false; }
		OutMargin = FMath::Min(FMath::Max(Outline, 0.f) * FMath::Max(Width, 0.f) * Unit, Cap);
		if (!FMath::IsFinite(OutMargin)) { OutError = TEXT("世界轮廓界限溢出"); return false; }
		// 私有MID固定本租用的几何合同；灯宿主只改灯输入，不让父MI热改悄悄扩大此bounds。
		Material.SetScalarParameterValue(TEXT("UE_OutlineMaxOffsetCm"), Cap);
		Material.SetScalarParameterValue(TEXT("_Outline"), Outline);
		Material.SetScalarParameterValue(TEXT("_OutlineWidth"), Width);
		Material.SetScalarParameterValue(TEXT("UE_OutlineWidthCm"), Unit);
		return true;
	}
}

EGGYGOFXSceneMaterialPreparation GGYGOFXNiagaraSceneMaterials::Prepare(UNiagaraComponent& Component,
	TSharedPtr<FGGYGOFXSceneMaterialState>& OutState, FString& OutError)
{
	OutState.Reset();
	OutError.Reset();
	UNiagaraSystem* const System = Component.GetAsset();
	if (!IsValid(System)) { OutError = TEXT("场景材质准备缺原NS"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
	auto State = MakeShared<FGGYGOFXSceneMaterialState>();
	State->System = System;
	State->World = Component.GetWorld();
	TMap<FName, int32> Parameters;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Handle.GetIsEnabled() || !Data) { continue; }
		float Margin = 0;
		bool bEmitterUsesScene = false;
		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (!IsValid(Renderer) || !Renderer->GetIsEnabled()) { continue; }
			TArray<UMaterialInterface*> Used;
			Renderer->GetUsedMaterials(nullptr, Used);
			const bool bNeedsScene = Used.ContainsByPredicate([](UMaterialInterface* Material)
			{ return IsValid(Material) && NeedsSceneInputs(*Material); });
			if (!bNeedsScene) { continue; }
			const auto* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderer);
			if (!Mesh || Mesh->GetClass() != UNiagaraMeshRendererProperties::StaticClass() || !Mesh->bOverrideMaterials
				|| Mesh->SourceMode != ENiagaraRendererSourceDataMode::Emitter || !Data->bLocalSpace
				|| Data->CalculateBoundsMode != ENiagaraEmitterCalculateBoundMode::Fixed || !Data->FixedBounds.IsValid
				|| Data->FixedBounds.Min.ContainsNaN() || Data->FixedBounds.Max.ContainsNaN()
				|| Data->FixedBounds.Min.X > Data->FixedBounds.Max.X || Data->FixedBounds.Min.Y > Data->FixedBounds.Max.Y
				|| Data->FixedBounds.Min.Z > Data->FixedBounds.Max.Z)
			{ OutError = TEXT("场景FX材质需要原生Emitter Mesh、明确UserMaterial与源固定界限"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
			bEmitterUsesScene = true;
			bool bBoundSceneSlot = false;
			for (const FNiagaraMeshMaterialOverride& Slot : Mesh->OverrideMaterials)
			{
				if (!IsValid(Slot.ExplicitMat) || !NeedsSceneInputs(*Slot.ExplicitMat)) { continue; }
				const FNiagaraVariable& Variable = Slot.UserParamBinding.Parameter;
				if (!Variable.IsValid() || Variable.GetType() != FNiagaraTypeDefinition::GetUMaterialDef()
					|| !FNameBuilder(Variable.GetName()).ToView().StartsWith(TEXT("User.")))
				{ OutError = TEXT("场景FX的UserMaterial参数未显式声明"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
				const FNiagaraParameterStore& Overrides = Component.GetOverrideParameters();
				const bool bHasOverride = Overrides.IndexOf(Variable) != INDEX_NONE;
				UMaterialInterface* Original = Cast<UMaterialInterface>(bHasOverride ? Overrides.GetUObject(Variable).Get()
					: System->GetExposedParameters().GetUObject(Variable).Get());
				if (!IsValid(Original) || !NeedsSceneInputs(*Original))
				{ OutError = TEXT("场景FX的实际UserMaterial为空或接口不符"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
				bBoundSceneSlot = true;
				if (const int32* Index = Parameters.Find(Variable.GetName()))
				{
					if (State->Materials[*Index].OriginalMaterial.Get() != Original)
					{ OutError = TEXT("相同UserMaterial参数映射到不同源材质"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
					float ExistingMargin;
					if (!FreezeWorldMargin(*State->Materials[*Index].InstalledMaterial.Get(), ExistingMargin, OutError)) return EGGYGOFXSceneMaterialPreparation::Invalid;
					Margin = FMath::Max(Margin, ExistingMargin);
					continue;
				}
				FGGYGOFXSceneMaterialBinding Binding;
				Binding.Parameter = Variable;
				Binding.OriginalMaterial.Reset(Original);
				Binding.InstalledMaterial.Reset(UMaterialInstanceDynamic::Create(Original, &Component));
				float Bound;
				if (!Binding.InstalledMaterial.IsValid() || !FreezeWorldMargin(*Binding.InstalledMaterial.Get(), Bound, OutError))
				{ if (OutError.IsEmpty()) OutError = TEXT("场景FX私有MID创建失败"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
				Margin = FMath::Max(Margin, Bound);
				Parameters.Add(Variable.GetName(), State->Materials.Num());
				State->Materials.Add(MoveTemp(Binding));
			}
			if (!bBoundSceneSlot)
			{ OutError = TEXT("场景FX实际借用Mesh材质，缺明确的Scene UserMaterial槽"); return EGGYGOFXSceneMaterialPreparation::Invalid; }
		}
		if (bEmitterUsesScene)
		{
			FGGYGOFXSceneBoundsBinding Bounds;
			Bounds.EmitterName = Handle.GetName();
			Bounds.SourceBounds = Data->FixedBounds;
			Bounds.WorldMarginCm = Margin;
			State->Bounds.Add(MoveTemp(Bounds));
		}
	}
	if (State->Materials.IsEmpty()) { return EGGYGOFXSceneMaterialPreparation::NotRequired; }
	OutState = MoveTemp(State);
	return EGGYGOFXSceneMaterialPreparation::Prepared;
}

bool GGYGOFXNiagaraSceneMaterials::Install(UNiagaraComponent& Component, FGGYGOFXSceneMaterialState& State,
	TFunctionRef<bool()> IsOriginal, FString& OutError)
{
	for (const auto& Binding : State.Materials)
	{
		const FNiagaraParameterStore& Overrides = Component.GetOverrideParameters();
		const UObject* Current = Overrides.IndexOf(Binding.Parameter) != INDEX_NONE
			? Overrides.GetUObject(Binding.Parameter).Get()
			: State.System->GetExposedParameters().GetUObject(Binding.Parameter).Get();
		if (Current != Binding.OriginalMaterial.Get())
		{ OutError = TEXT("等待场景灯期间实际UserMaterial已被其它写入者改变"); return false; }
		Component.SetVariableMaterial(Binding.Parameter.GetName(), Binding.InstalledMaterial.Get());
		if (!IsOriginal()) { OutError = TEXT("安装UserMaterial期间原NS租用已改变"); return false; }
		if (Component.GetOverrideParameters().GetUObject(Binding.Parameter).Get() != Binding.InstalledMaterial.Get())
		{ OutError = TEXT("原生UserMaterial未读回本次MID"); return false; }
	}
	State.bInstalled = true;
	return true;
}

bool GGYGOFXNiagaraSceneMaterials::UpdateBounds(UNiagaraComponent& Component, FGGYGOFXSceneMaterialState& State,
	TFunctionRef<bool()> IsOriginal, FString& OutError)
{
	if (State.bUpdatingBounds) { return true; }
	State.bUpdatingBounds = true;
	ON_SCOPE_EXIT { State.bUpdatingBounds = false; };
	const double MinimumScale = Component.GetComponentScale().GetAbs().GetMin();
	for (auto& Bounds : State.Bounds)
	{
		if (Bounds.WorldMarginCm <= 0) { continue; }
		if (!FMath::IsFinite(MinimumScale) || MinimumScale <= 0)
		{ OutError = FString::Printf(TEXT("Emitter='%s'世界轮廓界限确需逆Root，原Root最小scale为零或无效"), *Bounds.EmitterName.ToString()); return false; }
		const double Margin = Bounds.WorldMarginCm / MinimumScale;
		if (!FMath::IsFinite(Margin) || Margin > MAX_flt)
		{ OutError = FString::Printf(TEXT("Emitter='%s'世界轮廓转换到Root局部界限溢出"), *Bounds.EmitterName.ToString()); return false; }
		const FBox Current = Component.GetEmitterFixedBounds(Bounds.EmitterName);
		if (!Bounds.bInstalled) { Bounds.OriginalBounds = Current; }
		else if (Current != Bounds.InstalledBounds)
		{ OutError = TEXT("原Emitter界限已被其它写入者改变"); return false; }
		const FBox Next = Bounds.SourceBounds.ExpandBy(Margin);
		if (Next.Min.ContainsNaN() || Next.Max.ContainsNaN())
		{ OutError = TEXT("扩展后的Emitter世界轮廓界限无效"); return false; }
		Component.SetEmitterFixedBounds(Bounds.EmitterName, Next);
		if (!IsOriginal()) { OutError = TEXT("更新Emitter界限时原NS租用已改变"); return false; }
		if (Component.GetEmitterFixedBounds(Bounds.EmitterName) != Next)
		{ OutError = TEXT("原生Emitter未读回完整世界轮廓界限"); return false; }
		Bounds.InstalledBounds = Next;
		Bounds.bInstalled = true;
	}
	return true;
}

void GGYGOFXNiagaraSceneMaterials::Restore(UNiagaraComponent& Component, FGGYGOFXSceneMaterialState& State,
	TFunctionRef<bool()> IsOriginal)
{
	for (const auto& Binding : State.Materials)
	{
		if (!IsOriginal()) { return; }
		if (Component.GetOverrideParameters().GetUObject(Binding.Parameter).Get() == Binding.InstalledMaterial.Get())
			Component.SetVariableMaterial(Binding.Parameter.GetName(), Binding.OriginalMaterial.Get());
	}
	for (const auto& Bounds : State.Bounds)
	{
		if (!IsOriginal()) { return; }
		if (Bounds.bInstalled && Component.GetEmitterFixedBounds(Bounds.EmitterName) == Bounds.InstalledBounds)
			Component.SetEmitterFixedBounds(Bounds.EmitterName, Bounds.OriginalBounds);
	}
}
