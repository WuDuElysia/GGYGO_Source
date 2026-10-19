/** @file GGYGOAnimNotifyState_GameplayEventWindow.cpp @brief 保留公共事件并借出同次原 Notify 的本地发送事实。 */
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAnimNotifyState_GameplayEventWindow)

namespace GGYGOGameplayEventWindow
{
	struct FScopedDispatch;
	struct FScopedBranchingSource;
	// 仅 GT 同步借用；嵌套原生回调返回后恢复外层，不保留窗口或动画时钟。
	const FScopedDispatch* CurrentDispatch = nullptr;
	FScopedBranchingSource* CurrentBranchingSource = nullptr;

	struct FScopedDispatch
	{
		FScopedDispatch(const FGGYGOMontageWindowSourceData& InSource,
			const TWeakObjectPtr<AActor>& InOriginalOwner, const FGameplayEventData& InPayload)
			: Previous(CurrentDispatch), OriginalOwner(InOriginalOwner), Payload(InPayload)
		{
			FGGYGOMontageWindowSourceData Snapshot = InSource;
			Snapshot.DispatchIdentity.Reset();
			Source = MakeShared<FGGYGOMontageWindowSourceData>(MoveTemp(Snapshot));
			CurrentDispatch = this;
		}

		~FScopedDispatch()
		{
			check(CurrentDispatch == this);
			CurrentDispatch = Previous;
		}

		FScopedDispatch(const FScopedDispatch&) = delete;
		FScopedDispatch& operator=(const FScopedDispatch&) = delete;

		const FScopedDispatch* Previous;
		const TWeakObjectPtr<AActor> OriginalOwner;
		const FGameplayEventData& Payload;
		TSharedPtr<const FGGYGOMontageWindowSourceData> Source;
	};

	/** 仅借给本次 Super 的普通 Notify 转发；在 BP 回调前消费，重入不能冒用外层来源。 */
	struct FScopedBranchingSource
	{
		FScopedBranchingSource(const UGGYGOAnimNotifyState_GameplayEventWindow* InNotify,
			const FGGYGOMontageWindowSourceData& InSource, const TWeakObjectPtr<AActor>& InOriginalOwner)
			: Previous(CurrentBranchingSource), Notify(InNotify), Source(InSource), OriginalOwner(InOriginalOwner)
		{
			CurrentBranchingSource = this;
		}

		~FScopedBranchingSource()
		{
			check(CurrentBranchingSource == this);
			CurrentBranchingSource = Previous;
		}

		FScopedBranchingSource(const FScopedBranchingSource&) = delete;
		FScopedBranchingSource& operator=(const FScopedBranchingSource&) = delete;

		FScopedBranchingSource* Previous;
		const TWeakObjectPtr<const UGGYGOAnimNotifyState_GameplayEventWindow> Notify;
		const FGGYGOMontageWindowSourceData Source;
		const TWeakObjectPtr<AActor> OriginalOwner;
		bool bConsumed = false;
	};

	FGGYGOMontageWindowSourceData CaptureSource(USkeletalMeshComponent* MeshComp,
		UAnimSequenceBase* Animation, FGameplayTag EventTag, EGGYGOMontageWindowPhase Phase,
		const FAnimNotifyEvent* Event, int32 MontageInstanceId, bool bHasMontageInstanceContext)
	{
		check(IsInGameThread());
		FGGYGOMontageWindowSourceData Source;
		Source.EventTag = EventTag;
		Source.Animation = Animation;
		Source.Mesh = MeshComp;
		Source.AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
		Source.Phase = Phase;
		Source.MontageInstanceId = MontageInstanceId;
		Source.bHasMontageInstanceContext = bHasMontageInstanceContext;
		if (Event)
		{
			// End 可以来自引擎活动数组的副本；只复制 cooked 可用的对象和值，不保存事件指针。
			Source.Notify = Event->NotifyStateClass.Get();
			Source.BeginSeconds = Event->GetTime();
			Source.EndSeconds = Event->GetTime() + Event->GetDuration();
			Source.BeginTriggerSeconds = Event->GetTriggerTime();
			Source.EndTriggerSeconds = Event->GetEndTriggerTime();
			Source.bQueued = Event->MontageTickType == EMontageNotifyTickType::Queued;
			if (const UAnimNotifyState* Notify = Source.Notify.Get())
			{
				Source.bNoMergeOnConcurrentPlay = (Notify->NotifyStateBehaviorFlags
					& static_cast<uint8>(EAnimNotifyStateBehaviorFlags::NoMergeOnConcurrentPlay)) != 0;
			}
		}
		return Source;
	}

	FGGYGOMontageWindowSourceData CaptureSource(USkeletalMeshComponent* MeshComp,
		UAnimSequenceBase* Animation, FGameplayTag EventTag, EGGYGOMontageWindowPhase Phase,
		const FAnimNotifyEventReference& EventReference)
	{
		const UE::Anim::FAnimNotifyMontageInstanceContext* Context =
			EventReference.GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>();
		return CaptureSource(MeshComp, Animation, EventTag, Phase, EventReference.GetNotify(),
			Context ? Context->MontageInstanceID : INDEX_NONE, Context != nullptr);
	}

	void TakeBranchingSource(const UGGYGOAnimNotifyState_GameplayEventWindow* Notify,
		USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, EGGYGOMontageWindowPhase Phase,
		FGGYGOMontageWindowSourceData& OutSource, TWeakObjectPtr<AActor>& OutOriginalOwner)
	{
		FScopedBranchingSource* Pending = CurrentBranchingSource;
		if (!Pending || Pending->bConsumed || Pending->Notify.Get() != Notify || Pending->Source.Phase != Phase
			|| !Pending->Source.Mesh.HasSameIndexAndSerialNumber(TWeakObjectPtr<USkeletalMeshComponent>(MeshComp))
			|| !Pending->Source.Animation.HasSameIndexAndSerialNumber(TWeakObjectPtr<UAnimSequenceBase>(Animation)))
		{
			return;
		}
		Pending->bConsumed = true;
		OutSource = Pending->Source;
		OutOriginalOwner = Pending->OriginalOwner;
	}

	bool MatchesPublicPayload(const FGameplayEventData& Payload, const FGameplayEventData& Expected)
	{
		return Payload.EventTag == Expected.EventTag && Payload.Instigator == Expected.Instigator
			&& Payload.Target == Expected.Target && Payload.OptionalObject == Expected.OptionalObject
			&& Payload.OptionalObject2 == Expected.OptionalObject2 && Payload.EventMagnitude == Expected.EventMagnitude
			&& Payload.ContextHandle == Expected.ContextHandle && Payload.InstigatorTags == Expected.InstigatorTags
			&& Payload.TargetTags == Expected.TargetTags && Payload.TargetData.Num() == 0;
	}
}

void UGGYGOAnimNotifyState_GameplayEventWindow::NotifyBegin(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	FGGYGOMontageWindowSourceData Source = GGYGOGameplayEventWindow::CaptureSource(
		MeshComp, Animation, BeginEventTag, EGGYGOMontageWindowPhase::Begin, EventReference);
	const float LegacyEventMagnitude = Source.bHasMontageInstanceContext
		? static_cast<float>(static_cast<int64>(Source.MontageInstanceId) + 1) : 0.0f;
	TWeakObjectPtr<AActor> OriginalOwner(MeshComp ? MeshComp->GetOwner() : nullptr);
	GGYGOGameplayEventWindow::TakeBranchingSource(this, MeshComp, Animation,
		EGGYGOMontageWindowPhase::Begin, Source, OriginalOwner);
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);
	SendEvent(BeginEventTag, Source, OriginalOwner, LegacyEventMagnitude);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::NotifyEnd(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	FGGYGOMontageWindowSourceData Source = GGYGOGameplayEventWindow::CaptureSource(
		MeshComp, Animation, EndEventTag, EGGYGOMontageWindowPhase::End, EventReference);
	const float LegacyEventMagnitude = Source.bHasMontageInstanceContext
		? static_cast<float>(static_cast<int64>(Source.MontageInstanceId) + 1) : 0.0f;
	TWeakObjectPtr<AActor> OriginalOwner(MeshComp ? MeshComp->GetOwner() : nullptr);
	GGYGOGameplayEventWindow::TakeBranchingSource(this, MeshComp, Animation,
		EGGYGOMontageWindowPhase::End, Source, OriginalOwner);
	Super::NotifyEnd(MeshComp, Animation, EventReference);
	SendEvent(EndEventTag, Source, OriginalOwner, LegacyEventMagnitude);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::BranchingPointNotifyBegin(
	FBranchingPointNotifyPayload& BranchingPointPayload)
{
	const FGGYGOMontageWindowSourceData Source = GGYGOGameplayEventWindow::CaptureSource(
		BranchingPointPayload.SkelMeshComponent, BranchingPointPayload.SequenceAsset, BeginEventTag,
		EGGYGOMontageWindowPhase::Begin, BranchingPointPayload.NotifyEvent,
		BranchingPointPayload.MontageInstanceID, true);
	const TWeakObjectPtr<AActor> OriginalOwner(BranchingPointPayload.SkelMeshComponent
		? BranchingPointPayload.SkelMeshComponent->GetOwner() : nullptr);
	GGYGOGameplayEventWindow::FScopedBranchingSource Forward(this, Source, OriginalOwner);
	// 保留引擎虚调用、BP 空 EventReference 及原公开事件顺序，不手补通知引用。
	Super::BranchingPointNotifyBegin(BranchingPointPayload);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::BranchingPointNotifyEnd(
	FBranchingPointNotifyPayload& BranchingPointPayload)
{
	const FGGYGOMontageWindowSourceData Source = GGYGOGameplayEventWindow::CaptureSource(
		BranchingPointPayload.SkelMeshComponent, BranchingPointPayload.SequenceAsset, EndEventTag,
		EGGYGOMontageWindowPhase::End, BranchingPointPayload.NotifyEvent,
		BranchingPointPayload.MontageInstanceID, true);
	const TWeakObjectPtr<AActor> OriginalOwner(BranchingPointPayload.SkelMeshComponent
		? BranchingPointPayload.SkelMeshComponent->GetOwner() : nullptr);
	GGYGOGameplayEventWindow::FScopedBranchingSource Forward(this, Source, OriginalOwner);
	// bReachedEnd 的上下文仍由原 Super 构造，内部来源不改变传给 BP 的引用。
	Super::BranchingPointNotifyEnd(BranchingPointPayload);
}

bool UGGYGOAnimNotifyState_GameplayEventWindow::TryReadWindowDispatch(
	const FGameplayEventData* Payload, FGGYGOMontageWindowSourceData& OutSource)
{
	OutSource = {};
	if (!IsInGameThread() || !Payload || !GGYGOGameplayEventWindow::CurrentDispatch)
	{
		return false;
	}
	const GGYGOGameplayEventWindow::FScopedDispatch& Dispatch = *GGYGOGameplayEventWindow::CurrentDispatch;
	if (!GGYGOGameplayEventWindow::MatchesPublicPayload(*Payload, Dispatch.Payload))
	{
		return false;
	}
	const FGGYGOMontageWindowSourceData& Source = *Dispatch.Source;
	USkeletalMeshComponent* Mesh = Source.Mesh.Get();
	AActor* Owner = Dispatch.OriginalOwner.Get();
	UAnimInstance* Anim = Source.AnimInstance.Get();
	// Super 的 BP 回调及 ASC 查询均可重入；原对象失效时不把后继装配当成原发送源。
	if (!Mesh || !Owner || Owner->IsActorBeingDestroyed() || Mesh->GetOwner() != Owner
		|| !Anim || Mesh->GetAnimInstance() != Anim || !Source.Animation.IsValid() || !Source.Notify.IsValid()
		|| Source.EventTag != Payload->EventTag || Payload->Instigator.Get() != Owner || Payload->Target.Get() != Owner
		|| Payload->OptionalObject.Get() != Source.Animation.Get() || Payload->OptionalObject2.Get() != Mesh)
	{
		return false;
	}
	OutSource = Source;
	// 身份与不可变快照共用控制块；不写回快照，防止强引用自身。
	OutSource.DispatchIdentity = TSharedPtr<const void>(Dispatch.Source, Dispatch.Source.Get());
	return true;
}

void UGGYGOAnimNotifyState_GameplayEventWindow::SendEvent(
	FGameplayTag EventTag, const FGGYGOMontageWindowSourceData& Source,
	const TWeakObjectPtr<AActor>& OriginalOwner, float LegacyEventMagnitude) const
{
	USkeletalMeshComponent* MeshComp = Source.Mesh.Get();
	AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
	if (!IsValid(Owner) || !EventTag.IsValid())
	{
		return;
	}
	const TWeakObjectPtr<AActor> DispatchOwner(Owner);
	if (!UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Owner))
	{
		// Persona 等预览 Actor 可以没有 ASC；游戏世界缺少接收者仍需要诊断。
		Owner = DispatchOwner.Get();
		const UWorld* World = Owner ? Owner->GetWorld() : nullptr;
		if (World && World->IsGameWorld())
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("GameplayEventWindow：游戏 Actor [%s] 没有 ASC，无法发送 [%s]，动画 [%s]。"),
				*Owner->GetPathName(), *EventTag.ToString(), *GetNameSafe(Source.Animation.Get()));
		}
		return;
	}

	Owner = DispatchOwner.Get();
	MeshComp = Source.Mesh.Get();
	UAnimSequenceBase* Animation = Source.Animation.Get();
	if (!Owner || Owner->IsActorBeingDestroyed() || !MeshComp || MeshComp->GetOwner() != Owner
		|| (!Source.Animation.IsExplicitlyNull() && !Animation))
	{
		return;
	}
	FGameplayEventData Payload;
	Payload.EventTag = EventTag;
	Payload.Instigator = Owner;
	Payload.Target = Owner;
	Payload.OptionalObject = Animation;
	Payload.OptionalObject2 = MeshComp;
	// 公开提示只取原 EventReference；Branching 原空引用仍为 0，内部整数来源独立传递。
	Payload.EventMagnitude = LegacyEventMagnitude;
	const GGYGOGameplayEventWindow::FScopedDispatch Dispatch(Source, OriginalOwner, Payload);
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Owner, EventTag, Payload);
}
