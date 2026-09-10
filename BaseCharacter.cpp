/**
 * @file BaseCharacter.cpp
 * @brief 旧角色基类过渡壳实现
 */
#include "BaseCharacter.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BaseCharacter)

ABaseCharacter::ABaseCharacter()
{
	// 不做任何事。组件与 Tick 设置全部由 AGGYGOCharacterBase 的构造函数完成。
	//
	// 旧实现在这里 CreateDefaultSubobject 了三个对象（引擎原生 ASC、旧单体
	// AttributeSet、RuntimeComponent）。前两个已被父类的项目 ASC 取代，
	// 第三个随 Pipeline 退役。都不能保留 —— 两个 ASC 同时存在会让
	// IAbilitySystemInterface 的返回值与实际接收 GE 的对象不是同一个。
}

float ABaseCharacter::GetCurrentSpeed() const
{
	return GetGGYGOMovementComponent()->GetHorizontalSpeed();
}

float ABaseCharacter::GetMoveAngle() const
{
	return GetGGYGOMovementComponent()->GetLocalVelocityAngle();
}

bool ABaseCharacter::IsMoving() const
{
	return GetGGYGOMovementComponent()->IsMovingHorizontally();
}

bool ABaseCharacter::IsGrounded() const
{
	return GetGGYGOMovementComponent()->IsMovingOnGround();
}

EGGYGOGait ABaseCharacter::GetResolvedGait() const
{
	return GetGGYGOMovementComponent()->GetResolvedGait();
}

void ABaseCharacter::NotifyCanYaw()
{
	// 阶段 6 在这里转发给 CMC 的 TurnBack 相位机。见头文件说明。
}
