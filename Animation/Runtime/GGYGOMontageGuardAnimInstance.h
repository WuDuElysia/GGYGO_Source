// Copyright Epic Games, Inc. All Rights Reserved.
/** @file GGYGOMontageGuardAnimInstance.h @brief 识别本次原生 Montage 播放所创建实例的返栈屏障，以及动作姿态契约提供者。 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/Runtime/GGYGOActionPoseContract.h"
#include "Animation/Runtime/GGYGOMontagePlayGuard.h"

#include "GGYGOMontageGuardAnimInstance.generated.h"

/**
 * 原生 Montage 播放的返栈屏障：只持有 GameThread 上借用的调用方作用域链，用来识别本次调用创建的实例。
 * 引擎 Montage 实例与 GAS 播放状态仍归各自原执行者所有。
 * 项目 AnimInstance 基类 UGGYGOAnimInstanceBase 继承本类；ASC 播放入口和 Montage Task 通过它取得本次实例身份及动作姿态契约。
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class GGYGO_API UGGYGOMontageGuardAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUninitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/** None explicitly selects ordinary native slots; a correction producer must be declared. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Animation|Action Pose")
	FName RequiredPoseCorrectionSlot = NAME_None;

	/** GT only, waits for the original native parallel evaluation before reading its node. */
	EGGYGOActionPoseContractAcquireResult AcquireActionPoseContract(UAnimMontage* OriginalMontage,
		FGGYGOActionPoseContractTicket& OutTicket, FString& OutDiagnostic) const;

	/** GT only. Does not refresh the original ticket, play, stop or evaluate a pose. */
	EGGYGOActionPoseContractPollResult PollActionPoseContract(
		const FGGYGOActionPoseContractTicket& OriginalTicket, FString& OutDiagnostic) const;

	/**
	 * Read-only game-thread query of the original instance, lifecycle, issued call ID and owner.
	 * Does not refresh lifecycle state or require a montage instance to exist/be active.
	 * No ASC/GA ownership or Accepted proof; unobserved owner A -> B -> A is not detectable.
	 */
	bool IsMontagePlayGuardIdentityCurrent(const FGGYGOMontagePlayGuardIdentity& Identity) const;

	/**
	 * Instantaneous game-thread view of the innermost live scope for the original coordinator.
	 * Unavailable queries reset outputs to NotEntered/false; no ancestor lookup or state refresh.
	 * Returned does not mean ASC Super finished; completed does not mean source publication.
	 * Requery after external calls. Neither success nor unavailable establishes Task/GA ownership.
	 */
	bool TryGetCurrentMontagePlayGuardStage(const UObject* CallerIdentity,
		EGGYGOMontagePlayGuardNativeStage& OutNativeStage, bool& bOutCompleted) const;

protected:
	virtual float Montage_PlayInternal(UAnimMontage* MontageToPlay,
		const FMontageBlendSettings& BlendInSettings, float InPlayRate = 1.f,
		EMontagePlayReturnType ReturnValueType = EMontagePlayReturnType::MontageLength,
		float InTimeToStartMontageAt = 0.f, bool bStopAllMontages = true) override;

private:
	friend class FGGYGOMontagePlayGuardScope;

	void RegisterScope(FGGYGOMontagePlayGuardScope& Scope);
	void UnregisterScope(FGGYGOMontagePlayGuardScope& Scope);
	bool CanExecuteScope(FGGYGOMontagePlayGuardScope& Scope);
	void CompleteScope(FGGYGOMontagePlayGuardScope& Scope);
	bool ValidateScopeContext(FGGYGOMontagePlayGuardScope& Scope);
	bool ValidateEnclosingScope(FGGYGOMontagePlayGuardScope& Scope);
	bool ObserveCreatedInstance(FGGYGOMontagePlayGuardScope& Scope);
	bool IsCreatedInstanceActive(const FGGYGOMontagePlayGuardScope& Scope);
	void PublishAcceptedCall(const FGGYGOMontagePlayGuardScope& Scope);
	void InvalidateGuardLifecycle();
	void RefreshLifecycleOwner();

	/** Nonowning; invalidation never clears it. Each old scope unregisters itself by identity. */
	TArray<FGGYGOMontagePlayGuardScope*> ScopeChain;
	/** Derived lifecycle observation only, not Pawn/Avatar ownership authority. */
	TWeakObjectPtr<UObject> LifecycleOwner;
	uint64 LifecycleGeneration = 0;
	uint64 LastCallId = 0;
	bool bHadLifecycleOwner = false;
	bool bLifecycleReady = false;
	bool bIdentityExhausted = false;
	bool bUnscopedNativeExecuting = false;
};
