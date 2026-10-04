/**
 * @file GGYGOGameplayCueNotify_HitImpact.cpp
 * @brief 命中特效分流实现
 */
#include "AbilitySystem/Cues/GGYGOGameplayCueNotify_HitImpact.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Components/SceneComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayCueNotify_HitImpact)

namespace
{
enum class EInheritedAttachmentLocation : uint8
{
	Unattached,
	RelativeOffset,
	SoundSnapToSocket
};

bool IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

const TCHAR* FailureName(EGGYGOHitImpactResolveFailure Failure)
{
	switch (Failure)
	{
	case EGGYGOHitImpactResolveFailure::Unresolved: return TEXT("Unresolved");
	case EGGYGOHitImpactResolveFailure::None: return TEXT("None");
	case EGGYGOHitImpactResolveFailure::InvalidTarget: return TEXT("InvalidTarget");
	case EGGYGOHitImpactResolveFailure::InvalidWorld: return TEXT("InvalidWorld");
	case EGGYGOHitImpactResolveFailure::InvalidLocationMode: return TEXT("InvalidLocationMode");
	case EGGYGOHitImpactResolveFailure::MissingHitResult: return TEXT("MissingHitResult");
	case EGGYGOHitImpactResolveFailure::NonFiniteLocation: return TEXT("NonFiniteLocation");
	case EGGYGOHitImpactResolveFailure::NonFiniteNormal: return TEXT("NonFiniteNormal");
	case EGGYGOHitImpactResolveFailure::InvalidSurfaceConfiguration: return TEXT("InvalidSurfaceConfiguration");
	case EGGYGOHitImpactResolveFailure::MissingSurfaceTag: return TEXT("MissingSurfaceTag");
	case EGGYGOHitImpactResolveFailure::UnmatchedSurfaceTag: return TEXT("UnmatchedSurfaceTag");
	case EGGYGOHitImpactResolveFailure::InvalidSelectedResource: return TEXT("InvalidSelectedResource");
	case EGGYGOHitImpactResolveFailure::InheritedPlacementConflict: return TEXT("InheritedPlacementConflict");
	case EGGYGOHitImpactResolveFailure::InheritedConfigurationUnavailable: return TEXT("InheritedConfigurationUnavailable");
	case EGGYGOHitImpactResolveFailure::TargetExpiredDuringFeedback: return TEXT("TargetExpiredDuringFeedback");
	default: return TEXT("Unknown");
	}
}

// BurstEffects 没有公开的配置遍历接口；只读取引擎已反射的固定配置，不修改父类数据。
template <typename EffectType>
const EffectType* ReadInheritedEffect(const FGameplayCueNotify_BurstEffects& Effects, FName FieldName)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(
		FGameplayCueNotify_BurstEffects::StaticStruct(), FieldName);
	return Property && Property->Struct == EffectType::StaticStruct()
		? Property->ContainerPtrToValuePtr<EffectType>(&Effects) : nullptr;
}

template <typename EffectType, typename CheckType>
bool CheckInheritedEffectArray(const FGameplayCueNotify_BurstEffects& Effects, FName FieldName,
	CheckType Check, FGGYGOHitImpactResolveResult& OutResult)
{
	const FArrayProperty* Property = FindFProperty<FArrayProperty>(
		FGameplayCueNotify_BurstEffects::StaticStruct(), FieldName);
	const FStructProperty* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
	if (!Inner || Inner->Struct != EffectType::StaticStruct())
	{
		OutResult.Failure = EGGYGOHitImpactResolveFailure::InheritedConfigurationUnavailable;
		OutResult.Detail = FString::Printf(TEXT("父类反射配置 %s 不存在或类型改变。"), *FieldName.ToString());
		return false;
	}
	FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(&Effects));
	for (int32 Index = 0; Index < Array.Num(); ++Index)
	{
		if (!Check(*reinterpret_cast<const EffectType*>(Array.GetRawPtr(Index)),
			FString::Printf(TEXT("%s[%d]"), *FieldName.ToString(), Index)))
		{
			return false;
		}
	}
	return true;
}
}

UGGYGOGameplayCueNotify_HitImpact::UGGYGOGameplayCueNotify_HitImpact()
{
}

FGGYGOHitImpactResolveResult UGGYGOGameplayCueNotify_HitImpact::ResolveImpactPresentation(
	AActor* Target, const FGameplayCueParameters& Parameters) const
{
	FGGYGOHitImpactResolveResult Result;
	auto Reject = [&Result](EGGYGOHitImpactResolveFailure Failure, const TCHAR* Detail)
	{
		Result.Failure = Failure;
		Result.Effect = nullptr;
		Result.Detail = Detail;
		return Result;
	};
	if (!IsValid(Target)) { return Reject(EGGYGOHitImpactResolveFailure::InvalidTarget, TEXT("Cue 目标缺失或已失效。")); }
	if (!IsValid(Target->GetWorld())) { return Reject(EGGYGOHitImpactResolveFailure::InvalidWorld, TEXT("Cue 目标没有有效 World。")); }

	switch (LocationMode)
	{
	case EGGYGOHitImpactLocationMode::HitResult:
	{
		const FHitResult* Hit = Parameters.EffectContext.GetHitResult();
		if (!Hit) { return Reject(EGGYGOHitImpactResolveFailure::MissingHitResult, TEXT("HitResult 模式需要原始命中载荷。")); }
		Result.Location = Hit->ImpactPoint;
		Result.Normal = Hit->ImpactNormal;
		break;
	}
	case EGGYGOHitImpactLocationMode::ParametersLocation:
		Result.Location = FVector(Parameters.Location);
		Result.Normal = FVector(Parameters.Normal);
		break;
	case EGGYGOHitImpactLocationMode::TargetCenter:
		Result.Location = Target->GetActorLocation();
		Result.Normal = FVector(Parameters.Normal);
		break;
	default:
		return Reject(EGGYGOHitImpactResolveFailure::InvalidLocationMode, TEXT("位置模式枚举无效。"));
	}
	if (!IsFiniteVector(Result.Location)) { return Reject(EGGYGOHitImpactResolveFailure::NonFiniteLocation, TEXT("所选位置含 NaN 或 Infinity。")); }
	if (!IsFiniteVector(Result.Normal)) { return Reject(EGGYGOHitImpactResolveFailure::NonFiniteNormal, TEXT("所选法线含 NaN 或 Infinity。")); }

	const FGameplayTagContainer& TargetTags = Parameters.AggregatedTargetTags;
	const FGameplayTag SurfaceRoot = FGameplayTag::RequestGameplayTag(FName(TEXT("SurfaceType")), false);
	if (!SurfaceRoot.IsValid())
	{
		return Reject(EGGYGOHitImpactResolveFailure::InvalidSurfaceConfiguration, TEXT("SurfaceType Tag 根未注册。"));
	}
	const bool bHasSurface = TargetTags.HasTag(SurfaceRoot);
	if (!bHasSurface)
	{
		switch (NoSurfacePolicy)
		{
		case EGGYGOHitImpactNoSurfacePolicy::Generic: Result.Effect = &DefaultEffect; break;
		case EGGYGOHitImpactNoSurfacePolicy::Reject:
			return Reject(EGGYGOHitImpactResolveFailure::MissingSurfaceTag, TEXT("当前资产明确拒绝无 SurfaceType Tag 的载荷。"));
		default:
			return Reject(EGGYGOHitImpactResolveFailure::InvalidSurfaceConfiguration, TEXT("无表面策略枚举无效。"));
		}
	}
	else
	{
		for (const FGGYGOHitImpactEffect& Effect : SurfaceEffects)
		{
			if (!Effect.SurfaceTag.IsValid() || !Effect.SurfaceTag.MatchesTag(SurfaceRoot))
			{
				return Reject(EGGYGOHitImpactResolveFailure::InvalidSurfaceConfiguration, TEXT("SurfaceEffects 含无效或非 SurfaceType 映射项。"));
			}
			if (TargetTags.HasTag(Effect.SurfaceTag)) { Result.Effect = &Effect; break; }
		}
		if (!Result.Effect)
		{
			if (!bAllowUnmatchedSurfaceGeneric)
			{
				return Reject(EGGYGOHitImpactResolveFailure::UnmatchedSurfaceTag, TEXT("载荷有表面 Tag，但资产没有匹配且未允许通用未匹配反馈。"));
			}
			Result.Effect = &DefaultEffect;
		}
	}
	if ((Result.Effect->ImpactEffect && !IsValid(Result.Effect->ImpactEffect.Get()))
		|| (Result.Effect->ImpactSound && !IsValid(Result.Effect->ImpactSound.Get())))
	{
		return Reject(EGGYGOHitImpactResolveFailure::InvalidSelectedResource, TEXT("选中反馈的非空资产引用已失效。"));
	}

	FGameplayCueParameters SelectedParameters = Parameters;
	SelectedParameters.Location = Result.Location;
	SelectedParameters.Normal = Result.Normal;
	if (!ValidateInheritedPlacement(Target, SelectedParameters, Result.Location, Result))
	{
		Result.Effect = nullptr;
		return Result;
	}
	Result.Failure = EGGYGOHitImpactResolveFailure::None;
	return Result;
}

bool UGGYGOGameplayCueNotify_HitImpact::ValidateInheritedPlacement(AActor* Target,
	const FGameplayCueParameters& Parameters, const FVector& SelectedLocation,
	FGGYGOHitImpactResolveResult& OutResult) const
{
	FGameplayCueNotify_SpawnContext Context(Target->GetWorld(), Target, Parameters);
	Context.SetDefaultPlacementInfo(&DefaultPlacementInfo);
	auto CheckPlacement = [&Context, &SelectedLocation, &OutResult](
		const FGameplayCueNotify_PlacementInfo& Placement, EInheritedAttachmentLocation AttachmentLocation,
		const FString& Field)
	{
		auto Conflict = [&OutResult, &Field](const FString& Detail)
		{
			OutResult.Failure = EGGYGOHitImpactResolveFailure::InheritedPlacementConflict;
			OutResult.Detail = Field + TEXT(": ") + Detail;
			return false;
		};
		// 在调用原生变换构造前拒绝非有限输入，防止引擎 NaN 诊断将其重置后被误判为兼容。
		if ((Placement.bOverrideRotation && Placement.RotationOverride.ContainsNaN())
			|| (Placement.bOverrideScale && !IsFiniteVector(Placement.ScaleOverride)))
		{
			return Conflict(TEXT("原生 RotationOverride / ScaleOverride 含非有限值。"));
		}
		if (Context.HitResult && Context.HitResult->bBlockingHit
			&& (!IsFiniteVector(Context.HitResult->ImpactPoint) || !IsFiniteVector(Context.HitResult->ImpactNormal)))
		{
			return Conflict(TEXT("原生定位会消费的 BlockingHit 位置/法线含非有限值。"));
		}
		const bool bUsesTargetSocket = !(Context.HitResult && Context.HitResult->bBlockingHit)
			&& Context.CueParameters.Location.IsZero();
		const bool bAttaches = AttachmentLocation != EInheritedAttachmentLocation::Unattached && Context.TargetComponent
			&& Placement.AttachPolicy == EGameplayCueNotify_AttachPolicy::AttachToTarget;
		if ((bUsesTargetSocket || bAttaches) && (!IsValid(Context.TargetComponent)
			|| (!Placement.SocketName.IsNone() && !Context.TargetComponent->DoesSocketExist(Placement.SocketName))))
		{
			return Conflict(TEXT("原生定位需要有效目标组件及配置 Socket。"));
		}
		FTransform NativeTransform;
		if (!Placement.FindSpawnTransform(Context, NativeTransform)
			|| NativeTransform.ContainsNaN())
		{
			return Conflict(TEXT("原生定位不能产生有限变换。"));
		}
		FVector NativeLocation = NativeTransform.GetLocation();
		if (bAttaches)
		{
			const FTransform SocketTransform = Context.TargetComponent->GetSocketTransform(Placement.SocketName);
			switch (Placement.AttachmentRule)
			{
			case EAttachmentRule::KeepWorld: break;
			case EAttachmentRule::KeepRelative: NativeLocation = SocketTransform.TransformPosition(NativeLocation); break;
			case EAttachmentRule::SnapToTarget:
				// 本机原生 Sound Snap 丢弃 Location；Niagara / Decal / ForceFeedback 仍将它当相对偏移。
				NativeLocation = AttachmentLocation == EInheritedAttachmentLocation::SoundSnapToSocket
					? SocketTransform.GetLocation() : SocketTransform.TransformPosition(NativeLocation);
				break;
			default: return Conflict(TEXT("原生 AttachmentRule 无效。"));
			}
		}
		if (!IsFiniteVector(NativeLocation) || !NativeLocation.Equals(SelectedLocation, KINDA_SMALL_NUMBER))
		{
			return Conflict(FString::Printf(TEXT("原生落点 %s 与显式选定落点 %s 冲突；有效原点/Overlap 本身未被判非法。"),
				*NativeLocation.ToString(), *SelectedLocation.ToString()));
		}
		return true;
	};

	// 配置了资源就检查其布局，不调用 ShouldSpawn：预检不得消费随机概率或修改条件缓存。
	if (!CheckInheritedEffectArray<FGameplayCueNotify_ParticleInfo>(BurstEffects, FName(TEXT("BurstParticles")),
		[&](const FGameplayCueNotify_ParticleInfo& Effect, const FString& Field)
		{
			return !Effect.NiagaraSystem || CheckPlacement(
				Context.GetPlacementInfo(Effect.bOverridePlacementInfo, Effect.PlacementInfoOverride),
				EInheritedAttachmentLocation::RelativeOffset, Field);
		}, OutResult)
		|| !CheckInheritedEffectArray<FGameplayCueNotify_SoundInfo>(BurstEffects, FName(TEXT("BurstSounds")),
		[&](const FGameplayCueNotify_SoundInfo& Effect, const FString& Field)
		{
			return !Effect.Sound || CheckPlacement(
				Context.GetPlacementInfo(Effect.bOverridePlacementInfo, Effect.PlacementInfoOverride),
				EInheritedAttachmentLocation::SoundSnapToSocket, Field);
		}, OutResult)) { return false; }

	const auto* Shake = ReadInheritedEffect<FGameplayCueNotify_CameraShakeInfo>(BurstEffects, FName(TEXT("BurstCameraShake")));
	const auto* Lens = ReadInheritedEffect<FGameplayCueNotify_CameraLensEffectInfo>(BurstEffects, FName(TEXT("BurstCameraLensEffect")));
	const auto* Feedback = ReadInheritedEffect<FGameplayCueNotify_ForceFeedbackInfo>(BurstEffects, FName(TEXT("BurstForceFeedback")));
	const auto* Decal = ReadInheritedEffect<FGameplayCueNotify_DecalInfo>(BurstEffects, FName(TEXT("BurstDecal")));
	if (!Shake || !Lens || !Feedback || !Decal)
	{
		OutResult.Failure = EGGYGOHitImpactResolveFailure::InheritedConfigurationUnavailable;
		OutResult.Detail = TEXT("Burst 空间配置的反射字段不存在或类型改变。");
		return false;
	}
	if (Shake->CameraShake && Shake->bPlayInWorld && !(Shake->ShakeScale <= 0.0f)
		&& !CheckPlacement(Context.GetPlacementInfo(Shake->bOverridePlacementInfo, Shake->PlacementInfoOverride),
			EInheritedAttachmentLocation::Unattached, TEXT("BurstCameraShake"))) { return false; }
	if (Lens->CameraLensEffect && Lens->bPlayInWorld
		&& !CheckPlacement(Context.GetPlacementInfo(Lens->bOverridePlacementInfo, Lens->PlacementInfoOverride),
			EInheritedAttachmentLocation::Unattached, TEXT("BurstCameraLensEffect"))) { return false; }
	if (Feedback->ForceFeedbackEffect && Feedback->bPlayInWorld
		&& !CheckPlacement(Context.GetPlacementInfo(Feedback->bOverridePlacementInfo, Feedback->PlacementInfoOverride),
			EInheritedAttachmentLocation::RelativeOffset, TEXT("BurstForceFeedback"))) { return false; }
	if (Decal->DecalMaterial
		&& !CheckPlacement(Context.GetPlacementInfo(Decal->bOverridePlacementInfo, Decal->PlacementInfoOverride),
			EInheritedAttachmentLocation::RelativeOffset, TEXT("BurstDecal"))) { return false; }
	return true;
}

void UGGYGOGameplayCueNotify_HitImpact::ReportFailure(AActor* Target, const FGameplayCueParameters& Parameters,
	const FGGYGOHitImpactResolveResult& Result) const
{
	static_assert(static_cast<uint8>(EGGYGOHitImpactResolveFailure::TargetExpiredDuringFeedback) < 32);
	const uint32 ReasonBit = 1u << static_cast<uint8>(Result.Failure);
	if (ReportedFailureReasons & ReasonBit) { return; }
	ReportedFailureReasons |= ReasonBit;
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("Physics HitImpact [%s] 目标[%s] LocationMode[%d] NoSurfacePolicy[%d] Tags[%s] 拒绝[%s]：%s"),
		*GetPathName(), *GetPathNameSafe(Target), static_cast<int32>(LocationMode), static_cast<int32>(NoSurfacePolicy),
		*Parameters.AggregatedTargetTags.ToString(), FailureName(Result.Failure), *Result.Detail);
}

void UGGYGOGameplayCueNotify_HitImpact::HandleGameplayCue(AActor* Target, EGameplayCueEvent::Type EventType,
	const FGameplayCueParameters& Parameters)
{
	if (EventType != EGameplayCueEvent::Executed)
	{
		Super::HandleGameplayCue(Target, EventType, Parameters);
		return;
	}
	FGameplayCueParameters SelectedParameters = Parameters;
	{
		const FGGYGOHitImpactResolveResult Result = ResolveImpactPresentation(Target, Parameters);
		if (!Result.IsSuccess()) { ReportFailure(Target, Parameters, Result); return; }
		SelectedParameters.Location = Result.Location;
		SelectedParameters.Normal = Result.Normal;
	} // 借用的 Effect 指针不跨 K2_HandleGameplayCue 外调。
	Super::HandleGameplayCue(Target, EventType, SelectedParameters);
}

bool UGGYGOGameplayCueNotify_HitImpact::OnExecute_Implementation(AActor* Target,
	const FGameplayCueParameters& Parameters) const
{
	FGameplayCueParameters SelectedParameters = Parameters;
	TStrongObjectPtr<UNiagaraSystem> ImpactEffect;
	TStrongObjectPtr<USoundBase> ImpactSound;
	FVector ImpactLocation;
	FRotator ImpactRotation;
	{
		const FGGYGOHitImpactResolveResult Result = ResolveImpactPresentation(Target, Parameters);
		if (!Result.IsSuccess()) { ReportFailure(Target, Parameters, Result); return false; }
		ImpactLocation = Result.Location;
		ImpactRotation = Result.Normal.Rotation();
		SelectedParameters.Location = Result.Location;
		SelectedParameters.Normal = Result.Normal;
		ImpactEffect.Reset(Result.Effect->ImpactEffect.Get());
		ImpactSound.Reset(Result.Effect->ImpactSound.Get());
	} // 资源强引用和数值快照已取得；Super / OnBurst 重入不能使旧数组引用悬空。
	const TWeakObjectPtr<AActor> TargetSnapshot(Target);
	const TWeakObjectPtr<UWorld> WorldSnapshot(Target->GetWorld());

	// Burst 原生实现播放后也固定返回 false；它不是本项目输入验证或反馈成功的结果。
	Super::OnExecute_Implementation(Target, SelectedParameters);
	auto HasLiveFeedbackInputs = [&]()
	{
		FGGYGOHitImpactResolveResult Failure;
		if (!TargetSnapshot.IsValid() || !WorldSnapshot.IsValid())
		{
			Failure.Failure = EGGYGOHitImpactResolveFailure::TargetExpiredDuringFeedback;
			Failure.Detail = TEXT("外部反馈回调后目标/World 已失效，中止剩余自身反馈。");
		}
		else if ((ImpactEffect.Get() && !IsValid(ImpactEffect.Get()))
			|| (ImpactSound.Get() && !IsValid(ImpactSound.Get())))
		{
			Failure.Failure = EGGYGOHitImpactResolveFailure::InvalidSelectedResource;
			Failure.Detail = TEXT("外部反馈回调后选中资产引用已失效，中止剩余自身反馈。");
		}
		else { return true; }
		ReportFailure(TargetSnapshot.Get(), SelectedParameters, Failure);
		return false;
	};
	if (!HasLiveFeedbackInputs()) { return false; }

	if (ImpactEffect.IsValid())
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			WorldSnapshot.Get(),
			ImpactEffect.Get(),
			ImpactLocation,
			ImpactRotation);
	}
	if (!HasLiveFeedbackInputs()) { return false; }
	if (ImpactSound.IsValid())
	{
		UGameplayStatics::PlaySoundAtLocation(WorldSnapshot.Get(), ImpactSound.Get(), ImpactLocation);
	}
	// true 表示载荷通过并完成同步派发；不是粒子/音频实际可见或网络验收。
	return true;
}
