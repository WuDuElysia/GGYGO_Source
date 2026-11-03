#pragma once

#include "CoreMinimal.h"
#include "Character/Data/GGYGOMovementTypes.h"

class ACharacter;
class USceneComponent;
class UGGYGOCharacterMovementComponent;

/** Qualification belongs to Movement; consumers only recover their own presentation. */
enum class EGGYGOLocomotionSteeringStatus : uint8
{
	/** Original configuration initialization is in flight, or accepted dependencies await the first interval. Never movement admission. */
	Initial,
	Valid,
	NotApplicable,
	Invalid
};

/** GT capture of one completed native movement interval. No direction or gait authority. */
struct GGYGO_API FGGYGOLocomotionSteeringSnapshot
{
	EGGYGOLocomotionSteeringStatus Status = EGGYGOLocomotionSteeringStatus::Initial;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement;
	TWeakObjectPtr<ACharacter> OriginalCharacter;
	TWeakObjectPtr<USceneComponent> OriginalUpdatedComponent;
	uint64 SourceEpoch = 0;
	uint64 CompletedIntervalSerial = 0;
	uint64 CompletedFrame = 0;
	float NativeDeltaSeconds = 0.0f;
	/** Actual capsule yaw delta / native dt, degrees/s, UE right positive. */
	float ActualSignedYawRate = 0.0f;
	/** Actual OldVelocity -> completed Velocity heading delta / native dt. */
	float ActualSignedVelocityYawRate = 0.0f;
	/** Both original interval endpoint horizontal velocities were finite and nonzero. */
	bool bHasVelocityYawRate = false;
	/** Admitted input heading minus capsule heading, signed degrees. */
	float DesiredDirectionError = 0.0f;
	/** 镜头来源独立资格；Valid 的 0 表示没有镜头转向请求，不影响普通方向键转向。 */
	EGGYGOLocomotionSteeringStatus CameraTurnStatus = EGGYGOLocomotionSteeringStatus::Initial;
	/** 原输入实际投影基底的 Yaw 变化，不是完成时再读 PC 得到的另一份角度。 */
	float CameraControlYawDeltaDegrees = 0.0f;
	/** 镜头请求中尚未被真实完成转角抵扣的部分；只供表现，不驱动胶囊。 */
	float CameraTurnDemandDegrees = 0.0f;
	/** 远端只认原 RemoteMotionPoseRecord 与 native 呈现配对，不冒充本地 completed 区间。 */
	bool bCameraTurnRemote = false;
	uint32 CameraTurnPublisherEpoch = 0;
	uint32 CameraTurnPublicationSerial = 0;
	float CameraTurnNativeSampleKey = 0.0f;
	uint64 CameraTurnAcceptedLocalFrame = 0;
	/** camera 事实所属权威区间的真实步态/alpha；不代替 CMC 步态权威或新增混合时钟。 */
	EGGYGOGait CameraTurnGait = EGGYGOGait::None;
	float CameraTurnWalkRunBlendAlpha = 0.0f;
	FString CameraTurnDiagnostic;
	FString Diagnostic;
};
