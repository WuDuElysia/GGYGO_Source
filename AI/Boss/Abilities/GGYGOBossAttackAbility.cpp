#include "AI/Boss/Abilities/GGYGOBossAttackAbility.h"
#include "AI/Boss/Abilities/GGYGOBossAudioActor.h"
#include "AI/Boss/Abilities/GGYGOBossProjectile.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "AbilitySystem/Abilities/GGYGOMontageActionResources.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Combat/HitDetection/GGYGOCombatHitQuery.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossAttackAbility)

/** 仅存本次激活的资源与来源；活动、死亡、冷却仍由 GAS/Health 决定。 */
struct UGGYGOBossAttackAbility::FAttackResources
{
	FGGYGOAbilityActivationHandle Original;
	TWeakObjectPtr<AActor> Avatar;
	TWeakObjectPtr<AActor> Target;
	TWeakObjectPtr<UGGYGOHealthComponent> AvatarHealth;
	TWeakObjectPtr<UGGYGOHealthComponent> TargetHealth;
	TSharedPtr<FGGYGOCombatHitQuery> AreaQuery;
	FGGYGOMontageWindowDeclaration AreaWindow;
	TArray<FGGYGOMontageWindowDeclaration> ConsumedWindows;
	TArray<TWeakObjectPtr<AGGYGOBossProjectile>> Projectiles;
	TArray<TWeakObjectPtr<AGGYGOBossAudioActor>> Audio;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> FlightMovement;
	int32 FlightHandle = INDEX_NONE;
	TWeakObjectPtr<ACharacter> LandingCharacter;
	FTimerHandle LandingTimeout;
	TSharedPtr<FGGYGOMontageActionResources> Recovery;
};

bool UGGYGOBossAttackAbility::IsAttackCurrent(const TSharedPtr<FAttackResources>& Resources) const
{
	return Resources && AttackResources == Resources && IsActivationCurrent(Resources->Original);
}

void UGGYGOBossAttackAbility::AbortAttack(const TSharedPtr<FAttackResources>& Resources, const FString& Reason)
{
	if (!IsAttackCurrent(Resources)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("BossAttack [%s] 原动作中止：%s"), *GetPathName(), *Reason);
	RequestAbilityCancel(Resources->Original, true);
}

void UGGYGOBossAttackAbility::InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original)
{
	if (!Original.HasActivation() || AttackResources)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("BossAttack [%s] 原身份为空或旧资源未归还，拒绝覆盖。"), *GetPathName());
		return;
	}
	AttackResources = MakeShared<FAttackResources>();
	AttackResources->Original = Original;
	AttackResources->Avatar = GetAvatarActorFromActorInfo();
	Super::InitializeAbilityActivation(Original);
}

bool UGGYGOBossAttackAbility::UsesMeleeTrace() const
{
	return HasConfiguredHitMode(EGGYGOBossHitMode::Melee);
}

bool UGGYGOBossAttackAbility::HasConfiguredHitMode(EGGYGOBossHitMode Mode) const
{
	if (WindowExecution.IsEmpty()) { return HitMode == Mode; }
	for (const auto& Pair : WindowExecution) { if (Pair.Value.Mode == Mode) { return true; } }
	return false;
}

bool UGGYGOBossAttackAbility::RequiresDamage() const
{
	if (WindowExecution.IsEmpty()) { return HitMode != EGGYGOBossHitMode::None; }
	for (const auto& Pair : WindowExecution) { if (Pair.Value.Mode != EGGYGOBossHitMode::None) { return true; } }
	return false;
}

void UGGYGOBossAttackAbility::GetWindowDamage(FGameplayTag Begin, float& OutDamage, float& OutPoise) const
{
	if (const FGGYGOBossWindowExecution* Config = WindowExecution.Find(Begin))
	{ OutDamage = Config->Damage; OutPoise = Config->PoiseDamage; }
	else { Super::GetWindowDamage(Begin, OutDamage, OutPoise); }
}

FGameplayTag UGGYGOBossAttackAbility::GetWindowHitCue(FGameplayTag Begin) const
{
	const FGGYGOBossWindowExecution* Config = WindowExecution.Find(Begin);
	return Config ? Config->HitCueTag : HitCueTag;
}

bool UGGYGOBossAttackAbility::ValidateBossAttackConfiguration(FString& Error) const
{
	if (static_cast<uint8>(HitMode) > static_cast<uint8>(EGGYGOBossHitMode::None))
	{ Error = TEXT("未知命中模式，拒绝选择其它查询代替。"); return false; }
	for (FName Function : {GET_FUNCTION_NAME_CHECKED(ThisClass, PrepareBossAction), GET_FUNCTION_NAME_CHECKED(ThisClass, MontageActionFinished),
		GET_FUNCTION_NAME_CHECKED(ThisClass, BossHitWindow), GET_FUNCTION_NAME_CHECKED(ThisClass, BossTargetInvalidated), GET_FUNCTION_NAME_CHECKED(ThisClass, BossTargetControlChanged)})
	{
		if (!GetClass()->IsFunctionImplementedInScript(Function))
		{ Error = TEXT("BossAttack 缺少实际共享BP策略处理：") + Function.ToString(); return false; }
	}
	const UGameplayEffect* Cooldown = GetCooldownGameplayEffect();
	if (!Cooldown || Cooldown->DurationPolicy != EGameplayEffectDurationType::HasDuration || Cooldown->GetGrantedTags().IsEmpty())
	{ Error = TEXT("BossAttack 必须配置具名Tag的真实GAS持续冷却GE。"); return false; }
	if (RequiresDamage() && HitWindowTags.IsEmpty())
	{ Error = TEXT("BossAttack 必须显式配置实际作者命中窗。"); return false; }
	if (static_cast<uint8>(QueryChannel) >= static_cast<uint8>(ECC_OverlapAll_Deprecated)
		|| AreaOffset.ContainsNaN() || ProjectileOffset.ContainsNaN())
	{ Error = TEXT("BossAttack 查询通道或几何偏移无效。"); return false; }
	if (!WindowExecution.IsEmpty())
	{
		if (WindowExecution.Num() != HitWindowTags.Num()) { Error = TEXT("组合动作的每个作者窗都必须显式选择执行模式。"); return false; }
		for (const FGGYGOBossWindowTags& Pair : HitWindowTags)
		{
			const FGGYGOBossWindowExecution* Config = WindowExecution.Find(Pair.Begin);
			if (!Config || !FMath::IsFinite(Config->Damage) || Config->Damage < 0.0f
				|| static_cast<uint8>(Config->Mode) > static_cast<uint8>(EGGYGOBossHitMode::None)
				|| !FMath::IsFinite(Config->PoiseDamage) || Config->PoiseDamage < 0.0f)
			{ Error = TEXT("组合动作窗口执行参数无效。"); return false; }
		}
	}
	if (HasConfiguredHitMode(EGGYGOBossHitMode::AreaSphere) && (!FMath::IsFinite(AreaRadius) || AreaRadius <= 0.0f))
	{ Error = TEXT("范围球半径必须有限且为正。"); return false; }
	if (HasConfiguredHitMode(EGGYGOBossHitMode::AreaBox) && (AreaHalfExtents.ContainsNaN()
		|| AreaHalfExtents.GetMin() <= 0.0f))
	{ Error = TEXT("范围盒三个半尺寸必须有限且为正。"); return false; }
	if (HasConfiguredHitMode(EGGYGOBossHitMode::Projectile)
		&& (!ProjectileSystem || !ProjectileVisualTransform.IsValid()
			|| !FMath::IsFinite(ProjectileRadius) || ProjectileRadius <= 0.0f
			|| !FMath::IsFinite(ProjectileSpeed) || ProjectileSpeed <= 0.0f
			|| !FMath::IsFinite(ProjectileLifetime) || ProjectileLifetime <= 0.0f
			|| !FMath::IsFinite(ProjectileGravity) || ProjectileGravity < 0.0f))
	{ Error = TEXT("弹体需要有限正半径/速度/寿命及有限非负重力。"); return false; }
	TSet<FGameplayTag> UsedTags;
	for (const FGGYGOBossWindowTags& Pair : HitWindowTags) { UsedTags.Add(Pair.Begin); UsedTags.Add(Pair.End); }
	for (const FGGYGOComboStepFX& FX : MontageFXWindows)
	{ if (FX.Mode != EGGYGOComboFXMode::Disabled) { UsedTags.Add(FX.BeginEventTag); UsedTags.Add(FX.EndEventTag); } }
	for (const FGGYGOBossAudioWindow& Audio : PersistentAudio)
	{
		FGGYGOMontageWindowDeclaration Window;
		if (!Audio.Sound || !FMath::IsFinite(Audio.Volume) || Audio.Volume < 0.0f
			|| !FMath::IsFinite(Audio.Pitch) || Audio.Pitch <= 0.0f || !Audio.RelativeTransform.IsValid()
			|| UsedTags.Contains(Audio.Events.Begin) || UsedTags.Contains(Audio.Events.End)
			|| !UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
				AttackMontage, Audio.Events.Begin, Audio.Events.End, 0.0f, AttackMontage->GetPlayLength(), Window, Error))
		{
			if (Error.IsEmpty()) { Error = TEXT("持续声需有效声音、TRS/音量/音高及独立原作者窗。"); }
			return false;
		}
		UsedTags.Add(Audio.Events.Begin); UsedTags.Add(Audio.Events.End);
	}
	Error.Reset();
	return true;
}

void UGGYGOBossAttackAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo Info, const FGameplayEventData* Event)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources)) { return; }
	ACharacter* Avatar = Cast<ACharacter>(Resources->Avatar.Get());
	Resources->AvatarHealth = UGGYGOHealthComponent::FindHealthComponent(Avatar);
	if (!Resources->AvatarHealth.IsValid() || Resources->AvatarHealth->IsDeadOrDying() || Resources->AvatarHealth->GetHealth() <= 0.0f)
	{ AbortAttack(Resources, TEXT("激活时需要存活原Avatar的Health资源。")); return; }
	Resources->AvatarHealth->OnDeathStarted.AddUniqueDynamic(this, &ThisClass::OnOriginalDeath);
	Resources->Avatar->OnEndPlay.AddUniqueDynamic(this, &ThisClass::OnOriginalEndPlay);
	Resources->Audio.SetNum(PersistentAudio.Num());
	USkeletalMeshComponent* Mesh = Avatar->GetMesh();
	if (!Mesh || (HasConfiguredHitMode(EGGYGOBossHitMode::Projectile)
		&& !ProjectileSocket.IsNone() && !Mesh->DoesSocketExist(ProjectileSocket)))
	{ AbortAttack(Resources, TEXT("弹体真实发射Socket或原Mesh无效。")); return; }
	for (const FGGYGOBossAudioWindow& Audio : PersistentAudio)
	{
		if (!Audio.Socket.IsNone() && !Mesh->DoesSocketExist(Audio.Socket))
		{ AbortAttack(Resources, FString::Printf(TEXT("持续声Socket [%s] 不存在。"), *Audio.Socket.ToString())); return; }
	}
	FGGYGOBossActionToken Token; Token.Original = Original;
	PrepareBossAction(Token);
	if (IsAttackCurrent(Resources)) { Super::ActivateAbilityBody(Original, Handle, ActorInfo, Info, Event); }
}

void UGGYGOBossAttackAbility::AppendMontageEventTags(FGameplayTagContainer& Tags) const
{
	for (const FGGYGOBossAudioWindow& Audio : PersistentAudio)
	{ Tags.AddTag(Audio.Events.Begin); Tags.AddTag(Audio.Events.End); }
}

bool UGGYGOBossAttackAbility::ConsumeAdditionalWindow(const FGGYGOAbilityActivationHandle& Original,
	const FGGYGOMontageWindowFact& Fact)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Original)) { return true; }
	for (int32 Index = 0; Index < PersistentAudio.Num(); ++Index)
	{
		const FGGYGOBossAudioWindow& Config = PersistentAudio[Index];
		if (Config.Events.Begin != Fact.Declaration.BeginEventTag || Config.Events.End != Fact.Declaration.EndEventTag) { continue; }
		if (Fact.Phase == EGGYGOMontageWindowPhase::End)
		{
			const TWeakObjectPtr<AGGYGOBossAudioActor> Audio = Resources->Audio[Index];
			Resources->Audio[Index].Reset();
			if (Audio.IsValid()) { Audio->Destroy(); }
		}
		else if (!Resources->ConsumedWindows.ContainsByPredicate([&Fact](const FGGYGOMontageWindowDeclaration& Window)
			{ return Window.HasSameWindow(Fact.Declaration); }))
		{
			Resources->ConsumedWindows.Add(Fact.Declaration);
			const FTransform Origin = Config.Socket.IsNone() ? Fact.Mesh->GetComponentTransform()
				: Fact.Mesh->GetSocketTransform(Config.Socket);
			const FTransform Transform = Config.RelativeTransform * Origin;
			AGGYGOBossAudioActor* Audio = GetWorld()->SpawnActorDeferred<AGGYGOBossAudioActor>(
				AGGYGOBossAudioActor::StaticClass(), Transform, Resources->Avatar.Get(), nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Audio) { AbortAttack(Resources, TEXT("持续声原资源创建失败。")); return true; }
			if (!IsAttackCurrent(Resources)) { Audio->Destroy(); return true; }
			Resources->Audio[Index] = Audio;
			Audio->Configure(Config.Sound, Config.Volume, Config.Pitch);
			Audio->AttachToComponent(Fact.Mesh, FAttachmentTransformRules::KeepWorldTransform, Config.Socket);
			UGameplayStatics::FinishSpawningActor(Audio, Transform);
		}
		return true;
	}
	const bool bHit = HitWindowTags.ContainsByPredicate([&Fact](const FGGYGOBossWindowTags& Pair)
	{ return Pair.Begin == Fact.Declaration.BeginEventTag && Pair.End == Fact.Declaration.EndEventTag; });
	if (!bHit) { return false; }
	FGGYGOBossActionToken Token; Token.Original = Original;
	BossHitWindow(Token, Fact, Fact.Mesh);
	return true;
}

bool UGGYGOBossAttackAbility::ExecuteBossWindow(FGGYGOBossActionToken Token, FGGYGOMontageWindowFact Window,
	const FTransform& AreaBasis, const FVector& ProjectileAimPoint, FString& Error)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	const FGGYGOMontageWindowFact& Fact = Window;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original)
		|| !HasOriginalActionReceivers(Token.Original) || !IsOriginalActionWindow(Token.Original, Fact)
		|| !AreaBasis.IsValid() || ProjectileAimPoint.ContainsNaN())
	{ Error = TEXT("命中执行需要原GA/Task认证窗口及显式有限世界几何。"); return false; }
	const FGGYGOBossWindowExecution* Execution = WindowExecution.Find(Fact.Declaration.BeginEventTag);
	const EGGYGOBossHitMode Mode = Execution ? Execution->Mode : HitMode;
	if (Mode == EGGYGOBossHitMode::Melee)
	{ ExecuteOriginalMeleeWindow(Token.Original, Fact); Error.Reset(); return IsAttackCurrent(Resources); }
	if (Fact.Phase == EGGYGOMontageWindowPhase::End)
	{
		if (Resources->AreaQuery && Resources->AreaWindow.HasSameWindow(Fact.Declaration))
		{ TSharedPtr<FGGYGOCombatHitQuery> Query = MoveTemp(Resources->AreaQuery); Query->Close(); }
		Error.Reset(); return true;
	}
	if (Resources->ConsumedWindows.ContainsByPredicate([&Fact](const FGGYGOMontageWindowDeclaration& Declaration)
		{ return Declaration.HasSameWindow(Fact.Declaration); })) { Error.Reset(); return true; }
	Resources->ConsumedWindows.Add(Fact.Declaration);
	OpenQueryWindow(Resources, Fact, Mode, AreaBasis, ProjectileAimPoint);
	Error.Reset(); return IsAttackCurrent(Resources);
}

void UGGYGOBossAttackAbility::OpenQueryWindow(const TSharedPtr<FAttackResources>& Resources,
	const FGGYGOMontageWindowFact& Fact, EGGYGOBossHitMode Mode,
	const FTransform& AreaBasis, const FVector& ProjectileAimPoint)
{
	float HitDamage = 0.0f, HitPoise = 0.0f;
	GetWindowDamage(Fact.Declaration.BeginEventTag, HitDamage, HitPoise);
	const FGameplayTag Cue = GetWindowHitCue(Fact.Declaration.BeginEventTag);
	if (Mode == EGGYGOBossHitMode::None) { return; }
	if (Mode == EGGYGOBossHitMode::Projectile)
	{
		const FTransform Socket = ProjectileSocket.IsNone() ? Fact.Mesh->GetComponentTransform()
			: Fact.Mesh->GetSocketTransform(ProjectileSocket);
		const FVector Start = Socket.TransformPosition(ProjectileOffset);
		const FVector Direction = ProjectileAimPoint - Start; // 瞄准点由BP选择；这里只计算底层发射几何。
		if (Direction.IsNearlyZero()) { AbortAttack(Resources, TEXT("发射点与显式瞄准点重合。")); return; }
		const FTransform Transform(Direction.Rotation(), Start);
		AGGYGOBossProjectile* Projectile = GetWorld()->SpawnActorDeferred<AGGYGOBossProjectile>(
			AGGYGOBossProjectile::StaticClass(), Transform, Resources->Avatar.Get(), nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Projectile) { AbortAttack(Resources, TEXT("原弹体创建失败。")); return; }
		if (!IsAttackCurrent(Resources)) { Projectile->Destroy(); return; }
		Resources->Projectiles.Add(Projectile);
		const TWeakPtr<FAttackResources> WeakResources(Resources);
		FString Error;
		if (!Projectile->Configure(Resources->Avatar.Get(), ProjectileRadius, ProjectileSpeed, ProjectileLifetime,
			ProjectileGravity, QueryChannel, ProjectileSystem, ProjectileVisualTransform, FGGYGOBossProjectileHit::CreateWeakLambda(this,
				[this, WeakResources, HitDamage, HitPoise, Cue](const FGGYGOCombatSweepFact& Hit) { ApplySweepFact(WeakResources.Pin(), Hit, HitDamage, HitPoise, Cue); }),
			FGGYGOBossProjectileFailure::CreateWeakLambda(this,
				[this, WeakResources](const FString& Reason) { AbortAttack(WeakResources.Pin(), Reason); }), Error))
		{ Projectile->Destroy(); AbortAttack(Resources, Error); return; }
		UGameplayStatics::FinishSpawningActor(Projectile, Transform);
		return;
	}
	if (Resources->AreaQuery)
	{ TSharedPtr<FGGYGOCombatHitQuery> Query = MoveTemp(Resources->AreaQuery); Query->Close(); }
	FString Error;
	Resources->AreaQuery = FGGYGOCombatHitQuery::Open(Resources->Avatar.Get(), Error);
	if (!Resources->AreaQuery) { AbortAttack(Resources, Error); return; }
	Resources->AreaWindow = Fact.Declaration;
	const FTransform& Basis = AreaBasis; // 范围基准由BP显式选择。
	const FVector Center = Basis.TransformPositionNoScale(AreaOffset);
	TArray<FGGYGOCombatOverlapFact> Hits;
	const bool bQueried = Mode == EGGYGOBossHitMode::AreaSphere
		? Resources->AreaQuery->OverlapSphere(Center, AreaRadius, QueryChannel, Hits, Error)
		: Resources->AreaQuery->OverlapBox(Center, Basis.GetRotation(), AreaHalfExtents, QueryChannel, Hits, Error);
	if (!bQueried) { AbortAttack(Resources, Error); return; }
	for (const FGGYGOCombatOverlapFact& Hit : Hits)
	{ if (!IsAttackCurrent(Resources)) { return; } ApplyAreaFact(Resources, Hit, HitDamage, HitPoise, Cue); }
}

void UGGYGOBossAttackAbility::ApplyAreaFact(const TSharedPtr<FAttackResources>& Resources,
	const FGGYGOCombatOverlapFact& Fact, float HitDamage, float HitPoise, FGameplayTag CueTag)
{
	if (!IsAttackCurrent(Resources) || Fact.SourceActor != Resources->Avatar) { return; }
	const TWeakObjectPtr<AActor> Target = Fact.Overlap.GetActor();
	const TWeakObjectPtr<UAbilitySystemComponent> TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target.Get());
	const TWeakObjectPtr<UAbilitySystemComponent> SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (!IsAttackCurrent(Resources) || !Target.IsValid() || !TargetASC.IsValid() || !SourceASC.IsValid() || Target == Resources->Avatar) { return; }
	FGameplayEffectContextHandle Context = MakeEffectContext(CurrentSpecHandle, CurrentActorInfo);
	if (!IsAttackCurrent(Resources)) { return; }
	FGGYGOGameplayEffectContext* Typed = FGGYGOGameplayEffectContext::ExtractEffectContext(Context);
	if (!Typed) { AbortAttack(Resources, TEXT("范围伤害Context创建失败。")); return; }
	// 原生 Overlap 没有接触点、法线或物材；只记录显式来源快照，不伪造 HitResult。
	Typed->SetSourceOriginSnapshot(Resources->Avatar->GetActorLocation());
	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(
		UGGYGOGameData::ResolveDamageGameplayEffect(DamageEffect, bUseSharedDamageEffectWhenUnset), GetAbilityLevel(), Context);
	if (!IsAttackCurrent(Resources) || !Target.IsValid() || !SourceASC.IsValid() || !TargetASC.IsValid()) { return; }
	if (!Spec.IsValid() || !IsValid(Spec.Data->Def.Get()) || !Spec.Data->GetContext().IsValid())
	{ AbortAttack(Resources, TEXT("范围伤害Spec创建失败。")); return; }
	FGameplayAbilitySpec* AbilitySpec = SourceASC->FindAbilitySpecFromHandle(CurrentSpecHandle);
	const TMap<FGameplayTag, float> Magnitudes = AbilitySpec ? AbilitySpec->SetByCallerTagMagnitudes : TMap<FGameplayTag, float>{};
	ApplyAbilityTagsToGameplayEffectSpec(*Spec.Data.Get(), AbilitySpec);
	if (!IsAttackCurrent(Resources) || !Target.IsValid() || !SourceASC.IsValid() || !TargetASC.IsValid()) { return; }
	Spec.Data->SetByCallerTagMagnitudes = Magnitudes;
	BP_EditSpecValues(Spec);
	if (!IsAttackCurrent(Resources) || !Target.IsValid() || !SourceASC.IsValid() || !TargetASC.IsValid()) { return; }
	if (!Spec.IsValid() || !IsValid(Spec.Data->Def.Get()) || !Spec.Data->GetContext().IsValid())
	{ AbortAttack(Resources, TEXT("范围伤害扩展返回无效Spec。")); return; }
	Spec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, HitDamage);
	Spec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, HitPoise);
	FGameplayCueParameters Cue;
	UAbilitySystemGlobals::Get().InitGameplayCueParameters_GESpec(Cue, *Spec.Data.Get());
	Cue.Location = Target->GetActorLocation(); // 明确的目标根表现锚，不是物理接触点。
	SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC.Get());
	if (IsAttackCurrent(Resources) && TargetASC.IsValid() && CueTag.IsValid()) { TargetASC->ExecuteGameplayCue(CueTag, Cue); }
}

void UGGYGOBossAttackAbility::ApplySweepFact(const TSharedPtr<FAttackResources>& Resources,
	const FGGYGOCombatSweepFact& Fact, float HitDamage, float HitPoise, FGameplayTag CueTag)
{
	if (!IsAttackCurrent(Resources) || Fact.SourceActor != Resources->Avatar) { return; }
	const TWeakObjectPtr<UAbilitySystemComponent> TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Fact.Hit.GetActor());
	if (!IsAttackCurrent(Resources) || !TargetASC.IsValid()) { return; }
	FGGYGOHitEffectPayload Payload;
	if (!BuildHitEffectPayload(TargetASC.Get(), UGGYGOGameData::ResolveDamageGameplayEffect(
		DamageEffect, bUseSharedDamageEffectWhenUnset), GetAbilityLevel(), Fact.Hit, Resources->Avatar->GetActorLocation(), Payload)) { return; }
	if (!IsAttackCurrent(Resources) || !TargetASC.IsValid()) { return; }
	Payload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, HitDamage);
	Payload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, HitPoise);
	GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToTarget(*Payload.EffectSpec.Data.Get(), TargetASC.Get());
	if (IsAttackCurrent(Resources) && TargetASC.IsValid() && CueTag.IsValid())
	{ TargetASC->ExecuteGameplayCue(CueTag, Payload.CueParameters); }
}

void UGGYGOBossAttackAbility::OnOriginalDeath(AActor* Actor)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources)) { return; }
	if (Resources->Avatar.Get(true) == Actor) { AbortAttack(Resources, TEXT("原Avatar开始死亡，归还自有资源。")); return; }
	if (Resources->Target.Get(true) == Actor)
	{ FGGYGOBossActionToken Token; Token.Original = Resources->Original; BossTargetInvalidated(Token, Actor, TEXT("原目标开始死亡。")); }
}

void UGGYGOBossAttackAbility::OnOriginalEndPlay(AActor* Actor, EEndPlayReason::Type Reason)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources)) { return; }
	if (Resources->Avatar.Get(true) == Actor) { AbortAttack(Resources, TEXT("原Avatar结束Play，归还自有资源。")); return; }
	if (Resources->Target.Get(true) == Actor)
	{ FGGYGOBossActionToken Token; Token.Original = Resources->Original; BossTargetInvalidated(Token, Actor, TEXT("原目标结束Play。")); }
}

void UGGYGOBossAttackAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	TSharedPtr<FAttackResources> Resources = AttackResources;
	if (Resources && Resources->Original.HasSameActivation(Context.GetOriginalActivation()))
	{
		AttackResources.Reset();
		if (Resources->LandingCharacter.IsValid())
		{ Resources->LandingCharacter->LandedDelegate.RemoveDynamic(this, &ThisClass::OnOwnedLanded); }
		if (Resources->Avatar.IsValid()) { Resources->Avatar->GetWorldTimerManager().ClearTimer(Resources->LandingTimeout); }
		if (Resources->AvatarHealth.IsValid()) { Resources->AvatarHealth->OnDeathStarted.RemoveDynamic(this, &ThisClass::OnOriginalDeath); }
		if (Resources->TargetHealth.IsValid()) { Resources->TargetHealth->OnDeathStarted.RemoveDynamic(this, &ThisClass::OnOriginalDeath); }
		if (AActor* Avatar = Resources->Avatar.Get(true)) { Avatar->OnEndPlay.RemoveDynamic(this, &ThisClass::OnOriginalEndPlay); }
		if (AActor* Target = Resources->Target.Get(true)) { Target->OnEndPlay.RemoveDynamic(this, &ThisClass::OnOriginalEndPlay); }
		if (APawn* Target = Cast<APawn>(Resources->Target.Get(true)))
		{ Target->ReceiveControllerChangedDelegate.RemoveDynamic(this, &ThisClass::OnOriginalTargetControllerChanged); }
		if (Resources->AreaQuery) { Resources->AreaQuery->Close(); }
		for (const auto& Audio : Resources->Audio) { if (Audio.IsValid()) { Audio->Destroy(); } }
		for (const auto& Projectile : Resources->Projectiles) { if (Projectile.IsValid()) { Projectile->Destroy(); } }
		if (Resources->Recovery)
		{
			Resources->Recovery->UnregisterTaskCallbacks();
			Resources->Recovery->ReleaseFX(!Context.WasCancelled());
			Resources->Recovery->ClearWatchdog();
			Resources->Recovery->ReleaseTask(true);
		}
	}
	else { Resources.Reset(); }
	Super::CleanupAbilityResourcesForTermination(Context);
	// 原 Motion/Task 已归还，再处理原模式 token；旧 token 不能修改外调中出生的后继。
	if (Resources && Resources->FlightMovement.IsValid() && Resources->FlightHandle != INDEX_NONE)
	{
		FString Error;
		if (!Resources->FlightMovement->ReleaseFlyingMovementMode(Resources->FlightHandle, Error))
		{ UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("BossAttack [%s] 原飞行模式归还失败：%s"), *GetPathName(), *Error); }
	}
}

bool UGGYGOBossAttackAbility::AcquireBossFlight(FGGYGOBossActionToken Token, FString& Error)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original) || Resources->FlightHandle != INDEX_NONE)
	{ Error = TEXT("借用飞行需要未借用的原GA资源。"); return false; }
	ACharacter* Avatar = Cast<ACharacter>(Resources->Avatar.Get());
	UGGYGOCharacterMovementComponent* Movement = Avatar ? Cast<UGGYGOCharacterMovementComponent>(Avatar->GetCharacterMovement()) : nullptr;
	if (!Movement) { Error = TEXT("原角色没有项目CMC。"); return false; }
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement(Movement);
	int32 Handle = INDEX_NONE;
	const bool bAcquired = Movement->AcquireFlyingMovementMode(Avatar, Handle, Error);
	if (!IsAttackCurrent(Resources))
	{ if (bAcquired && OriginalMovement.IsValid()) { FString ReleaseError; OriginalMovement->ReleaseFlyingMovementMode(Handle, ReleaseError); } return false; }
	if (!bAcquired) { return false; }
	if (!OriginalMovement.IsValid() || !Resources->Avatar.IsValid()
		|| Cast<ACharacter>(Resources->Avatar.Get())->GetCharacterMovement() != OriginalMovement.Get())
	{
		if (OriginalMovement.IsValid()) { FString ReleaseError; OriginalMovement->ReleaseFlyingMovementMode(Handle, ReleaseError); }
		Error = TEXT("飞行借用返回后原角色CMC已失效/改变。"); return false;
	}
	Resources->FlightMovement = OriginalMovement;
	Resources->FlightHandle = Handle;
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("BossAttack FlightGranted Ability=%s Avatar=%s CMC=%s Handle=%d Original=%p"),
		*GetName(), *GetNameSafe(Resources->Avatar.Get()), *GetNameSafe(OriginalMovement.Get()), Handle, Resources->Original.GetResourceIdentity().Get());
	return true;
}

bool UGGYGOBossAttackAbility::ReleaseBossFlight(FGGYGOBossActionToken Token, bool bEnterFalling, FString& Error)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original)
		|| !Resources->FlightMovement.IsValid() || Resources->FlightHandle == INDEX_NONE)
	{ Error = TEXT("释放飞行需要原GA持有的原CMC/token。"); return false; }
	const int32 Handle = Resources->FlightHandle;
	const bool bReleased = Resources->FlightMovement->ReleaseFlyingMovementMode(Handle, Error, bEnterFalling);
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("BossAttack FlightReleased Ability=%s Handle=%d Released=%d EnterFalling=%d Original=%p"),
		*GetName(), Handle, bReleased, bEnterFalling, Resources->Original.GetResourceIdentity().Get());
	if (bReleased && IsAttackCurrent(Resources)) { Resources->FlightHandle = INDEX_NONE; }
	return bReleased;
}

bool UGGYGOBossAttackAbility::WaitBossLanding(FGGYGOBossActionToken Token, float TimeoutSeconds, FString& Error)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original)
		|| !IsOriginalMontageActionComplete(Token.Original)
		|| !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0f || Resources->LandingCharacter.IsValid())
	{ Error = TEXT("等待落地需要原GA、正超时及空原订阅。"); return false; }
	ACharacter* Avatar = Cast<ACharacter>(Resources->Avatar.Get());
	if (!Avatar || !GetClass()->IsFunctionImplementedInScript(GET_FUNCTION_NAME_CHECKED(ThisClass, BossLanded)))
	{ Error = TEXT("等待落地需要原Character及BP BossLanded处理图。"); return false; }
	Resources->LandingCharacter = Avatar;
	Avatar->LandedDelegate.AddUniqueDynamic(this, &ThisClass::OnOwnedLanded);
	const TWeakPtr<FAttackResources> WeakResources(Resources);
	Avatar->GetWorldTimerManager().SetTimer(Resources->LandingTimeout,
		FTimerDelegate::CreateWeakLambda(this, [this, WeakResources]()
		{ AbortAttack(WeakResources.Pin(), TEXT("等待真实Landed超时。")); }), TimeoutSeconds, false);
	Error.Reset();
	return true;
}

void UGGYGOBossAttackAbility::OnOwnedLanded(const FHitResult& Hit)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->LandingCharacter.IsValid()
		|| !Hit.bBlockingHit || !Resources->FlightMovement.IsValid()
		|| Resources->LandingCharacter->GetCharacterMovement() != Resources->FlightMovement.Get()
		|| !Resources->FlightMovement->IsWalkable(Hit)) { return; }
	Resources->LandingCharacter->LandedDelegate.RemoveDynamic(this, &ThisClass::OnOwnedLanded);
	Resources->LandingCharacter->GetWorldTimerManager().ClearTimer(Resources->LandingTimeout);
	Resources->LandingCharacter.Reset();
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("BossAttack Landed Ability=%s Avatar=%s Floor=%s Z=%.3f Original=%p"),
		*GetName(), *GetNameSafe(Resources->Avatar.Get()), *GetNameSafe(Hit.GetActor()), Resources->Avatar->GetActorLocation().Z,
		Resources->Original.GetResourceIdentity().Get());
	FGGYGOBossActionToken Token; Token.Original = Resources->Original;
	BossLanded(Token, Hit);
}

void UGGYGOBossAttackAbility::OnOriginalTargetControllerChanged(APawn* Pawn, AController* OldController, AController* NewController)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (IsAttackCurrent(Resources) && Resources->Target.Get(true) == Pawn)
	{ FGGYGOBossActionToken Token; Token.Original = Resources->Original; BossTargetControlChanged(Token, NewController); }
}

void UGGYGOBossAttackAbility::FinishBossAction(FGGYGOBossActionToken Token, bool bCancelled)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original)) { return; }
	if (!bCancelled && (!IsOriginalMontageActionComplete(Token.Original)
		|| (Resources->Recovery && !Resources->Recovery->bMontageCompleted)))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("BossAttack [%s] 原Montage/CMC未完成，拒绝报告正常结束。"), *GetPathName());
		return;
	}
	if (bCancelled) { RequestAbilityCancel(Resources->Original, true); }
	else { RequestAbilityEnd(Resources->Original, true, false); }
}

void UGGYGOBossAttackAbility::OnOriginalMontageActionCompleted(const FGGYGOAbilityActivationHandle& Original)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Original)) { return; }
	FGGYGOBossActionToken Token; Token.Original = Original;
	MontageActionFinished(Token);
}

void UGGYGOBossAttackAbility::FailBossAction(FGGYGOBossActionToken Token, const FString& Reason)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (Resources && Resources->Original.HasSameActivation(Token.Original)) { AbortAttack(Resources, Reason); }
}

bool UGGYGOBossAttackAbility::PlayBossRecoveryPresentation(FGGYGOBossActionToken Token, UAnimMontage* Montage,
	float PlayRate, const FGGYGOComboStepFX& FX, FString& Error)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original)
		|| !IsOriginalMontageActionComplete(Token.Original) || Resources->Recovery)
	{ Error = TEXT("收尾播放需要原动作已完成且没有其它原收尾资源。"); return false; }
	if (!IsValid(Montage) || Montage->HasRootMotion() || !GetClass()->IsFunctionImplementedInScript(
		GET_FUNCTION_NAME_CHECKED(ThisClass, RecoveryMontageFinished)))
	{ Error = TEXT("纯姿态收尾需要禁用RootMotion的有效Montage及BP原收尾完成事件。"); return false; }
	const TSharedPtr<FGGYGOMontageActionResources> Recovery = MakeShared<FGGYGOMontageActionResources>();
	Recovery->Activation = Token.Original; Recovery->AbilityPath = GetPathName();
	// Source这里只证明原姿态/时间来源；不要求XYZ，也绝不交CMC执行。
	if (!GGYGOActionMotionSource::BuildSourceBinding(Montage, MotionSlot, TEXT("Main"), Recovery->Source, Error)
		|| !FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, FX, 0.0f, Montage->GetPlayLength(), Error, &Recovery->FXWindow)) { return false; }
	Recovery->FX = FX;
	Recovery->bExpectFXBegin = FX.Mode != EGGYGOComboFXMode::Disabled;
	Recovery->bFXWindowClosed = !Recovery->bExpectFXBegin;
	FGameplayTagContainer Tags;
	TArray<FGGYGOMontageWindowDeclaration> Windows;
	if (Recovery->bExpectFXBegin)
	{ Tags.AddTag(FX.BeginEventTag); Tags.AddTag(FX.EndEventTag); Windows.Add(Recovery->FXWindow); }
	float TaskRate = 0.0f;
	if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(Montage, PlayRate, TaskRate, Recovery->EffectiveRate))
	{ Error = TEXT("收尾Montage播放速率无效。"); return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("BossRecovery"), Montage, Tags, PlayRate);
	if (!IsAttackCurrent(Resources)) { if (IsValid(Task)) { Task->TaskOwnerEnded(); } return false; }
	if (!IsValid(Task) || !Recovery->AdoptTask(Task)) { Error = TEXT("收尾Task创建失败。"); return false; }
	Resources->Recovery = Recovery;
	const TWeakPtr<FAttackResources> WeakResources(Resources);
	const TWeakPtr<FGGYGOMontageActionResources> WeakRecovery(Recovery);
	const auto Current = [this, WeakResources, WeakRecovery]()
	{
		const auto Batch = WeakResources.Pin(); const auto Motion = WeakRecovery.Pin();
		return IsAttackCurrent(Batch) && Motion && Batch->Recovery == Motion;
	};
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	Callbacks.OnCompleted.BindWeakLambda(this, [this, WeakResources, WeakRecovery, Current](FGameplayTag, FGameplayEventData)
	{
		if (!Current()) { return; }
		if (WeakRecovery.Pin()->bExpectFXBegin && !WeakRecovery.Pin()->bFXBeginConsumed)
		{ AbortAttack(WeakResources.Pin(), TEXT("纯姿态收尾缺少原FX Begin事实。")); return; }
		WeakRecovery.Pin()->ReleaseFX(true);
		if (!Current()) { return; }
		WeakRecovery.Pin()->bMontageCompleted = true; // 原Task真实自然完成；该显式姿态模式没有CMC资源。
		CompleteRecovery(WeakResources.Pin());
	});
	Callbacks.OnBlendOut.BindWeakLambda(this, [this, WeakResources, WeakRecovery, Current](FGameplayTag, FGameplayEventData)
	{
		if (!Current()) { return; }
		if (WeakRecovery.Pin()->bExpectFXBegin && !WeakRecovery.Pin()->bFXBeginConsumed)
		{ AbortAttack(WeakResources.Pin(), TEXT("纯姿态自然混出缺少原FX Begin。")); return; }
		WeakRecovery.Pin()->ReleaseFX(true);
	});
	Callbacks.WindowReceived.BindWeakLambda(this, [this, WeakResources, WeakRecovery, Current](const FGGYGOMontageWindowFact& Fact)
	{
		if (!Current()) { return; }
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Sender = WeakRecovery.Pin()->GetTask();
		if (!Sender || !Sender->IsOriginalMontageWindowFact(Fact)) { return; }
		FGGYGOMontageFXLocalStartReadyDelegate Ready = FGGYGOMontageFXLocalStartReadyDelegate::CreateWeakLambda(this,
			[this, WeakResources](const FGGYGOMontageFXLocalStartReadyFact& ReadyFact) { ContinueRecoveryFX(WeakResources.Pin(), ReadyFact); });
		FString Reason;
		if (!WeakRecovery.Pin()->ConsumeFXWindow(this, GET_FUNCTION_NAME_CHECKED(ThisClass, OnRecoveryFXFailed), Fact, MoveTemp(Ready), Current, Reason))
		{ AbortAttack(WeakResources.Pin(), Reason); }
	});
	Callbacks.OnInterrupted.BindWeakLambda(this, [this, WeakResources, Current](FGameplayTag, FGameplayEventData)
	{ if (Current()) { AbortAttack(WeakResources.Pin(), TEXT("原收尾Montage被打断。")); } });
	Callbacks.OnCancelled = Callbacks.OnInterrupted;
	Callbacks.OnFailed.BindWeakLambda(this, [this, WeakResources, WeakRecovery, Current](const FGGYGOMontageTaskFailureFact& Fact)
	{ if (Current() && WeakRecovery.Pin()->MatchesFailure(Fact)) { AbortAttack(WeakResources.Pin(), Fact.Diagnostic); } });
	if (!Task->ConfigureOriginalMontageWindows(Windows, Error) || !Recovery->RegisterCallbacks(MoveTemp(Callbacks)))
	{ if (Error.IsEmpty()) { Error = TEXT("收尾Task原生回调注册失败。"); } return false; }
	if (!Recovery->ArmFixedWatchdog(GetWorld(), Montage->GetPlayLength() / Recovery->EffectiveRate + 2.0f,
		FTimerDelegate::CreateWeakLambda(this, [this, WeakResources, Current]()
		{ if (Current()) { AbortAttack(WeakResources.Pin(), TEXT("原收尾Montage/CMC完成超时。")); } }), Error)) { return false; }
	Task->ReadyForActivation();
	if (!Current()) { return false; }
	return true; // 明确PresentationOnly，不采样或新建CMC位移。
}

void UGGYGOBossAttackAbility::CompleteRecovery(const TSharedPtr<FAttackResources>& Resources)
{
	if (!IsAttackCurrent(Resources) || !Resources->Recovery || !Resources->Recovery->bMontageCompleted) { return; }
	Resources->Recovery->ClearWatchdog();
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("BossAttack RecoveryCompleted Ability=%s Original=%p"),
		*GetName(), Resources->Original.GetResourceIdentity().Get());
	FGGYGOBossActionToken Token; Token.Original = Resources->Original;
	RecoveryMontageFinished(Token);
}

ACharacter* UGGYGOBossAttackAbility::GetBossCharacter(FGGYGOBossActionToken Token) const
{
	return IsAttackCurrent(AttackResources) && AttackResources->Original.HasSameActivation(Token.Original)
		? Cast<ACharacter>(AttackResources->Avatar.Get()) : nullptr;
}

AActor* UGGYGOBossAttackAbility::GetBoundBossTarget(FGGYGOBossActionToken Token) const
{
	return IsAttackCurrent(AttackResources) && AttackResources->Original.HasSameActivation(Token.Original)
		? AttackResources->Target.Get() : nullptr;
}

bool UGGYGOBossAttackAbility::BindBossTarget(FGGYGOBossActionToken Token, AActor* Target, FString& Error)
{
	const TSharedPtr<FAttackResources> Resources = AttackResources;
	if (!IsAttackCurrent(Resources) || !Resources->Original.HasSameActivation(Token.Original)
		|| !IsValid(Target) || !Resources->Avatar.IsValid() || Target == Resources->Avatar.Get()
		|| Target->GetWorld() != Resources->Avatar->GetWorld()
		|| (!Resources->Target.IsExplicitlyNull() && Resources->Target.Get() != Target))
	{ Error = TEXT("绑定需要同世界的显式目标及未被覆盖的原资源。"); return false; }
	Resources->Target = Target;
	Resources->TargetHealth = UGGYGOHealthComponent::FindHealthComponent(Target);
	if (Resources->TargetHealth.IsValid()) { Resources->TargetHealth->OnDeathStarted.AddUniqueDynamic(this, &ThisClass::OnOriginalDeath); }
	Target->OnEndPlay.AddUniqueDynamic(this, &ThisClass::OnOriginalEndPlay);
	if (APawn* Pawn = Cast<APawn>(Target)) { Pawn->ReceiveControllerChangedDelegate.AddUniqueDynamic(this, &ThisClass::OnOriginalTargetControllerChanged); }
	Error.Reset(); return true;
}

void UGGYGOBossAttackAbility::ContinueRecoveryFX(const TSharedPtr<FAttackResources>& Resources,
	const FGGYGOMontageFXLocalStartReadyFact& Fact)
{
	if (!IsAttackCurrent(Resources) || !Resources->Recovery) { return; }
	FString Error;
	if (!Resources->Recovery->ContinueFXLocalStart(Fact, [this, Resources]() { return IsAttackCurrent(Resources); }, Error))
	{ AbortAttack(Resources, Error); }
}

void UGGYGOBossAttackAbility::OnRecoveryFXFailed(UGGYGOAbilityTask_PlayNiagaraEffect* Sender, const FString& Reason)
{
	const auto Resources = AttackResources;
	if (Resources && Resources->Recovery && Sender && Resources->Recovery->GetFXTask() == Sender) { AbortAttack(Resources, Reason); }
}
