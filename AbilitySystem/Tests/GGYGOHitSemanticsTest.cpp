/** @file GGYGOHitSemanticsTest.cpp @brief EffectContext 命中载荷与 Cue 位置语义 */
#include "AbilitySystem/Tests/GGYGOHitSemanticsTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Cues/GGYGOGameplayCueNotify_HitImpact.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameplayEffect.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Physics/GGYGOPhysicalMaterialWithTags.h"
#include "System/GGYGOGameplayTags.h"
#include "Templates/UniquePtr.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FGGYGOHitSemanticsTestWorld
	{
		UEngine* Engine = GEngine;
		UWorld* World = nullptr;

		FGGYGOHitSemanticsTestWorld()
		{
			if (!Engine)
			{
				return;
			}

			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
			if (World)
			{
				Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			}
		}

		~FGGYGOHitSemanticsTestWorld()
		{
			if (World)
			{
				UPackage* WorldPackage = World->GetPackage();
				World->DestroyWorld(false);
				if (Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				if (WorldPackage)
				{
					WorldPackage->SetDirtyFlag(false);
				}
			}
		}
	};

	UGGYGOAbilitySystemComponent* MakeHitSemanticsTestASC(UWorld* World, AActor*& OutActor)
	{
		OutActor = World ? World->SpawnActor<AActor>() : nullptr;
		if (!OutActor)
		{
			return nullptr;
		}

		UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(OutActor);
		if (ASC)
		{
			ASC->RegisterComponent();
			ASC->InitAbilityActorInfo(OutActor, OutActor);
		}
		return ASC;
	}

	struct FGGYGOFinishHitSemanticsAbility
	{
		UGGYGOHitSemanticsTestAbility* Ability = nullptr;
		~FGGYGOFinishHitSemanticsAbility()
		{
			if (Ability)
			{
				Ability->FinishForTest();
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHitSemanticsTest, "GGYGO.AbilitySystem.HitSemantics.ContextAndCueLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHitSemanticsTest::RunTest(const FString& Parameters)
{
	const FVector ExplicitOrigin(100.0, 25.0, -10.0);
	const FVector ImpactPoint(103.0, 29.0, 2.0);
	FHitResult HitResult;
	HitResult.ImpactPoint = ImpactPoint;

	FGGYGOGameplayEffectContext Context;
	Context.AddHitResult(HitResult, /*bReset=*/true);
	Context.SetSourceOriginSnapshot(ExplicitOrigin);

	TUniquePtr<FGameplayEffectContext> DuplicatedBase(Context.Duplicate());
	const FGGYGOGameplayEffectContext* Duplicated = static_cast<const FGGYGOGameplayEffectContext*>(DuplicatedBase.Get());
	TestNotNull(TEXT("Duplicate retains custom EffectContext"), Duplicated);
	if (!Duplicated)
	{
		return false;
	}
	TestTrue(TEXT("Duplicate retains explicit Origin set after HitResult"), Duplicated->HasOrigin());
	TestTrue(TEXT("Duplicate retains the explicit source Origin marker"), Duplicated->HasSourceOriginSnapshot());
	TestTrue(TEXT("Duplicate retains the explicit Origin value"), Duplicated->GetOrigin().Equals(ExplicitOrigin));
	TestNotNull(TEXT("Duplicate retains HitResult"), Duplicated->GetHitResult());
	if (Duplicated->GetHitResult())
	{
		TestTrue(TEXT("Duplicate retains ImpactPoint"), Duplicated->GetHitResult()->ImpactPoint.Equals(ImpactPoint));
		TestTrue(TEXT("Duplicate owns a deep-copied HitResult"), Duplicated->GetHitResult() != Context.GetHitResult());
	}
	const float ExpectedDistance = static_cast<float>(FVector::Dist(ExplicitOrigin, ImpactPoint));
	TestEqual(TEXT("Distance uses Origin to ImpactPoint"), Duplicated->GetDistanceFromOriginToHitResult(), ExpectedDistance);

	FGGYGOGameplayEffectContext NoHitContext;
	NoHitContext.SetSourceOriginSnapshot(ExplicitOrigin);
	TestEqual(TEXT("Distance is zero without HitResult"), NoHitContext.GetDistanceFromOriginToHitResult(), 0.0f);
	FGGYGOGameplayEffectContext NoOriginContext;
	NoOriginContext.AddHitResult(HitResult, /*bReset=*/true);
	TestTrue(TEXT("AddHitResult supplies an implicit TraceStart Origin"), NoOriginContext.HasOrigin());
	TestFalse(TEXT("Implicit TraceStart is not an explicit source snapshot"), NoOriginContext.HasSourceOriginSnapshot());
	TestEqual(TEXT("Distance is zero without explicit Origin"), NoOriginContext.GetDistanceFromOriginToHitResult(), 0.0f);

	FGGYGOHitSemanticsTestWorld TestWorld;
	if (!TestNotNull(TEXT("Cue 独立测试世界"), TestWorld.World)) { return false; }
	AActor* Target = TestWorld.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Cue 实际目标"), Target)) { return false; }
	USceneComponent* TargetRoot = NewObject<USceneComponent>(Target, NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("Cue 目标 SceneRoot"), TargetRoot)) { return false; }
	TargetRoot->SetMobility(EComponentMobility::Movable);
	Target->AddInstanceComponent(TargetRoot);
	if (!TestTrue(TEXT("Cue 目标设置 Root 成功"), Target->SetRootComponent(TargetRoot))) { return false; }
	TargetRoot->RegisterComponent();
	const FVector TargetLocation(500.0, 600.0, 700.0);
	if (!TestTrue(TEXT("Cue 目标实际移动成功"), Target->SetActorLocation(TargetLocation))) { return false; }
	if (!TestTrue(TEXT("Cue 目标实际坐标与 World 有效"),
		TargetRoot->IsRegistered() && Target->GetWorld() == TestWorld.World
		&& Target->GetActorLocation().Equals(TargetLocation)
		&& TargetRoot->GetComponentLocation().Equals(TargetLocation))) { return false; }

	TStrongObjectPtr<UGGYGOGameplayCueNotify_HitImpact> Cue(
		NewObject<UGGYGOGameplayCueNotify_HitImpact>(GetTransientPackage(), NAME_None, RF_Transient));
	if (!TestNotNull(TEXT("独立 HitImpact Cue"), Cue.Get())) { return false; }
	const FEnumProperty* LocationModeProperty = FindFProperty<FEnumProperty>(
		Cue->GetClass(), FName(TEXT("LocationMode")));
	if (!TestNotNull(TEXT("Cue 位置模式反射字段"), LocationModeProperty)) { return false; }
	const FNumericProperty* LocationModeValue = LocationModeProperty->GetUnderlyingProperty();
	if (!TestNotNull(TEXT("Cue 位置模式底层枚举字段"), LocationModeValue)) { return false; }
	auto SetLocationMode = [&](EGGYGOHitImpactLocationMode Mode)
	{
		LocationModeValue->SetIntPropertyValue(LocationModeProperty->ContainerPtrToValuePtr<void>(Cue.Get()),
			static_cast<uint64>(Mode));
	};

	FGameplayCueParameters CueParameters;
	const FVector ParameterLocation(20.0, 30.0, 40.0);
	CueParameters.Location = ParameterLocation;
	{
		const FGGYGOHitImpactResolveResult MissingHit = Cue->ResolveImpactPresentation(Target, CueParameters);
		TestTrue(TEXT("Cue 默认无 HitResult 明确拒绝"),
			MissingHit.Failure == EGGYGOHitImpactResolveFailure::MissingHitResult);
		TestFalse(TEXT("Cue 默认无 HitResult 不会表现为成功"), MissingHit.IsSuccess());
	}

	SetLocationMode(EGGYGOHitImpactLocationMode::TargetCenter);
	{
		const FGGYGOHitImpactResolveResult TargetResult = Cue->ResolveImpactPresentation(Target, CueParameters);
		TestTrue(TEXT("Cue 显式 TargetCenter 与无表面 Generic 正常模式通过"), TargetResult.IsSuccess());
		TestTrue(TEXT("Cue 显式 TargetCenter 使用实际目标位置"),
			TargetResult.Location.Equals(Target->GetActorLocation()));
	}

	SetLocationMode(EGGYGOHitImpactLocationMode::ParametersLocation);
	{
		const FGGYGOHitImpactResolveResult ParameterResult = Cue->ResolveImpactPresentation(Target, CueParameters);
		TestTrue(TEXT("Cue 显式 ParametersLocation 无 HitResult 通过"), ParameterResult.IsSuccess());
		TestTrue(TEXT("Cue 显式 ParametersLocation 保留传入位置"), ParameterResult.Location.Equals(ParameterLocation));
	}
	CueParameters.Location = FVector::ZeroVector;
	{
		const FGGYGOHitImpactResolveResult ParameterOrigin = Cue->ResolveImpactPresentation(Target, CueParameters);
		TestTrue(TEXT("Cue 显式 ParametersLocation 世界原点通过"), ParameterOrigin.IsSuccess());
		TestTrue(TEXT("Cue 显式 ParametersLocation 严格保留世界原点"), ParameterOrigin.Location.IsZero());
	}

	SetLocationMode(EGGYGOHitImpactLocationMode::HitResult);
	FHitResult OriginHit;
	OriginHit.ImpactPoint = FVector::ZeroVector;
	OriginHit.ImpactNormal = FVector::UpVector;
	OriginHit.bBlockingHit = false; // 合成 Overlap 载荷；不以 BlockingHit 作为 Cue 的必需条件。
	FGGYGOGameplayEffectContext* CueContext = new FGGYGOGameplayEffectContext();
	CueContext->AddHitResult(OriginHit, /*bReset=*/true);
	CueParameters.EffectContext = FGameplayEffectContextHandle(CueContext);
	CueParameters.Location = ParameterLocation;
	const FHitResult* OriginalCueHit = CueParameters.EffectContext.GetHitResult();
	{
		const FGGYGOHitImpactResolveResult HitOrigin = Cue->ResolveImpactPresentation(Target, CueParameters);
		TestTrue(TEXT("Cue 合成世界原点 Overlap 通过"), HitOrigin.IsSuccess());
		TestTrue(TEXT("Cue keeps a valid hit at world origin"),
			HitOrigin.Location.IsZero());
		TestTrue(TEXT("Cue 保留原 Context 的 Overlap 命中载荷"),
			OriginalCueHit && CueParameters.EffectContext.GetHitResult() == OriginalCueHit
			&& OriginalCueHit->ImpactPoint.IsZero() && !OriginalCueHit->bBlockingHit);
	}

	const FGameplayTag SurfaceTag = GGYGOGameplayTags::SurfaceType_Metal;
	if (!TestTrue(TEXT("已知 Metal Surface Tag 已注册"), SurfaceTag.IsValid())) { return false; }
	CueParameters.AggregatedTargetTags.AddTag(SurfaceTag);
	{
		const FGGYGOHitImpactResolveResult UnmatchedSurface = Cue->ResolveImpactPresentation(Target, CueParameters);
		TestTrue(TEXT("Cue 已知表面漏配默认明确拒绝"),
			UnmatchedSurface.Failure == EGGYGOHitImpactResolveFailure::UnmatchedSurfaceTag);
		TestFalse(TEXT("Cue 已知表面漏配不会隐式使用 Generic 成功"), UnmatchedSurface.IsSuccess());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHitPayloadTagPreservationTest,
	"GGYGO.AbilitySystem.HitSemantics.PayloadTargetAndSurfaceTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHitPayloadTagPreservationTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine))
	{
		return false;
	}

	FGGYGOHitSemanticsTestWorld TestWorld;
	if (!TestNotNull(TEXT("独立测试世界"), TestWorld.World))
	{
		return false;
	}

	AActor* SourceActor = nullptr;
	UGGYGOAbilitySystemComponent* SourceASC = MakeHitSemanticsTestASC(TestWorld.World, SourceActor);
	AActor* TargetActor = nullptr;
	UGGYGOAbilitySystemComponent* TargetASC = MakeHitSemanticsTestASC(TestWorld.World, TargetActor);
	if (!TestNotNull(TEXT("攻击方 ASC"), SourceASC)
		|| !TestNotNull(TEXT("目标 ASC"), TargetASC))
	{
		return false;
	}

	const FGameplayAbilitySpecHandle AbilityHandle = SourceASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOHitSemanticsTestAbility::StaticClass(), 1));
	if (!TestTrue(TEXT("专用命中测试能力成功激活"), SourceASC->TryActivateAbility(AbilityHandle)))
	{
		return false;
	}

	FGameplayAbilitySpec* AbilitySpec = SourceASC->FindAbilitySpecFromHandle(AbilityHandle);
	UGGYGOHitSemanticsTestAbility* Ability = AbilitySpec
		? Cast<UGGYGOHitSemanticsTestAbility>(AbilitySpec->GetPrimaryInstance())
		: nullptr;
	if (!TestNotNull(TEXT("专用测试能力实例"), Ability))
	{
		return false;
	}
	FGGYGOFinishHitSemanticsAbility FinishAbility{Ability};

	const FGameplayTag TargetOwnedTag = GGYGOGameplayTags::Gameplay_Damage_Immunity;
	const FGameplayTag SurfaceTag = GGYGOGameplayTags::SurfaceType_Metal;
	TargetASC->AddLooseGameplayTag(TargetOwnedTag);
	TStrongObjectPtr<UGGYGOPhysicalMaterialWithTags> PhysicalMaterial(
		NewObject<UGGYGOPhysicalMaterialWithTags>(TestWorld.World));
	if (!TestNotNull(TEXT("带表面 Tag 的物理材质"), PhysicalMaterial.Get()))
	{
		return false;
	}
	PhysicalMaterial->Tags.AddTag(SurfaceTag);

	FHitResult HitResult(TargetActor, nullptr, FVector(12.0, 23.0, 34.0), FVector(1.0, 2.0, 3.0));
	HitResult.ImpactPoint = FVector(15.0, 27.0, 39.0);
	HitResult.ImpactNormal = FVector::UpVector;
	HitResult.PhysMaterial = PhysicalMaterial.Get();
	const FVector ExplicitOrigin(1.0, 2.0, 3.0);

	FGGYGOHitEffectPayload WithEffectPayload;
	const bool bBuiltWithEffect = Ability->BuildHitEffectPayloadForTest(
		TargetASC, UGameplayEffect::StaticClass(), 1.0f, HitResult, ExplicitOrigin, WithEffectPayload);
	if (!TestTrue(TEXT("GE 路径构造命中载荷"), bBuiltWithEffect)
		|| !TestTrue(TEXT("GE 路径创建有效 Spec"), WithEffectPayload.EffectSpec.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("GE Cue 保留目标 Owned Tag"),
		WithEffectPayload.CueParameters.AggregatedTargetTags.HasTagExact(TargetOwnedTag));
	TestTrue(TEXT("GE Cue 保留物理材质 Tag"),
		WithEffectPayload.CueParameters.AggregatedTargetTags.HasTagExact(SurfaceTag));
	const FGGYGOGameplayEffectContext* BuiltContext =
		FGGYGOGameplayEffectContext::ExtractEffectContext(WithEffectPayload.EffectContext);
	if (!TestNotNull(TEXT("共享 builder 生成项目 EffectContext"), BuiltContext))
	{
		return false;
	}
	TestTrue(TEXT("共享 builder 标记显式来源快照"), BuiltContext->HasSourceOriginSnapshot());
	SourceActor->SetActorLocation(FVector(900.0, 800.0, 700.0));
	TestEqual(TEXT("来源 Actor 后续移动不改变命中距离快照"),
		BuiltContext->GetDistanceFromOriginToHitResult(),
		static_cast<float>(FVector::Dist(ExplicitOrigin, HitResult.ImpactPoint)));

	FGGYGOHitEffectPayload WithoutEffectPayload;
	const bool bBuiltWithoutEffect = Ability->BuildHitEffectPayloadForTest(
		TargetASC, nullptr, 1.0f, HitResult, ExplicitOrigin, WithoutEffectPayload);
	if (!TestTrue(TEXT("无 GE 路径仍构造命中 Cue 载荷"), bBuiltWithoutEffect)
		|| !TestFalse(TEXT("无 GE 路径没有 EffectSpec"), WithoutEffectPayload.EffectSpec.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("无 GE Cue 保留目标 Owned Tag"),
		WithoutEffectPayload.CueParameters.AggregatedTargetTags.HasTagExact(TargetOwnedTag));
	TestTrue(TEXT("无 GE Cue 保留物理材质 Tag"),
		WithoutEffectPayload.CueParameters.AggregatedTargetTags.HasTagExact(SurfaceTag));
	TestTrue(TEXT("无 GE Cue 仍携带命中上下文"),
		WithoutEffectPayload.CueParameters.EffectContext.GetHitResult() != nullptr);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHitPayloadSpecContextAndTargetCaptureTest,
	"GGYGO.AbilitySystem.HitSemantics.PayloadSpecContextAndTargetCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHitPayloadSpecContextAndTargetCaptureTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Spec 专项 GEngine"), GEngine))
	{
		return false;
	}
	FGGYGOHitSemanticsTestWorld TestWorld;
	if (!TestNotNull(TEXT("Spec 专项独立 World"), TestWorld.World)
		|| !TestNotNull(TEXT("Spec 专项 WorldContext"), GEngine->GetWorldContextFromWorld(TestWorld.World)))
	{
		return false;
	}

	AActor* SourceActor = nullptr;
	UGGYGOAbilitySystemComponent* SourceASC = MakeHitSemanticsTestASC(TestWorld.World, SourceActor);
	AActor* TargetActor = nullptr;
	UGGYGOAbilitySystemComponent* TargetASC = MakeHitSemanticsTestASC(TestWorld.World, TargetActor);
	if (!TestNotNull(TEXT("Spec 专项源 Actor"), SourceActor)
		|| !TestNotNull(TEXT("Spec 专项目标 Actor"), TargetActor)
		|| !TestNotNull(TEXT("Spec 专项源 ASC"), SourceASC)
		|| !TestNotNull(TEXT("Spec 专项目标 ASC"), TargetASC))
	{
		return false;
	}
	bool bPassed = true;
	bPassed &= TestTrue(TEXT("Spec 专项两个 ASC 已注册"), SourceASC->IsRegistered() && TargetASC->IsRegistered());
	bPassed &= TestTrue(TEXT("Spec 专项两个 ASC 均为权威"),
		SourceASC->IsOwnerActorAuthoritative() && TargetASC->IsOwnerActorAuthoritative());
	bPassed &= TestTrue(TEXT("Spec 专项源 Owner/Avatar 正确"),
		SourceASC->GetOwnerActor() == SourceActor && SourceASC->GetAvatarActor() == SourceActor);
	bPassed &= TestTrue(TEXT("Spec 专项目标 Owner/Avatar 正确"),
		TargetASC->GetOwnerActor() == TargetActor && TargetASC->GetAvatarActor() == TargetActor);
	bPassed &= TestTrue(TEXT("Spec 专项 Actor 可发现各自 ASC"),
		SourceActor->FindComponentByClass<UAbilitySystemComponent>() == SourceASC
		&& TargetActor->FindComponentByClass<UAbilitySystemComponent>() == TargetASC);
	const FGameplayEffectContextHandle ProbeContext = SourceASC->MakeEffectContext();
	bPassed &= TestNotNull(TEXT("Spec 专项工厂分配项目 Context"),
		FGGYGOGameplayEffectContext::ExtractEffectContext(ProbeContext));
	if (!bPassed)
	{
		return false;
	}

	const FGameplayAbilitySpecHandle AbilityHandle = SourceASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOHitSemanticsTestAbility::StaticClass(), 1));
	if (!TestTrue(TEXT("Spec 专项真实授予句柄有效"), AbilityHandle.IsValid())
		|| !TestTrue(TEXT("Spec 专项真实激活成功"), SourceASC->TryActivateAbility(AbilityHandle)))
	{
		return false;
	}
	FGameplayAbilitySpec* AbilitySpec = SourceASC->FindAbilitySpecFromHandle(AbilityHandle);
	UGGYGOHitSemanticsTestAbility* Ability = AbilitySpec
		? Cast<UGGYGOHitSemanticsTestAbility>(AbilitySpec->GetPrimaryInstance()) : nullptr;
	FGGYGOFinishHitSemanticsAbility FinishAbility{Ability};
	if (!TestNotNull(TEXT("Spec 专项实际能力实例"), Ability)
		|| !TestTrue(TEXT("Spec 专项能力保持激活"), Ability->IsActive()))
	{
		return false;
	}

	const FGameplayTag TargetOwnedTag = GGYGOGameplayTags::Gameplay_Damage_Immunity;
	const FGameplayTag SurfaceTag = GGYGOGameplayTags::SurfaceType_Metal;
	TargetASC->AddLooseGameplayTag(TargetOwnedTag);
	TStrongObjectPtr<UGGYGOPhysicalMaterialWithTags> PhysicalMaterial(
		NewObject<UGGYGOPhysicalMaterialWithTags>(TestWorld.World));
	if (!TestNotNull(TEXT("Spec 专项强引用物理材质"), PhysicalMaterial.Get()))
	{
		return false;
	}
	PhysicalMaterial->Tags.AddTag(SurfaceTag);
	if (!TestTrue(TEXT("Spec 专项目标 Owned Tag 已存在"), TargetASC->GetOwnedGameplayTags().HasTagExact(TargetOwnedTag))
		|| !TestTrue(TEXT("Spec 专项材质 Surface Tag 已存在"), PhysicalMaterial->Tags.HasTagExact(SurfaceTag)))
	{
		return false;
	}

	FHitResult InputHit(TargetActor, nullptr, FVector(12.0, 23.0, 34.0), FVector::UpVector);
	InputHit.TraceStart = FVector(300.0, 400.0, 500.0);
	InputHit.ImpactPoint = FVector(15.0, 27.0, 39.0);
	InputHit.ImpactNormal = FVector::UpVector;
	InputHit.PhysMaterial = PhysicalMaterial.Get();
	const FHitResult ExpectedHit = InputHit;
	const FVector ExplicitOrigin(1.0, 2.0, 3.0);
	FGGYGOHitEffectPayload Payload;
	if (!TestTrue(TEXT("Spec 专项实际 Builder 成功"), Ability->BuildHitEffectPayloadForTest(
		TargetASC, UGameplayEffect::StaticClass(), 1.0f, InputHit, ExplicitOrigin, Payload))
		|| !TestTrue(TEXT("Spec 专项实际 Spec 有效"), Payload.EffectSpec.IsValid()))
	{
		return false;
	}
	const FGameplayEffectSpec& BuiltSpec = *Payload.EffectSpec.Data.Get();
	const UGameplayEffect* Definition = BuiltSpec.Def;
	if (!TestNotNull(TEXT("Spec 专项基础 GE 定义"), Definition))
	{
		return false;
	}
	bPassed &= TestTrue(TEXT("Spec 专项使用未修改的基础 Instant GE"),
		Definition == GetDefault<UGameplayEffect>() && Definition->DurationPolicy == EGameplayEffectDurationType::Instant);
	bPassed &= TestTrue(TEXT("Spec 专项基础 GE 无 Modifier/Execution/Cue"),
		Definition->Modifiers.IsEmpty() && Definition->Executions.IsEmpty() && Definition->GameplayCues.IsEmpty());
	bPassed &= TestTrue(TEXT("构造 Spec 的 SpecTags 含 Surface"), BuiltSpec.CapturedTargetTags.GetSpecTags().HasTagExact(SurfaceTag));
	bPassed &= TestTrue(TEXT("构造 Spec 的聚合 Tags 含 Surface"), BuiltSpec.CapturedTargetTags.GetAggregatedTags()->HasTagExact(SurfaceTag));
	bPassed &= TestFalse(TEXT("构造 Spec 尚未捕获目标 Owned Tag"), BuiltSpec.CapturedTargetTags.GetAggregatedTags()->HasTagExact(TargetOwnedTag));
	bPassed &= TestTrue(TEXT("构造 Cue 含 Owned"), Payload.CueParameters.AggregatedTargetTags.HasTagExact(TargetOwnedTag));
	bPassed &= TestTrue(TEXT("构造 Cue 含 Surface"), Payload.CueParameters.AggregatedTargetTags.HasTagExact(SurfaceTag));
	bPassed &= TestTrue(TEXT("Spec/Payload/Cue 实际共享同一 Context"),
		BuiltSpec.GetContext().Get() == Payload.EffectContext.Get()
		&& Payload.CueParameters.EffectContext.Get() == Payload.EffectContext.Get());
	bPassed &= TestTrue(TEXT("Context 缓存正确源 ASC"), Payload.EffectContext.GetInstigatorAbilitySystemComponent() == SourceASC);

	auto CheckContext = [this, &ExpectedHit, &ExplicitOrigin, &PhysicalMaterial, TargetActor](
		const TCHAR* Label, const FGameplayEffectContextHandle& ContextHandle)
	{
		const FGGYGOGameplayEffectContext* Context = FGGYGOGameplayEffectContext::ExtractEffectContext(ContextHandle);
		if (!TestNotNull(FString::Printf(TEXT("%s 项目 Context"), Label), Context)) { return false; }
		bool bContextPassed = true;
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 显式快照标志"), Label), Context->HasSourceOriginSnapshot());
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 明确 Origin 保持"), Label), Context->GetOrigin().Equals(ExplicitOrigin));
		const FHitResult* Hit = Context->GetHitResult();
		if (!TestNotNull(FString::Printf(TEXT("%s HitResult"), Label), Hit)) { return false; }
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 命中目标保持"), Label), Hit->GetActor() == TargetActor);
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 命中点保持"), Label), Hit->ImpactPoint.Equals(ExpectedHit.ImpactPoint));
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 命中法线保持"), Label), Hit->ImpactNormal.Equals(ExpectedHit.ImpactNormal));
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 原材质对象身份保持"), Label),
			Hit->PhysMaterial.Get() == PhysicalMaterial.Get() && Context->GetPhysicalMaterial() == PhysicalMaterial.Get());
		return bContextPassed;
	};
	bPassed &= CheckContext(TEXT("构造 Payload"), Payload.EffectContext);
	bPassed &= CheckContext(TEXT("构造 Spec"), BuiltSpec.GetContext());
	bPassed &= CheckContext(TEXT("构造 Cue"), Payload.CueParameters.EffectContext);
	bPassed &= TestTrue(TEXT("Builder 拷贝命中数据而非引用输入"), Payload.EffectContext.GetHitResult() != &InputHit);
	InputHit = FHitResult(SourceActor, nullptr, FVector(900.0, 800.0, 700.0), FVector::ForwardVector);
	InputHit.ImpactPoint = FVector(900.0, 800.0, 700.0);
	InputHit.ImpactNormal = FVector::ForwardVector;
	InputHit.PhysMaterial = nullptr;
	bPassed &= CheckContext(TEXT("修改输入后 Payload"), Payload.EffectContext);
	bPassed &= CheckContext(TEXT("修改输入后 Spec"), BuiltSpec.GetContext());
	bPassed &= CheckContext(TEXT("修改输入后 Cue"), Payload.CueParameters.EffectContext);
	if (!bPassed) { return false; }

	int32 AppliedCount = 0;
	UAbilitySystemComponent* AppliedSourceASC = nullptr;
	FGameplayEffectSpec AppliedSpec;
	const FDelegateHandle ObservationHandle = TargetASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
		[&AppliedCount, &AppliedSourceASC, &AppliedSpec](UAbilitySystemComponent* Source,
			const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
		{
			++AppliedCount;
			AppliedSourceASC = Source;
			AppliedSpec = Spec;
		});
	ON_SCOPE_EXIT
	{
		TargetASC->OnGameplayEffectAppliedDelegateToSelf.Remove(ObservationHandle);
	};
	if (!TestTrue(TEXT("Spec 专项公开观察句柄有效"), ObservationHandle.IsValid())) { return false; }
	// Owned 由 GAS 在应用副本捕获；不向构造 Spec 的 ActorTags 手工写入。
	const FActiveGameplayEffectHandle ApplicationHandle = SourceASC->ApplyGameplayEffectSpecToTarget(BuiltSpec, TargetASC);
	bPassed &= TestTrue(TEXT("空 Instant GE 实际应用成功"), ApplicationHandle.WasSuccessfullyApplied());
	bPassed &= TestEqual(TEXT("目标公开应用回调恰好一次"), AppliedCount, 1);
	bPassed &= TestTrue(TEXT("目标公开应用回调来源正确"), AppliedSourceASC == SourceASC);
	if (!bPassed) { return false; }
	bPassed &= TestTrue(TEXT("实际应用副本 ActorTags 含目标 Owned"), AppliedSpec.CapturedTargetTags.GetActorTags().HasTagExact(TargetOwnedTag));
	bPassed &= TestTrue(TEXT("实际应用副本 SpecTags 保持 Surface"), AppliedSpec.CapturedTargetTags.GetSpecTags().HasTagExact(SurfaceTag));
	bPassed &= TestTrue(TEXT("实际应用副本聚合 Tags 含 Owned+Surface"),
		AppliedSpec.CapturedTargetTags.GetAggregatedTags()->HasTagExact(TargetOwnedTag)
		&& AppliedSpec.CapturedTargetTags.GetAggregatedTags()->HasTagExact(SurfaceTag));
	bPassed &= TestTrue(TEXT("应用副本 Context 与原载荷一致"), AppliedSpec.GetContext().Get() == Payload.EffectContext.Get());
	bPassed &= CheckContext(TEXT("实际应用副本"), AppliedSpec.GetContext());
	bPassed &= TestFalse(TEXT("原构造 Spec 未被应用副本反写 Owned"), BuiltSpec.CapturedTargetTags.GetAggregatedTags()->HasTagExact(TargetOwnedTag));
	if (!bPassed) { return false; }

	const FGameplayEffectContextHandle DuplicateHandle = BuiltSpec.GetContext().Duplicate();
	FGGYGOGameplayEffectContext* DuplicateContext = FGGYGOGameplayEffectContext::ExtractEffectContext(DuplicateHandle);
	if (!TestNotNull(TEXT("Spec 专项 Duplicate 为项目 Context"), DuplicateContext)) { return false; }
	bPassed &= TestTrue(TEXT("Duplicate Context 地址独立"), DuplicateHandle.Get() != Payload.EffectContext.Get());
	bPassed &= TestTrue(TEXT("Duplicate HitResult 地址独立"), DuplicateHandle.GetHitResult() != Payload.EffectContext.GetHitResult());
	bPassed &= CheckContext(TEXT("Duplicate"), DuplicateHandle);
	if (!bPassed) { return false; }
	DuplicateContext->AddHitResult(InputHit, /*bReset=*/true);
	DuplicateContext->SetSourceOriginSnapshot(FVector(600.0, 500.0, 400.0));
	bPassed &= TestTrue(TEXT("只修改副本的 Origin 已生效"), DuplicateContext->GetOrigin().Equals(FVector(600.0, 500.0, 400.0)));
	bPassed &= TestTrue(TEXT("只修改副本的命中与材质已生效"),
		DuplicateContext->GetHitResult() && DuplicateContext->GetHitResult()->GetActor() == SourceActor
		&& DuplicateContext->GetHitResult()->ImpactPoint.Equals(InputHit.ImpactPoint)
		&& DuplicateContext->GetPhysicalMaterial() == nullptr);
	bPassed &= CheckContext(TEXT("修改副本后原 Payload"), Payload.EffectContext);
	bPassed &= CheckContext(TEXT("修改副本后原 Spec"), BuiltSpec.GetContext());
	bPassed &= CheckContext(TEXT("修改副本后原 Cue"), Payload.CueParameters.EffectContext);
	bPassed &= CheckContext(TEXT("修改副本后应用 Spec"), AppliedSpec.GetContext());
	return bPassed;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHitDistanceExecutionOriginSnapshotTest,
	"GGYGO.AbilitySystem.HitSemantics.DistanceExecutionUsesOriginSnapshotAfterCauserMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOHitDistanceExecutionOriginSnapshotTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("距离专项构建包含服务器伤害执行"), WITH_SERVER_CODE != 0)
		|| !TestNotNull(TEXT("距离专项 GEngine"), GEngine)) { return false; }
	FGGYGOHitSemanticsTestWorld TestWorld;
	if (!TestNotNull(TEXT("距离专项独立 World"), TestWorld.World)
		|| !TestNotNull(TEXT("距离专项 WorldContext"), GEngine->GetWorldContextFromWorld(TestWorld.World))) { return false; }

	AActor* SourceActor = nullptr;
	UGGYGOAbilitySystemComponent* SourceASC = MakeHitSemanticsTestASC(TestWorld.World, SourceActor);
	AActor* TargetActor = nullptr;
	UGGYGOAbilitySystemComponent* TargetASC = MakeHitSemanticsTestASC(TestWorld.World, TargetActor);
	if (!TestNotNull(TEXT("距离专项源 Actor"), SourceActor)
		|| !TestNotNull(TEXT("距离专项目标 Actor"), TargetActor)
		|| !TestNotNull(TEXT("距离专项源 ASC"), SourceASC)
		|| !TestNotNull(TEXT("距离专项目标 ASC"), TargetASC)) { return false; }
	bool bPassed = true;
	bPassed &= TestTrue(TEXT("距离专项 ASC 已注册且均为权威"),
		SourceASC->IsRegistered() && TargetASC->IsRegistered()
		&& SourceASC->IsOwnerActorAuthoritative() && TargetASC->IsOwnerActorAuthoritative());
	bPassed &= TestTrue(TEXT("距离专项双方 Owner/Avatar 与 Actor 发现路径正确"),
		SourceASC->GetOwnerActor() == SourceActor && SourceASC->GetAvatarActor() == SourceActor
		&& TargetASC->GetOwnerActor() == TargetActor && TargetASC->GetAvatarActor() == TargetActor
		&& SourceActor->FindComponentByClass<UAbilitySystemComponent>() == SourceASC
		&& TargetActor->FindComponentByClass<UAbilitySystemComponent>() == TargetASC);
	const FGameplayEffectContextHandle ProbeContext = SourceASC->MakeEffectContext();
	bPassed &= TestNotNull(TEXT("距离专项工厂分配项目 Context"), FGGYGOGameplayEffectContext::ExtractEffectContext(ProbeContext));
	if (!bPassed) { return false; }

	USceneComponent* SourceRoot = NewObject<USceneComponent>(SourceActor, NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("距离专项源 SceneRoot"), SourceRoot)) { return false; }
	SourceRoot->SetMobility(EComponentMobility::Movable);
	SourceActor->AddInstanceComponent(SourceRoot);
	if (!TestTrue(TEXT("距离专项设置自有 Root 成功"), SourceActor->SetRootComponent(SourceRoot))) { return false; }
	SourceRoot->RegisterComponent();
	bPassed &= TestTrue(TEXT("距离专项 Root 已注册、可移动且归属正确"),
		SourceRoot->IsRegistered() && SourceRoot->GetMobility() == EComponentMobility::Movable
		&& SourceRoot->GetOwner() == SourceActor && SourceRoot->GetWorld() == TestWorld.World
		&& SourceActor->GetRootComponent() == SourceRoot);
	if (!bPassed) { return false; }
	const FVector InitialLocation(10.0, 20.0, 30.0);
	if (!TestTrue(TEXT("距离专项初始 SetActorLocation 实际成功"), SourceActor->SetActorLocation(InitialLocation))) { return false; }
	bPassed &= TestTrue(TEXT("距离专项 Actor/Root 初始世界坐标正确"),
		SourceActor->GetActorLocation().Equals(InitialLocation) && SourceRoot->GetComponentLocation().Equals(InitialLocation));
	if (!bPassed) { return false; }

	TStrongObjectPtr<UGGYGOHitDistanceTestSource> SourceObject(NewObject<UGGYGOHitDistanceTestSource>(SourceActor, NAME_None, RF_Transient));
	TStrongObjectPtr<UGGYGOCombatSet> CombatSet(NewObject<UGGYGOCombatSet>(SourceActor, NAME_None, RF_Transient));
	TStrongObjectPtr<UGGYGOHealthSet> HealthSet(NewObject<UGGYGOHealthSet>(TargetActor, NAME_None, RF_Transient));
	if (!TestNotNull(TEXT("距离专项强引用来源对象"), SourceObject.Get())
		|| !TestNotNull(TEXT("距离专项真实 CombatSet"), CombatSet.Get())
		|| !TestNotNull(TEXT("距离专项真实 HealthSet"), HealthSet.Get())) { return false; }
	const IGGYGOAbilitySourceInterface* AbilitySource = Cast<IGGYGOAbilitySourceInterface>(SourceObject.Get());
	if (!TestNotNull(TEXT("距离专项来源实现公开接口"), AbilitySource)
		|| !TestTrue(TEXT("距离专项来源 Actor Outer 合法"), SourceObject->GetOuter() == SourceActor)) { return false; }
	SourceASC->AddAttributeSetSubobject(CombatSet.Get());
	TargetASC->AddAttributeSetSubobject(HealthSet.Get());
	ON_SCOPE_EXIT
	{
		TargetASC->RemoveSpawnedAttribute(HealthSet.Get());
		SourceASC->RemoveSpawnedAttribute(CombatSet.Get());
	};
	bPassed &= TestTrue(TEXT("距离专项属性集 Outer/Actor/ASC 唯一归属正确"),
		CombatSet->GetOuter() == SourceActor && HealthSet->GetOuter() == TargetActor
		&& CombatSet->GetOwningActor() == SourceActor && HealthSet->GetOwningActor() == TargetActor
		&& CombatSet->GetOwningAbilitySystemComponent() == SourceASC && HealthSet->GetOwningAbilitySystemComponent() == TargetASC
		&& SourceASC->GetSet<UGGYGOCombatSet>() == CombatSet.Get() && TargetASC->GetSet<UGGYGOHealthSet>() == HealthSet.Get());
	bPassed &= TestTrue(TEXT("距离专项真实捕获/输出属性存在"),
		SourceASC->HasAttributeSetForAttribute(UGGYGOCombatSet::GetBaseDamageAttribute())
		&& SourceASC->HasAttributeSetForAttribute(UGGYGOCombatSet::GetBasePoiseDamageAttribute())
		&& TargetASC->HasAttributeSetForAttribute(UGGYGOHealthSet::GetDamageAttribute())
		&& TargetASC->HasAttributeSetForAttribute(UGGYGOHealthSet::GetPoiseDamageAttribute()));
	if (!bPassed) { return false; }
	SourceASC->SetNumericAttributeBase(UGGYGOCombatSet::GetBaseDamageAttribute(), 84.0f);
	SourceASC->SetNumericAttributeBase(UGGYGOCombatSet::GetBasePoiseDamageAttribute(), 0.0f);
	TargetASC->SetNumericAttributeBase(UGGYGOHealthSet::GetMaxHealthAttribute(), 100.0f);
	TargetASC->SetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute(), 100.0f);
	TargetASC->SetNumericAttributeBase(UGGYGOHealthSet::GetMaxPoiseAttribute(), 50.0f);
	TargetASC->SetNumericAttributeBase(UGGYGOHealthSet::GetPoiseAttribute(), 50.0f);
	bPassed &= TestEqual(TEXT("距离专项源实际 BaseDamage 初始化"), CombatSet->GetBaseDamage(), 84.0f);
	bPassed &= TestEqual(TEXT("距离专项源实际 BasePoiseDamage 初始化"), CombatSet->GetBasePoiseDamage(), 0.0f);
	bPassed &= TestTrue(TEXT("距离专项目标实际承受属性初始化"),
		HealthSet->GetHealth() == 100.0f && HealthSet->GetMaxHealth() == 100.0f
		&& HealthSet->GetPoise() == 50.0f && HealthSet->GetMaxPoise() == 50.0f
		&& HealthSet->GetDamage() == 0.0f && HealthSet->GetPoiseDamage() == 0.0f);
	bPassed &= TestFalse(TEXT("距离专项目标没有免疫或无敌作弊 Tag"),
		TargetASC->HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity)
		|| TargetASC->HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_GodMode));
	if (!bPassed) { return false; }

	const FGameplayAbilitySpecHandle AbilityHandle = SourceASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOHitSemanticsTestAbility::StaticClass(), 1, INDEX_NONE, SourceObject.Get()));
	if (!TestTrue(TEXT("距离专项真实授予句柄有效"), AbilityHandle.IsValid())) { return false; }
	ON_SCOPE_EXIT { SourceASC->ClearAbility(AbilityHandle); };
	if (!TestTrue(TEXT("距离专项真实激活成功"), SourceASC->TryActivateAbility(AbilityHandle))) { return false; }
	FGameplayAbilitySpec* AbilitySpec = SourceASC->FindAbilitySpecFromHandle(AbilityHandle);
	UGGYGOHitSemanticsTestAbility* Ability = AbilitySpec ? Cast<UGGYGOHitSemanticsTestAbility>(AbilitySpec->GetPrimaryInstance()) : nullptr;
	FGGYGOFinishHitSemanticsAbility FinishAbility{Ability};
	if (!TestNotNull(TEXT("距离专项实际能力实例"), Ability)
		|| !TestTrue(TEXT("距离专项能力激活且真实 Spec SourceObject 正确"),
			Ability->IsActive() && AbilitySpec->SourceObject.Get() == SourceObject.Get())) { return false; }

	const FVector OriginSnapshot = SourceActor->GetActorLocation();
	const FVector HitPoint(13.0, 24.0, 30.0);
	FHitResult HitResult(TargetActor, nullptr, HitPoint, FVector::UpVector);
	HitResult.ImpactPoint = HitPoint;
	HitResult.ImpactNormal = FVector::UpVector;
	HitResult.TraceStart = FVector(-100.0, -200.0, -300.0);
	bPassed &= TestEqual(TEXT("距离专项显式初始距离为 5"), static_cast<float>(FVector::Dist(OriginSnapshot, HitPoint)), 5.0f);
	FGGYGOHitEffectPayload Payload;
	if (!bPassed || !TestTrue(TEXT("距离专项实际 Builder 成功"), Ability->BuildHitEffectPayloadForTest(
		TargetASC, UGGYGOHitDistanceTestEffect::StaticClass(), 1.0f, HitResult, OriginSnapshot, Payload))
		|| !TestTrue(TEXT("距离专项实际构造 Spec 有效"), Payload.EffectSpec.IsValid())) { return false; }
	const FGameplayEffectSpec& BuiltSpec = *Payload.EffectSpec.Data.Get();
	const UGameplayEffect* Definition = BuiltSpec.Def;
	if (!TestNotNull(TEXT("距离专项实际 GE 定义"), Definition)) { return false; }
	bPassed &= TestTrue(TEXT("距离专项 Instant GE 仅配置生产 Execution"),
		Definition == GetDefault<UGGYGOHitDistanceTestEffect>() && Definition->DurationPolicy == EGameplayEffectDurationType::Instant
		&& Definition->Modifiers.IsEmpty() && Definition->GameplayCues.IsEmpty() && Definition->Executions.Num() == 1
		&& Definition->Executions[0].CalculationClass == UGGYGODamageExecution::StaticClass());
	const UGGYGODamageExecution* ExecutionDefinition = GetDefault<UGGYGODamageExecution>();
	if (!TestNotNull(TEXT("距离专项生产 Execution 定义"), ExecutionDefinition)) { return false; }
	const TArray<FGameplayEffectAttributeCaptureDefinition>& CaptureDefinitions = ExecutionDefinition->GetAttributeCaptureDefinitions();
	bPassed &= TestTrue(TEXT("距离专项两项生产源属性捕获均真实有效"),
		CaptureDefinitions.Num() == 2 && BuiltSpec.HasValidCapturedAttributes(CaptureDefinitions));
	bPassed &= TestTrue(TEXT("距离专项没有 SetByCaller 覆盖捕获值"),
		BuiltSpec.SetByCallerTagMagnitudes.IsEmpty() && BuiltSpec.SetByCallerNameMagnitudes.IsEmpty());
	bPassed &= TestTrue(TEXT("距离专项构造阶段未调用来源衰减接口"),
		SourceObject->ObservedDistances.IsEmpty() && SourceObject->PhysicalMaterialCalls == 0);

	auto CheckContext = [this, &Payload, &OriginSnapshot, &HitPoint, &SourceObject, AbilitySource, SourceActor, SourceASC, TargetActor](
		const TCHAR* Label, const FGameplayEffectContextHandle& ContextHandle)
	{
		const FGGYGOGameplayEffectContext* Context = FGGYGOGameplayEffectContext::ExtractEffectContext(ContextHandle);
		if (!TestNotNull(FString::Printf(TEXT("%s 项目 Context"), Label), Context)) { return false; }
		bool bContextPassed = true;
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 保持同一 Context 和实际来源"), Label),
			ContextHandle.Get() == Payload.EffectContext.Get() && Context->GetAbilitySource() == AbilitySource
			&& ContextHandle.GetSourceObject() == SourceObject.Get() && ContextHandle.GetEffectCauser() == SourceActor
			&& ContextHandle.GetInstigator() == SourceActor && ContextHandle.GetInstigatorAbilitySystemComponent() == SourceASC);
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 显式 Origin 快照保持"), Label),
			Context->HasSourceOriginSnapshot() && Context->GetOrigin().Equals(OriginSnapshot));
		const FHitResult* Hit = Context->GetHitResult();
		if (!TestNotNull(FString::Printf(TEXT("%s 实际 HitResult"), Label), Hit)) { return false; }
		bContextPassed &= TestTrue(FString::Printf(TEXT("%s 命中目标/点/法线保持且无材质"), Label),
			Hit->GetActor() == TargetActor && Hit->ImpactPoint.Equals(HitPoint)
			&& Hit->ImpactNormal.Equals(FVector::UpVector) && Context->GetPhysicalMaterial() == nullptr);
		return bContextPassed;
	};
	bPassed &= CheckContext(TEXT("构造载荷"), BuiltSpec.GetContext());
	if (!bPassed) { return false; }

	TArray<FGameplayEffectSpec> AppliedSpecs;
	TArray<UAbilitySystemComponent*> AppliedSources;
	const FDelegateHandle ObservationHandle = TargetASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
		[&AppliedSpecs, &AppliedSources](UAbilitySystemComponent* Source, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
		{
			AppliedSources.Add(Source);
			AppliedSpecs.Add(Spec);
		});
	ON_SCOPE_EXIT { TargetASC->OnGameplayEffectAppliedDelegateToSelf.Remove(ObservationHandle); };
	if (!TestTrue(TEXT("距离专项公开应用观察句柄有效"), ObservationHandle.IsValid())) { return false; }
	const FActiveGameplayEffectHandle FirstApplication = SourceASC->ApplyGameplayEffectSpecToTarget(BuiltSpec, TargetASC);
	bPassed &= TestTrue(TEXT("距离专项第一次 Instant GE 实际应用成功"), FirstApplication.WasSuccessfullyApplied());
	bPassed &= TestTrue(TEXT("距离专项第一次来源/目标回调各恰好一次"),
		SourceObject->ObservedDistances.Num() == 1 && AppliedSpecs.Num() == 1 && AppliedSources.Num() == 1);
	if (!bPassed) { return false; }
	bPassed &= TestEqual(TEXT("生产 Execution 首次传给 Source 的距离为 5"), SourceObject->ObservedDistances[0], 5.0f);
	bPassed &= TestTrue(TEXT("距离专项首次公开回调来源 ASC 正确"), AppliedSources[0] == SourceASC);
	bPassed &= CheckContext(TEXT("第一次实际应用 Spec"), AppliedSpecs[0].GetContext());
	bPassed &= TestEqual(TEXT("距离专项实际 Execution/HealthSet 首次扣血至 86"), HealthSet->GetHealth(), 86.0f);
	bPassed &= TestTrue(TEXT("距离专项首次 Damage 已消费且未发生材质衰减或削韧"),
		HealthSet->GetDamage() == 0.0f && HealthSet->GetPoiseDamage() == 0.0f
		&& HealthSet->GetPoise() == 50.0f && SourceObject->PhysicalMaterialCalls == 0);
	if (!bPassed) { return false; }

	const FVector MovedLocation(13.0, 24.0, 43.0);
	if (!TestTrue(TEXT("距离专项 EffectCauser 后续 SetActorLocation 实际成功"), SourceActor->SetActorLocation(MovedLocation))) { return false; }
	bPassed &= TestTrue(TEXT("距离专项 Actor/Root 世界坐标确实移动"),
		SourceActor->GetActorLocation().Equals(MovedLocation) && SourceRoot->GetComponentLocation().Equals(MovedLocation)
		&& !SourceActor->GetActorLocation().Equals(OriginSnapshot));
	bPassed &= TestEqual(TEXT("距离专项移动后 EffectCauser 当前位置到命中点距离为 13"),
		static_cast<float>(FVector::Dist(SourceActor->GetActorLocation(), HitPoint)), 13.0f);
	bPassed &= CheckContext(TEXT("EffectCauser 移动后原构造 Spec"), BuiltSpec.GetContext());
	if (!bPassed) { return false; }

	const FActiveGameplayEffectHandle SecondApplication = SourceASC->ApplyGameplayEffectSpecToTarget(BuiltSpec, TargetASC);
	bPassed &= TestTrue(TEXT("距离专项同一构造 Spec 第二次 Instant GE 实际应用成功"), SecondApplication.WasSuccessfullyApplied());
	bPassed &= TestTrue(TEXT("距离专项两次来源/目标回调累计恰好两次"),
		SourceObject->ObservedDistances.Num() == 2 && AppliedSpecs.Num() == 2 && AppliedSources.Num() == 2);
	if (!bPassed) { return false; }
	bPassed &= TestTrue(TEXT("生产 Execution 两次仅传入显式快照距离 [5,5]"),
		SourceObject->ObservedDistances[0] == 5.0f && SourceObject->ObservedDistances[1] == 5.0f);
	bPassed &= TestTrue(TEXT("距离专项两次公开回调来源 ASC 均正确"), AppliedSources[0] == SourceASC && AppliedSources[1] == SourceASC);
	bPassed &= CheckContext(TEXT("移动后第二次实际应用 Spec"), AppliedSpecs[1].GetContext());
	bPassed &= CheckContext(TEXT("移动后保留的第一次实际应用 Spec"), AppliedSpecs[0].GetContext());
	bPassed &= TestEqual(TEXT("距离专项实际 Execution/HealthSet 第二次扣血至 72"), HealthSet->GetHealth(), 72.0f);
	bPassed &= TestTrue(TEXT("距离专项第二次 Damage 已消费且未发生材质衰减或削韧"),
		HealthSet->GetDamage() == 0.0f && HealthSet->GetPoiseDamage() == 0.0f
		&& HealthSet->GetPoise() == 50.0f && SourceObject->PhysicalMaterialCalls == 0);
	return bPassed;
}
#endif
