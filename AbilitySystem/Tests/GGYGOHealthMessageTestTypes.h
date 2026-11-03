#pragma once

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Actor.h"

#include "GGYGOHealthMessageTestTypes.generated.h"

/** Test-only actor that exposes a real project ASC through GAS's source interface. */
UCLASS(Transient)
class AGGYGOHealthMessageTestActor : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOHealthMessageTestActor();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UGGYGOAbilitySystemComponent* GetProjectAbilitySystemComponent() const { return AbilitySystemComponent; }
	void SetTestNetRole(ENetRole InRole) { SetRole(InRole); AbilitySystemComponent->CacheIsNetSimulated(); }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;
};

/** Exposes the existing protected RepNotify entry points without changing production interfaces. */
UCLASS(Transient)
class UGGYGOHealthMessageRepNotifyTestSet : public UGGYGOHealthSet
{
	GENERATED_BODY()

public:
	virtual UWorld* GetWorld() const override;
	void SetMessageWorldOverride(UWorld* World);
	void ClearMessageWorldOverride();
	void BeginTestNetReceive() { PreNetReceive(); }
	void EndTestNetReceive() { PostNetReceive(); }

	void SimulateReplicatedMaxHealth(float NewValue)
	{
		const FGameplayAttributeData OldValue = *GetMaxHealthAttribute().GetGameplayAttributeDataChecked(this);
		*GetMaxHealthAttribute().GetGameplayAttributeDataChecked(this) = FGameplayAttributeData(NewValue);
		OnRep_MaxHealth(OldValue);
	}

	void SimulateReplicatedMaxPoise(float NewValue)
	{
		const FGameplayAttributeData OldValue = *GetMaxPoiseAttribute().GetGameplayAttributeDataChecked(this);
		*GetMaxPoiseAttribute().GetGameplayAttributeDataChecked(this) = FGameplayAttributeData(NewValue);
		OnRep_MaxPoise(OldValue);
	}

	void SimulateReplicatedHealth(float NewValue)
	{
		FGameplayAttributeData NewAttributeValue(NewValue);
		SimulateReplicatedHealth(NewAttributeValue);
	}

	void SimulateReplicatedHealth(const FGameplayAttributeData& NewValue)
	{
		const FGameplayAttributeData OldValue = *GetHealthAttribute().GetGameplayAttributeDataChecked(this);
		*GetHealthAttribute().GetGameplayAttributeDataChecked(this) = NewValue;
		OnRep_Health(OldValue);
	}

	void SimulateReplicatedPoise(float NewValue)
	{
		FGameplayAttributeData NewAttributeValue(NewValue);
		SimulateReplicatedPoise(NewAttributeValue);
	}

	void SimulateReplicatedPoise(const FGameplayAttributeData& NewValue)
	{
		const FGameplayAttributeData OldValue = *GetPoiseAttribute().GetGameplayAttributeDataChecked(this);
		*GetPoiseAttribute().GetGameplayAttributeDataChecked(this) = NewValue;
		OnRep_Poise(OldValue);
	}

	/** 按原生复制写入整份 Base/Current 后调用对应 OnRep。 */
	void SimulateReplicatedAttribute(const FGameplayAttribute& Attribute, const FGameplayAttributeData& NewValue)
	{
		FGameplayAttributeData* Data = Attribute.GetGameplayAttributeDataChecked(this);
		const FGameplayAttributeData OldValue = *Data;
		*Data = NewValue;
		if (Attribute == GetHealthAttribute()) { OnRep_Health(OldValue); }
		else if (Attribute == GetMaxHealthAttribute()) { OnRep_MaxHealth(OldValue); }
		else if (Attribute == GetPoiseAttribute()) { OnRep_Poise(OldValue); }
		else if (Attribute == GetMaxPoiseAttribute()) { OnRep_MaxPoise(OldValue); }
	}

private:
	bool bOverrideMessageWorld = false;
	TWeakObjectPtr<UWorld> MessageWorldOverride;
};

/**
 * 复现对照用：Lyra 的 HealthSet 结算方式，仅存在于测试。
 *
 * 服务器在 PostGameplayEffectExecute 中按 GE 精确幅度广播；客户端 OnRep 以 Old／New 广播、
 * Magnitude 取 New-Old；MaxHealth 下降时权威端压低 Health。为保持修复清单决定 #2，
 * Current 写入同样经 GGYGOAttributeBase 反算，Base 不做可见范围 Clamp。
 * 不使用父类的 Modifier／RepNotify／NetReceive 分类帧。
 */
UCLASS(Transient)
class UGGYGOHealthReproLyraSet : public UGGYGOHealthSet
{
	GENERATED_BODY()

public:
	void BeginTestNetReceive() { PreNetReceive(); }
	void EndTestNetReceive() { PostNetReceive(); }
	/** Lyra 的 OnRep：先走原生复制入口，再按 Old／New 广播。 */
	void SimulateReplicatedAttribute(const FGameplayAttribute& Attribute, const FGameplayAttributeData& NewValue);

protected:
	virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	virtual void OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const override;
	virtual void PreNetReceive() override;
	virtual void PostNetReceive() override;

private:
	void WriteCurrent(const FGameplayAttribute& Attribute, float DesiredCurrent);
	void Publish(const FGameplayTag& Verb, const FGameplayEffectSpec& Spec, float Magnitude);
	float HealthBefore = 0.0f;
	float MaxHealthBefore = 0.0f;
	float PoiseBefore = 0.0f;
	bool bLyraOutOfHealth = false;
	bool bLyraPoiseBroken = false;
};
