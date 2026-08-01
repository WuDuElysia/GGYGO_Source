/**
 * @file GGYGOStateManager.cpp
 * @brief 并行叠加状态管理器实现（纯 C++ 版本）
 */
#include "StateMachine/GGYGOStateManager.h"
#include "StateMachine/CharacterState.h"
#include "Data/Logic/RuntimeData.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"

// 所有具体状态头文件
#include "StateMachine/State/IdleState.h"
#include "StateMachine/State/MovingState.h"

// ============================================================
// 构造 / 析构
// ============================================================

FGYGOStateManager::FGYGOStateManager()
{
}

FGYGOStateManager::~FGYGOStateManager()
{
}

// ============================================================
// 初始化
// ============================================================

void FGYGOStateManager::Init(FRuntimeData& InRuntimeData, UDataTable* InRelationTable)
{
	RuntimeData = &InRuntimeData;

	// ---- 注册所有状态实例（TUniquePtr 持有所有权）----

	RegisterState<FIdleState>(ECharacterStateType::Idle);
	RegisterState<FMovingState>(ECharacterStateType::Moving);

	// ---- 加载关系矩阵 ----
	LoadRelationMatrix(InRelationTable);

	// ---- 默认进入 Idle ----
	TUniquePtr<FCharacterState>* IdlePtr = AllStates.Find(ECharacterStateType::Idle);
	if (IdlePtr && *IdlePtr)
	{
		ActivateState(IdlePtr->Get());
	}
}

void FGYGOStateManager::InitASC(UAbilitySystemComponent* InASC)
{
	ASC = InASC;
}

// ============================================================
// 帧更新
// ============================================================

void FGYGOStateManager::Update(float DeltaTime)
{
	if (!RuntimeData) return;

	// 遍历副本（状态内部可能调 RequestState 改变 ActiveStates）
	TArray<FCharacterState*> ActiveSnapshot;
	ActiveSnapshot.Append(ActiveStates.Array());

	for (FCharacterState* State : ActiveSnapshot)
	{
		if (State && State->IsActive())
		{
			State->Update(DeltaTime, *RuntimeData, *this);
		}
	}
}

// ============================================================
// 核心接口：RequestState / ReleaseState
// ============================================================

bool FGYGOStateManager::RequestState(ECharacterStateType NewStateType)
{
#if !UE_BUILD_SHIPPING
	FString NewStateName = UEnum::GetValueAsString(NewStateType);
	NewStateName.ReplaceInline(TEXT("ECharacterStateType::"), TEXT(""));
#endif

	// 已活跃 → 不重复激活
	if (IsInState(NewStateType))
	{
		return true;
	}

	// 在 AllStates 中查找目标状态
	TUniquePtr<FCharacterState>* Found = AllStates.Find(NewStateType);
	if (!Found || !*Found)
	{
		UE_LOG(LogTemp, Error, TEXT("[SM] %s NOT FOUND in AllStates!"), *NewStateName);
		return false;
	}

	// 检查关系矩阵
	FRelationCheckResult Result = CheckRelations(NewStateType);
	if (!Result.bAllowed)
	{
#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[SM] %s BLOCKED (ActiveCount=%d)"),
			*NewStateName, ActiveStates.Num());
#endif
		return false;
	}

	// 先中断所有需要中断的旧状态
	for (FCharacterState* ToInterrupt : Result.StatesToInterrupt)
	{
		DeactivateState(ToInterrupt);
	}

	// 激活新状态
	ActivateState(Found->Get());

#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[SM] %s ACTIVATED (Active=%d)"),
		*NewStateName, ActiveStates.Num());
#endif

	return true;
}

bool FGYGOStateManager::ReleaseState(ECharacterStateType StateType)
{
	TUniquePtr<FCharacterState>* Found = AllStates.Find(StateType);
	if (!Found || !*Found) return false;

	if (!(*Found)->IsActive()) return false;

	DeactivateState(Found->Get());

	// ★ 统一在操作完成后写入最终值，并同步动画数据。
	if (RuntimeData)
	{
		const ECharacterStateType FinalState =
			(ActiveStates.Num() > 0) ? CachedPrimaryState : ECharacterStateType::Idle;
		RuntimeData->CurrentState = FinalState;
		RuntimeData->AnimData.CurrentState = FinalState;
	}

	return true;
}

// ============================================================
// 查询接口
// ============================================================

bool FGYGOStateManager::IsInState(ECharacterStateType StateType) const
{
	for (const FCharacterState* State : ActiveStates)
	{
		if (State && State->GetStateType() == StateType)
			return true;
	}
	return false;
}

bool FGYGOStateManager::IsInGroup(EStateGroup Group) const
{
	for (const FCharacterState* State : ActiveStates)
	{
		if (State && State->GetStateGroup() == Group)
			return true;
	}
	return false;
}

void FGYGOStateManager::ForceSetPrimaryState(ECharacterStateType NewState)
{
	// 找到目标状态
	TUniquePtr<FCharacterState>* Found = AllStates.Find(NewState);
	if (!Found || !*Found) return;

	// 如果已经是 PrimaryState 且已活跃，跳过
	if ((*Found)->IsActive() && CachedPrimaryState == NewState) return;

	// 先停用当前 Locomotion 组的所有活跃状态
	TArray<FCharacterState*> ToDeactivate;
	for (FCharacterState* State : ActiveStates)
	{
		if (State && State->GetStateGroup() == EStateGroup::Locomotion)
		{
			ToDeactivate.Add(State);
		}
	}
	for (FCharacterState* State : ToDeactivate)
	{
		DeactivateState(State);
	}

	// 激活新主状态
	ActivateState(Found->Get());
	CachedPrimaryState = NewState;
}

// ============================================================
// 关系矩阵查询
// ============================================================

EStateRelationType FGYGOStateManager::GetRelation(
	ECharacterStateType From, ECharacterStateType To) const
{
	const EStateRelationType* Cached = RelationMatrixCache.Find(
		TPair<ECharacterStateType, ECharacterStateType>(From, To));
	if (Cached)
	{
		return *Cached;
	}
	// 未命中缓存 → 默认拒绝（安全策略）
	return EStateRelationType::Blocked;
}

bool FGYGOStateManager::IsActionAllowed(const FGameplayTag& ActionRestrictionTag) const
{
	if (!ASC) return true; // 无 ASC 时默认允许（降级兼容）

	// 有该限制 Tag → 动作被禁止
	return !ASC->HasMatchingGameplayTag(ActionRestrictionTag);
}

bool FGYGOStateManager::CanEnterState(ECharacterStateType NewState) const
{
	// 无活跃 PrimaryState 表示初始状态，任何状态都可进入
	if (CachedPrimaryState == ECharacterStateType::None)
	{
		return true;
	}

	EStateRelationType Rel = GetRelation(CachedPrimaryState, NewState);
	return Rel != EStateRelationType::Blocked;
}

// ============================================================
// 内部方法：状态注册
// ============================================================

template<typename TStateClass>
void FGYGOStateManager::RegisterState(ECharacterStateType Type)
{
	AllStates.Add(Type, MakeUnique<TStateClass>());
}

// ============================================================
// 内部方法：关系矩阵加载
// ============================================================

void FGYGOStateManager::LoadRelationMatrix(UDataTable* Table)
{
	RelationMatrixCache.Empty();

	if (Table)
	{
		// 从 DataTable 读取
		static const UEnum* StateEnum = StaticEnum<ECharacterStateType>();
		if (!StateEnum) return;

		for (int32 i = 0; i < static_cast<int32>(ECharacterStateType::MAX); ++i)
		{
			ECharacterStateType From = static_cast<ECharacterStateType>(i);
			if (From == ECharacterStateType::None || From == ECharacterStateType::MAX) continue;

			FName RowName = StateEnum->GetNameByValue(static_cast<int64>(From));
			FStateRelationRow* Row = Table->FindRow<FStateRelationRow>(RowName, TEXT("StateManager"));
			if (!Row) continue;

			for (int32 j = 0; j < static_cast<int32>(ECharacterStateType::MAX); ++j)
			{
				ECharacterStateType To = static_cast<ECharacterStateType>(j);
				if (To == ECharacterStateType::None || To == ECharacterStateType::MAX) continue;

				FName ColName = StateEnum->GetNameByValue(static_cast<int64>(To));
				if (FProperty* Prop = FStateRelationRow::StaticStruct()->FindPropertyByName(ColName))
				{
					void* ValueAddr = Prop->ContainerPtrToValuePtr<void*>(Row);
					if (ValueAddr)
					{
						EStateRelationType Rel = *static_cast<EStateRelationType*>(ValueAddr);
						RelationMatrixCache.Add(
							TPair<ECharacterStateType, ECharacterStateType>(From, To), Rel);
					}
				}
			}
		}
	}
	else
	{
		// DT 未配置 → 使用内置硬编码默认矩阵
		BuildDefaultRelationMatrix();
	}
}

void FGYGOStateManager::BuildDefaultRelationMatrix()
{
	using ST = ECharacterStateType;
	using RT = EStateRelationType;
	auto Add = [&](ST From, ST To, RT Rel)
	{
		RelationMatrixCache.Add(TPair<ST, ST>(From, To), Rel);
	};

	// ===== Idle =====
	Add(ST::Idle,       ST::Idle,     RT::Blocked);
	Add(ST::Idle,       ST::Moving,   RT::Interrupted);

	// ===== Moving =====
	Add(ST::Moving,     ST::Idle,     RT::Interrupted);
	Add(ST::Moving,     ST::Moving,   RT::Blocked);
}

// ============================================================
// 内部方法：关系检查
// ============================================================

FRelationCheckResult FGYGOStateManager::CheckRelations(
	ECharacterStateType NewStateType)
{
	FRelationCheckResult Result;
	Result.bAllowed = true;

	for (FCharacterState* OldState : ActiveStates)
	{
		if (!OldState) continue;

		// ★ 查询方向：(当前旧状态 → 新请求状态)
		//    矩阵语义："从 Old 能否切换到 New"
		ECharacterStateType OldType = OldState->GetStateType();
		EStateRelationType Relation = GetRelation(OldType, NewStateType);

		switch (Relation)
		{
		case EStateRelationType::Independent:
			break;

		case EStateRelationType::Interrupted:
			Result.StatesToInterrupt.Add(OldState);
			break;

		case EStateRelationType::Blocked:
			Result.bAllowed = false;
			return Result;

		default:
			break;
		}
	}

	return Result;
}

// ============================================================
// 内部方法：激活 / 停用状态
// ============================================================

void FGYGOStateManager::ActivateState(FCharacterState* State)
{
	if (!State || State->IsActive()) return;
	if (!RuntimeData) return;

	// 1. 调用状态 Enter
	State->Enter(*RuntimeData);

	// 2. ★ GAS 联动：施加状态进入 GE（Phase 7）
	ApplyEnterGameplayEffects(State);

	// 3. ★ GAS 联动：按 Tag 移除冲突 GE（Phase 7）
	RemoveGameplayEffectsByTag(State);

	// 4. 功能限制位掩码（Phase 6~7 过渡方案，GE 的 OwnedTag 驱动后可移除）
	ApplyLimitFlags(State->GetLimitFlags());

	// 5. 加入活跃集合 + 标记激活
	ActiveStates.Add(State);
	State->bIsActive = true;

	// 6. 更新 PrimaryState（必须在写 CurrentState 之前）
	UpdatePrimaryState();

	// 7. 状态机同时写逻辑状态和动画状态，保持同源、同帧。
	RuntimeData->CurrentState = CachedPrimaryState;
	RuntimeData->AnimData.CurrentState = CachedPrimaryState;
}

void FGYGOStateManager::DeactivateState(FCharacterState* State)
{
	if (!State || !State->IsActive()) return;
	if (!RuntimeData) return;

	// 1. 调用状态 Exit
	State->Exit(*RuntimeData);

	// 2. ★ GAS 联动：移除此状态施加的 GE（Phase 7）
	//    通过查找 ActiveGameplayEffects 中由本状态施加的 GE 并移除
	//    （当前简化实现：依赖 GE 的 DurationPolicy = Infinite + RemoveGEsWithTag）
	//    完整方案：在 ApplyEnterGEs 时记录 FActiveGameplayEffectHandle，退出时按 Handle 移除

	// 3. 从活跃集合移除 + 清除标记
	ActiveStates.Remove(State);
	State->bIsActive = false;

	// 4. 更新 PrimaryState（缓存层）
	UpdatePrimaryState();

	// ★ 注意：不在这里更新 RuntimeData->CurrentState
	// CurrentState 统一由 RequestState/ReleaseState 在操作完成后写入，
	// 避免 Deactivate→Activate 中间出现"空档期"的回退值
}

// ============================================================
// 内部方法：PrimaryState 更新
// ============================================================

void FGYGOStateManager::UpdatePrimaryState()
{
	for (FCharacterState* State : ActiveStates)
	{
		if (State && State->GetStateGroup() == EStateGroup::Locomotion)
		{
			CachedPrimaryState = State->GetStateType();
			return;
		}
	}
	CachedPrimaryState = ECharacterStateType::Idle;
}

// ============================================================
// 内部方法：功能限制位掩码
// ============================================================

void FGYGOStateManager::ApplyLimitFlags(int32 LimitFlags)
{
	if (!RuntimeData) return;

	RuntimeData->bBlockMove    = !!(LimitFlags & (1 << 0));
	RuntimeData->bBlockAttack  = !!(LimitFlags & (1 << 1));
	RuntimeData->bBlockDodge   = !!(LimitFlags & (1 << 2));
}

// ============================================================
// ★ GAS 联动方法（Phase 7）
// ============================================================

void FGYGOStateManager::ApplyEnterGameplayEffects(FCharacterState* State)
{
	if (!ASC || !State) return;

	TArray<TSubclassOf<UGameplayEffect>> GEs = State->GetEnterGameplayEffects();
	if (GEs.Num() == 0) return; // 大多数状态没有 GE，快速跳过

	// 创建 GE 上下文（来源 = 角色自身）
	FGameplayEffectContextHandle Context = ASC->MakeEffectContext();

	for (const TSubclassOf<UGameplayEffect>& GEClass : GEs)
	{
		if (!GEClass) continue;

		FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(GEClass, 1, Context);
		if (!SpecHandle.IsValid()) continue;

#if !UE_BUILD_SHIPPING
		{
			FString GEName = GEClass->GetName();
			GEName.ReplaceInline(TEXT("_C"), TEXT(""));
			FString StateName = State->GetStateName().ToString();
			UE_LOG(LogTemp, Log, TEXT("[SM:GAS] %s → ApplyGE(%s)"),
				*StateName, *GEName);
		}
#endif

		ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
	}
}

void FGYGOStateManager::RemoveGameplayEffectsByTag(FCharacterState* State)
{
	if (!ASC || !State) return;

	FGameplayTagContainer TagsToRemove = State->GetRemoveGEsWithTag();
	if (TagsToRemove.Num() == 0) return;

	// 通过 FGameplayEffectQuery 按 OwnedTags 匹配，批量移除
	FGameplayEffectQuery Query;
	FGameplayTagQueryExpression TagExpression;
	TagExpression.AnyTagsMatch();
	TagExpression.AddTags(TagsToRemove);
	Query.OwningTagQuery = FGameplayTagQuery::BuildQuery(TagExpression);
	ASC->RemoveActiveEffects(Query, -1);  // -1 = 移除全部匹配项

#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[SM:GAS] Removed GEs by Tag match (NumTags=%d)"),
		TagsToRemove.Num());
#endif
}
