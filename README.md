# GGYGO Source 代码手册与运行时架构

> 本文是 `Source/GGYGO/**` 的源码级索引和运行时架构说明。内容以当前工作区源码为准，不以文件名、旧 NTE 逆向资料或未落地设计作为实现依据。
>
> 本文只保留稳定的项目架构、文件/目录职责、class/struct/字段/函数说明、数据权威、运行时顺序和当前源码已确认的未接线边界。

## 0. 阅读约定

- `Public/**`：模块公开头文件、UCLASS/USTRUCT、纯 C++ 接口和数据模型。
- `Private/**`：对应实现文件、运行时执行逻辑和日志定义。
- `UCLASS`、`USTRUCT`、`UBlueprintFunctionLibrary`、`UAnimInstance`：Unreal 反射/生命周期对象。
- 以 `F` 开头但不是 UObject 的类型：当前通常是纯 C++ 数据、接口、Pipeline、Processor、State 或 Driver。
- “当前没有实现”表示源码中确实没有可执行逻辑，不能理解为后续计划已经完成。
- “兼容/诊断”表示函数或字段仍存在并可能被调用，但不是当前主逻辑的唯一权威来源。
- 本文只搜索和描述 `Source/GGYGO/**`；`.kiro/specs`、AAADocs 和旧 NTE 资料不作为当前代码实现证据。

## 1. 项目当前定位

GGYGO 当前采用 **Unreal 对象负责生命周期与引擎边界，纯 C++ Pipeline 负责显式执行时序** 的混合架构。它不是严格 ECS，也不是“每个功能都各自 Tick 的组件集合”。

核心原则：

1. `ABaseCharacter` 是 Actor/GAS 宿主，也是唯一的角色帧调度入口。
2. `UGGYGOCharacterRuntimeComponent` 是单一运行时宿主组件，负责创建和销毁纯 C++ 子系统，但不启用自己的 `TickComponent`。
3. `FInputData`、`FRuntimeData`、各 Pipeline、Processor、State、`FGYGOStateManager` 和 `FMotionDriver` 不升级为 UObject。
4. `UAbilitySystemComponent`、`UGGYGOAttributeSet`、`UCharConfigData`、`UZZZAnimInstance` 保留 Unreal 对象身份。
5. `FRuntimeData` 是逻辑侧运行时 Model 聚合入口；`FRuntimeData::ZZZAnim` 是写给新版 ZZZ 动画层的游戏线程投影。
6. ZZZ 动画层通过 Snapshot 消费逻辑结果；AnimBP/AnimInstance 不反向成为逻辑字段的权威写入者。

## 2. 完整源码文件索引

下面的清单按当前工作区实际文件排列。目录存在但没有文件的区域也明确列出，防止 README 把未实现模块误认为已经存在。

### 2.1 模块根目录

| 路径 | 类型 | 作用 |
|---|---|---|
| `GGYGO.Build.cs` | ModuleRules | 声明模块依赖和 Public include path。 |
| `GGYGO.h` | 头文件 | 只包含 `CoreMinimal.h`，当前没有类型或函数。 |
| `GGYGO.cpp` | cpp | 使用 `IMPLEMENT_PRIMARY_GAME_MODULE` 注册主游戏模块。 |
| `README.md` | 文档 | 本源码手册和运行时架构说明。 |
| `.gitignore` | Git 配置 | 模块仓库的忽略规则，不参与运行时。 |

### 2.2 Public 文件

```text
Public/
├─ BaseCharacter.h
├─ PlayerCharacter.h
├─ GGYGOGameplayEffects.h
├─ GGYGOPlayerController.h
├─ GGYGOTags.h
├─ SyncMarkerImporter.h
├─ Animation/
│  ├─ AnimSyncMarkerTools.h
│  └─ zzzAnim/
│     ├─ ZZZAnimInstance.h
│     ├─ ZZZAnimLog.h
│     ├─ Capture/ZZZAnimSnapshotCapture.h
│     ├─ Data/
│     │  ├─ ZZZAnimContext.h
│     │  ├─ ZZZAnimEnums.h
│     │  ├─ ZZZAnimSet.h
│     │  ├─ ZZZAnimSnapshot.h
│     │  ├─ ZZZAnimStateMemory.h
│     │  └─ ZZZAnimTuning.h
│     └─ Locomotion/
│        ├─ ZZZLocomotionDecisions.h
│        ├─ ZZZLocomotionEvents.h
│        └─ ZZZLocomotionRules.h
├─ Attributes/GGYGOAttributeSet.h
├─ Components/GGYGOCharacterRuntimeComponent.h
├─ Contracts/
│  ├─ Animation/
│  │  └─ ZZZAnimRuntimeModel.h
│  ├─ Pipeline/CharacterFrameCommands.h
│  └─ State/StateUpdateResult.h
├─ Data/
│  ├─ Config/UCharConfigData.h
│  ├─ Input/InputData.h
│  └─ Runtime/
│     ├─ ArbiterRuntimeModel.h
│     ├─ GaitRuntimeModel.h
│     ├─ IntentRuntimeModel.h
│     ├─ MovementRuntimeModel.h
│     ├─ RootMotionRuntimeModel.h
│     ├─ RuntimeData.h
│     ├─ StateRuntimeModel.h
│     └─ ViewRuntimeModel.h
├─ Drivers/MotionDriver.h
├─ Movement/MovementConfig.h
├─ Pipeline/
│  ├─ CharacterControlPipeline.h
│  ├─ ArbiterPipeline.h
│  ├─ InputPipeline.h
│  ├─ IntentPipeline.h
│  ├─ Arbiters/
│  │  ├─ ActionArbiter.h
│  │  ├─ GASArbiter.h
│  │  ├─ HealthArbiter.h
│  │  └─ StaminaArbiter.h
│  ├─ Gait/
│  │  ├─ GaitAuthorityProcessor.h
│  │  └─ GaitLog.h
│  ├─ Intents/
│  │  ├─ AttackIntentProcessor.h
│  │  ├─ DodgeIntentProcessor.h
│  │  ├─ LocomotionIntentProcessor.h
│  │  └─ ViewRotationProcessor.h
│  ├─ Interfaces/
│  │  ├─ IArbiter.h
│  │  ├─ IIntentProcessor.h
│  │  └─ IParameterProcessor.h
│  └─ Parameters/
│     ├─ MovementParameterProcessor.h
│     ├─ RootMotionParameterProcessor.h
│     └─ TurnBackPhaseProcessor.h
└─ StateMachine/
   ├─ CharacterState.h
   ├─ CharacterStateType.h
   ├─ GGYGOStateManager.h
   ├─ Data/FStateRelationRow.h
   └─ State/
      ├─ IdleState.h
      └─ MovingState.h
```

### 2.3 Private 文件

```text
Private/
├─ BaseCharacter.cpp
├─ PlayerCharacter.cpp
├─ GGYGOGameplayEffects.cpp
├─ GGYGOPlayerController.cpp
├─ SyncMarkerImporter.cpp
├─ Attributes/GGYGOAttributeSet.cpp
├─ Components/GGYGOCharacterRuntimeComponent.cpp
├─ Data/Config/UCharConfigData.cpp
├─ Drivers/MotionDriver.cpp
├─ Animation/
│  ├─ AnimSyncMarkerTools.cpp
│  └─ zzzAnim/
│     ├─ ZZZAnimInstance.cpp
│     ├─ ZZZAnimLog.cpp
│     ├─ Capture/ZZZAnimSnapshotCapture.cpp
│     └─ Locomotion/
│        ├─ ZZZLocomotionDecisions.cpp
│        ├─ ZZZLocomotionEvents.cpp
│        └─ ZZZLocomotionRules.cpp
├─ Pipeline/
│  ├─ CharacterControlPipeline.cpp
│  ├─ ArbiterPipeline.cpp
│  ├─ InputPipeline.cpp
│  ├─ IntentPipeline.cpp
│  ├─ Arbiters/
│  │  ├─ ActionArbiter.cpp
│  │  ├─ GASArbiter.cpp
│  │  ├─ HealthArbiter.cpp
│  │  └─ StaminaArbiter.cpp
│  ├─ Gait/
│  │  ├─ GaitAuthorityProcessor.cpp
│  │  └─ GaitLog.cpp
│  ├─ Intents/
│  │  ├─ AttackIntentProcessor.cpp
│  │  ├─ DodgeIntentProcessor.cpp
│  │  ├─ LocomotionIntentProcessor.cpp
│  │  └─ ViewRotationProcessor.cpp
│  └─ Parameters/
│     ├─ MovementParameterProcessor.cpp
│     ├─ RootMotionParameterProcessor.cpp
│     └─ TurnBackPhaseProcessor.cpp
└─ StateMachine/
   ├─ CharacterState.cpp
   ├─ GGYGOStateManager.cpp
   └─ State/
      ├─ IdleState.cpp
      └─ MovingState.cpp
```

当前没有 C++ GameplayAbility 类、独立 Movement 类或 Private Tests；这些能力尚未实现，对应的空脚手架目录已从源码树移除，需要时再新建。`Data/Runtime`、`Contracts`、多数 ZZZ 动画内部数据文件、枚举和接口是 header-only，这是设计上的纯数据/接口形式，不是漏掉的 `.cpp`。

## 3. 运行时对象拓扑和所有权

```text
ABaseCharacter (ACharacter + IAbilitySystemInterface)
├─ ASC : UAbilitySystemComponent                    Unreal/GAS 对象
├─ AttributeSet : UGGYGOAttributeSet                Unreal/GAS 对象
├─ RuntimeComponent : UGGYGOCharacterRuntimeComponent Unreal 生命周期薄壳
│  └─ TUniquePtr<FCharacterControlPipeline>
│     ├─ TUniquePtr<FInputData>
│     ├─ TUniquePtr<FRuntimeData>
│     ├─ TUniquePtr<FInputPipeline>
│     ├─ TUniquePtr<FIntentPipeline>
│     ├─ TUniquePtr<FArbiterPipeline>
│     ├─ TUniquePtr<FGYGOStateManager>
│     └─ TUniquePtr<FMotionDriver>
└─ Mesh : USkeletalMeshComponent                 Unreal 引擎组件
   └─ AnimInstance : UZZZAnimInstance             引擎/AnimBP 拥有
```

### 3.1 Unreal 对象边界

- `ABaseCharacter`：角色 Actor、GAS 初始化、唯一角色 Tick 和输入门面。
- `APlayerCharacter`：Enhanced Input 绑定、玩家摄像机角度配置。
- `UGGYGOCharacterRuntimeComponent`：不独立 Tick 的 Unreal 生命周期薄壳，只持有并转发给 `FCharacterControlPipeline`。
- `UAbilitySystemComponent`：Ability、GameplayEffect、GameplayTag 和 ASC 查询。
- `UGGYGOAttributeSet`：GAS 属性及 GameplayEffect 执行后的属性处理。
- `UCharConfigData`：角色配置 DataAsset。
- `UZZZAnimInstance`：引擎拥有的 AnimInstance、快照消费和 AnimBP 查询入口。
- `UAnimSequence`、`UBlendSpace`、`USkeletalMeshComponent`、`UCharacterMovementComponent`：Unreal 动画/移动对象。

### 3.2 纯 C++ 边界

`FCharacterControlPipeline` 由 Runtime Component 通过 `TUniquePtr` 拥有，内部再拥有所有纯 C++ 数据和阶段对象。以下类型没有 GC、反射、复制或 Blueprint 生命周期需求：

- 控制根：`FCharacterControlPipeline`、`FCharacterFramePlan`、`FCharacterFrameCommandBuffer`、`FCharacterMovementCommand`、`FCharacterAnimationPublishCommand`。
- 输入：`FInputData`、`FInputPipeline`。
- 黑板/Model：`FRuntimeData`（`Data/Runtime/RuntimeData.h`）及 `Data/Runtime/*.h`；跨层投影和帧级提交契约位于 `Contracts/`。
- 仲裁：`FArbiterPipeline`、`IArbiter`、四个 Arbiter。
- 意图和参数：`FIntentPipeline`、`IIntentProcessor`、`IParameterProcessor`、各 Processor。
- 步态：`FGaitAuthorityProcessor`。
- 状态：`FGYGOStateManager`、`FStateUpdateResult`、`FStateTransitionEvent`、`FCharacterState`、`FIdleState`、`FMovingState`。
- 移动：`FMotionDriver`，消费 `FCharacterMovementCommand` 并回写 RuntimeData。
- 动画逻辑辅助：`FZZZAnimSnapshotCapture`、`FZZZLocomotionDecisions`、`FZZZLocomotionEvents`、ZZZ Data/Context/Model/Memory/Rules。

## 4. 初始化和唯一帧时序

### 4.1 BeginPlay 初始化

`ABaseCharacter::BeginPlay()` 的实际顺序：

1. `Super::BeginPlay()`。
2. `InitGEGlobals()`：加载全局 GameplayEffect 软引用。
3. `ASC->InitAbilityActorInfo(this, this)`。
4. `RuntimeComponent->InitializeRuntime(this, ASC, GetMesh())`。
5. Runtime Component 将依赖转发给 `FCharacterControlPipeline::Initialize`；Pipeline 内部按顺序执行：
   - `FArbiterPipeline::Init`（只有 ASC 非空时注册仲裁器）；
   - `FIntentPipeline::Init`；
   - `FMotionDriver::Init`；
   - `FGYGOStateManager::Init(*RuntimeData)`；
   - `FGYGOStateManager::InitASC(ASC)`。
6. `FCharacterControlPipeline` 用 `bInitialized` 防止重复初始化；Component 自身不保存第二套初始化状态。

`FGYGOStateManager::Init` 必须早于 `InitASC`；当前没有传入关系 DataTable，因此使用内置 Idle/Moving 关系矩阵。

### 4.2 Tick 阶段

`ABaseCharacter::Tick` 仍是唯一入口，只调用 `RuntimeComponent->ProcessFrame(DeltaTime)`；Component 再转发给 `FCharacterControlPipeline::ProcessFrame`。Pipeline 内部固定执行：

| 顺序 | 显式阶段 | 主要函数 | 主要读写 |
|---:|---|---|---|
| 1 | Capture | `CaptureInputAndConstraints` | GAS/状态 → `RuntimeData.Arbiter`；Pending input → `FInputData` |
| 2 | Intent | `BuildIntent` | `FInputData` → `RuntimeData.Intent`/`View`/`ZZZAnim.bShouldMove` |
| 3 | Decision | `ResolveDecision` | Intent/约束 → Gait/Parameters/State；StateManager 输出状态结果；生成 `FCharacterFramePlan` 和命令缓冲 |
| 4 | Motion Commit | `CommitMovement` | Pipeline 消费 `FCharacterMovementCommand`，提交 Actor 位移并回写速度投影 |
| 5 | Animation Publish | `PublishAnimation` | Pipeline 消费动画发布命令，调用已有 `UZZZAnimInstance::PipelineDrive` |
| 6 | Reset | `ResetFrame` | 只清理帧级 Attack/Dodge 意图，保留最近一次计划快照 |

阶段内部顺序保持可见：Capture 先执行 `FArbiterPipeline::Process`、再执行 `FInputPipeline::Process`；Decision 依次执行 `ProcessGait`、`ProcessParameters`、`FGYGOStateManager::Update(DeltaTime, OutResult)`、`BuildFramePlan`。StateManager 仍在自身内部决定和完成状态转换，并把 `FStateUpdateResult` 告知 Pipeline；Pipeline 不重复执行状态操作，而是在后续 Commit/Publish 阶段消费 `FCharacterFrameCommandBuffer`。`FCharacterFramePlan` 同时记录本帧意图/仲裁/状态结果和真实存在的移动、动画发布命令。当前项目没有 C++ GameplayAbility 激活链，因此计划只记录 `ActionGranted`，不生成 GA 命令。

禁止给 Pipeline、Processor 或 State 增加第二个独立 Tick；禁止在 AnimBP 中反向写逻辑权威字段；禁止把每个纯 C++ 类型仅因为功能独立就升级为 UObject。

## 5. 根级模块、角色和 GAS 文件

### 5.1 `GGYGO.Build.cs`

**文件职责**：定义 `GGYGO : ModuleRules` 的编译依赖和公开头文件搜索路径。

**函数**：

- `GGYGO(ReadOnlyTargetRules Target)`：设置显式/共享 PCH；添加 Public 依赖 `Core`、`CoreUObject`、`Engine`、`InputCore`、`EnhancedInput`、`GameplayAbilities`、`GameplayTags`、`GameplayTasks`；添加 Private 依赖 `Json`；注册 `Public`、`Public/Attributes`、`Public/Movement`、`Public/StateMachine` 四个实际存在的 include paths；当目标是 Editor 时增加 `AutomationController`。没有运行时初始化逻辑。

### 5.2 `GGYGO.h` / `GGYGO.cpp`

- `Public/GGYGO.h`：只包含 `CoreMinimal.h`，当前没有 class、struct、函数或全局状态。
- `GGYGO.cpp`：通过 `IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, GGYGO, "GGYGO")` 注册主游戏模块；没有自定义函数。

### 5.3 `Public/BaseCharacter.h` / `Private/BaseCharacter.cpp`

**`ABaseCharacter`**：`ACharacter` + `IAbilitySystemInterface`。负责 GAS 对象、运行时宿主组件、输入门面、动画 getter 和唯一 Tick；不直接拥有纯 C++ 阶段对象。

**字段**：

- `ASC`：Actor 子对象形式的 `UAbilitySystemComponent`。
- `AttributeSet`：Actor 子对象形式的 `UGGYGOAttributeSet`。
- `DefaultAbilities`、`DefaultEffects`：蓝图可配置的 Ability/Effect 数组；当前 BeginPlay 不自动调用对应应用函数。
- `CharacterConfig`：`UCharConfigData` 资产引用。
- `RuntimeComponent`：`UGGYGOCharacterRuntimeComponent`，只托管单一 `FCharacterControlPipeline`。

**函数**：

- `ABaseCharacter()`：开启 `PrimaryActorTick`；创建 ASC、AttributeSet 和 Runtime Component。
- `GetAbilitySystemComponent()`：返回 `ASC`，供 `IAbilitySystemInterface` 和 GAS 使用。
- `GetAttributeSet()`：返回 `AttributeSet` 非拥有指针。
- `GetCurrentSpeed()`：读取 `RuntimeData.Movement.CurrentSpeed`。
- `GetMoveAngle()`：读取 `RuntimeData.Movement.MoveAngle`。
- `GetCurrentState()`：读取 `RuntimeData.State.CurrentState`。
- `IsMoving()`：读取 `RuntimeData.Movement.bIsMoving`。
- `IsGrounded()`：读取 `RuntimeData.Movement.bIsGrounded`。
- `GetAnimSpeed()`：读取 `RuntimeData.RootMotion.AnimSpeed`。
- `GetResolvedGait()`：读取 `RuntimeData.Gait.ResolvedGait`。
- `GetCharacterConfig()`：返回角色配置资产。
- `GetRuntimeData()`：返回控制 Pipeline 内部的 `FRuntimeData` 非拥有指针；组件为空时返回空。
- `GiveDefaultAbilities()`：遍历有效 `DefaultAbilities`，以 Level 1、`INDEX_NONE` InputID、当前角色为 SourceObject 调用 `ASC->GiveAbility`。
- `ApplyDefaultEffects()`：遍历有效 `DefaultEffects`，创建带自身 SourceObject 的 GameplayEffect Context 和 Spec，并调用 `ApplyGameplayEffectSpecToSelf`。
- `BeginPlay()`：调用父类、初始化 GE 全局引用、初始化 ASC ActorInfo，再调用 Runtime Component 初始化。
- `Tick(float DeltaTime)`：调用父类，然后唯一调用 `RuntimeComponent->ProcessFrame(DeltaTime)`。
- `SetMoveInput(const FVector2D&)`：把移动输入转发到组件。
- `ClearMoveInput()`：把移动输入清零请求转发到组件。
- `SetLookInput(const FVector2D&)`：把视角输入转发到组件。
- `SetSprintHeld(bool)`：把冲刺持续状态转发到组件。
- `SetForceWalkHeld(bool)`：把强制步行持续状态转发到组件。
- `SetupPlayerInputComponent(UInputComponent*)`：当前只调用父类，不在 Base 层绑定具体 Input Action。

### 5.4 `Public/PlayerCharacter.h` / `Private/PlayerCharacter.cpp`

**`APlayerCharacter`**：玩家角色输入和摄像机配置层，不直接访问纯 C++ Pipeline。

**字段**：`PitchMin`、`PitchMax`、`IA_Move`、`IA_Look`、`IA_Sprint`、`IA_ForceWalk`。

**函数**：

- `APlayerCharacter()`：当前为空构造函数。
- `BeginPlay()`：先调用 BaseCharacter；从 Controller 获取 `APlayerController`，存在 CameraManager 时写入俯仰角上下限。
- `SetupPlayerInputComponent(UInputComponent*)`：调用父类并转换成 `UEnhancedInputComponent`；在非 Shipping 下显示输入资产诊断；按 Triggered/Completed/Started 绑定移动、视角、冲刺和强制步行。
- `OnMoveInput(const FInputActionValue&)`：读取 `FVector2D`，调用 Base 的 `SetMoveInput`；非 Shipping 下显示输入值。
- `OnMoveCompleted(const FInputActionValue&)`：调用 Base 的 `ClearMoveInput`。
- `OnLookInput(const FInputActionValue&)`：读取 `FVector2D` 并调用 `SetLookInput`。
- `OnSprintInput(const FInputActionValue&)`：读取 bool 并调用 `SetSprintHeld`。
- `OnForceWalkInput(const FInputActionValue&)`：读取 bool 并调用 `SetForceWalkHeld`。

当前没有攻击/闪避 Input Action 回调；虽然 `FInputPipeline` 提供对应写入函数，但当前玩家类没有调用它们。

### 5.5 `Public/Components/GGYGOCharacterRuntimeComponent.h` / `Private/Components/GGYGOCharacterRuntimeComponent.cpp`

**`UGGYGOCharacterRuntimeComponent`**：角色控制 Pipeline 的 Unreal 生命周期薄壳。它自身 `PrimaryComponentTick.bCanEverTick = false`，只持有一个 `TUniquePtr<FCharacterControlPipeline>`，不直接拥有各阶段对象。

**字段**：`ControlPipeline` 是唯一纯 C++ 控制根；Owner、ASC、Mesh 和各阶段对象均由 Pipeline 内部保存。

**函数**：

- `UGGYGOCharacterRuntimeComponent()`：关闭组件 Tick，创建单一 `FCharacterControlPipeline`。
- `~UGGYGOCharacterRuntimeComponent()`：在 `Private/Components/GGYGOCharacterRuntimeComponent.cpp` 中包含完整 `FCharacterControlPipeline` 定义后执行 out-of-line `= default`；由 `TUniquePtr` 回收控制 Pipeline 及其内部纯 C++ 对象。组件头直接包含 Pipeline 定义，以满足 Unreal 生成构造路径对 `TUniquePtr` 删除器完整类型的要求。
- `InitializeRuntime(ABaseCharacter*, UAbilitySystemComponent*, USkeletalMeshComponent*)`：将 Unreal 依赖转发给 `FCharacterControlPipeline::Initialize`；重复初始化由 Pipeline 防止。
- `ProcessFrame(float)`：将唯一帧入口转发给 `FCharacterControlPipeline::ProcessFrame`，不在组件内展开阶段顺序。
- `GetRuntimeData()`：转发 Pipeline 内部 RuntimeData 的非拥有指针。
- `SetMoveInput`、`ClearMoveInput`、`SetLookInput`、`SetSprintHeld`、`SetForceWalkHeld`：转发给 Pipeline 的输入边界。
- `NotifyCanYaw()`：接收 `UZZZAnimInstance::AnimNotify_CanYaw`，转发给 TurnBack 参数处理器；组件不直接修改 RuntimeData。
- `IsInitialized()`：返回控制 Pipeline 的初始化状态。

### 5.6 `Public/Pipeline/CharacterControlPipeline.h` / `Private/Pipeline/CharacterControlPipeline.cpp`

**`FCharacterFramePlan`**：由 `ResolveDecision` 生成的本帧统一结果和轻量提交包。除攻击/闪避意图、移动/攻击/闪避阻断、`ActionGranted`、主状态、步态、世界移动方向和 `bShouldMove` 外，还包含 `FStateUpdateResult` 以及 `FCharacterFrameCommandBuffer`。`bShouldMove` 同时桥接到 `FCharacterMovementCommand`，作为普通零输入帧停止曲线移动的门控；StateManager 结果只作已完成状态操作的报告，移动命令和动画发布命令由 Pipeline 在后续阶段消费。它不拥有 Unreal 对象，也不直接执行 GameplayAbility；当前项目没有 C++ Ability 激活链，因此 `ActionGranted` 只记录已有 Arbiter 结果。

**`FCharacterControlPipeline`**：唯一纯 C++ 运行时所有权和时序中心。它拥有 InputData、RuntimeData、Input/Intent/Arbiter Pipeline、StateManager 和 MotionDriver；保存 Owner/ASC/Mesh 非拥有引用；自身不 Tick。

**函数**：

- `FCharacterControlPipeline()`：创建所有纯 C++ 数据和阶段对象，并让 InputPipeline 绑定 InputData。
- `~FCharacterControlPipeline()`：默认析构，按成员逆序释放内部纯 C++对象。
- `Initialize(ABaseCharacter*, UAbilitySystemComponent*, USkeletalMeshComponent*)`：缓存 Unreal 依赖；按 ASC/仲裁、Intent、Motion、StateManager::Init、StateManager::InitASC 的顺序初始化；重复调用直接返回。
- `ProcessFrame(float)`：按 Capture → Intent → Decision → Motion Commit → Animation Publish → Reset 调度一整帧。
- `CaptureInputAndConstraints(float)`：先执行 Arbiter，再执行 Input；分别把外部约束写入 RuntimeData、把 Pending 输入整理为 FInputData，不在本阶段选择动作。
- `BuildIntent()`：调用现有 IntentPipeline，把稳定 InputData 转换成 RuntimeData 意图和视角结果。
- `ResolveDecision(float)`：依次执行 Gait、Parameters 和 `FGYGOStateManager::Update(DeltaTime, OutResult)`；接收状态结果后调用 `BuildFramePlan` 生成结果快照和命令缓冲。
- `CommitMovement(float)`：消费 `FCharacterMovementCommand`，调用 `FMotionDriver::Process` 提交角色移动并回写实际速度。
- `PublishAnimation(float)`：消费动画发布命令，在已有 `UZZZAnimInstance` 存在时调用 `PipelineDrive`；不创建第二个动画实例。
- `ResetFrame()`：调用 `FRuntimeData::ResetFrameIntents`，只清除帧级攻击/闪避意图，不清空最近一次计划。
- `BuildFramePlan(const FStateUpdateResult&)`：从 RuntimeData 复制本帧结果，保存 StateManager 输出，并构造移动/动画发布命令；移动命令桥接 `bShouldMove`、`DesiredWorldMoveDir`、`TurnBackPhase`、`bTurnBackSecondSegment`、RM_Speed、`AnimCurveYawDelta`、`AnimCurveVelocity`、`AnimCurveVelocityDirection`、`RootMotionDelta` 和曲线源标记。`DesiredWorldMoveDir` 仍由 LocomotionIntentProcessor 按摄像机水平 Yaw 解析；TurnBack d0 在 CanYaw 前应用由累计 RM_Yaw 相邻采样差分得到的 AnimCurveYawDelta 并按当前 Bone_Root 相对入口的逆 yaw 修正速度方向，保持世界位移直线，d1 在 CanYaw 后使用玩家输入方向。
- `NotifyCanYaw()`：接收新版 ZZZAnim 的 CanYaw Notify，转发给 `FIntentPipeline`/`FTurnBackPhaseProcessor`；实际 d1 状态仍由 RuntimeData 和本帧命令统一产生。
- `GetRuntimeData()`：返回内部 RuntimeData 的非拥有指针。
- `SetMoveInput`、`ClearMoveInput`、`SetLookInput`、`SetSprintHeld`、`SetForceWalkHeld`：将外部输入写入 InputPipeline 的 Pending 区域。
- `GetLastFramePlan()`：返回最近一次完成 Decision 阶段的只读计划和命令快照。
- `IsInitialized()`：返回 Pipeline 初始化标志。

`FCharacterFramePlan` 的 `StateUpdate` 是 StateManager 已完成操作的结果，不会在 Commit 阶段再次请求状态；`Commands.Movement` 和 `Commands.Animation` 是当前已经接线的真实提交命令。GA、攻击、闪避和额外状态命令仍未接入。

### 5.7 `Public/GGYGOPlayerController.h` / `Private/GGYGOPlayerController.cpp`

**`AGGYGOPlayerController`**：空的 `APlayerController` 扩展壳。

- 头文件只有 UCLASS 声明和生成宏。
- cpp 没有自定义函数；输入映射上下文注册当前留给蓝图。

### 5.8 `Public/GGYGOGameplayEffects.h` / `Private/GGYGOGameplayEffects.cpp`

**文件职责**：集中保存全局 GameplayEffect 软引用路径。

- `GGYGOGEs::Restriction::{BlockAll, BlockCombat, BlockMoveOnly, Invincible}`：限制类 GE soft class pointer。
- `GGYGOGEs::Cooldown::{CooldownEvade, CooldownSkill}`：冷却类 GE soft class pointer。
- `GGYGOGEs::Attribute::InjuredSlowdown`：受伤减速 GE soft class pointer。
- `InitGEGlobals()`：按 Restriction → Cooldown → Attribute 顺序对所有 soft class pointer 调用 `LoadSynchronous`。当前没有加载失败回退或诊断。

### 5.9 `Public/GGYGOTags.h`

**文件职责**：集中定义 GameplayTag 的 FName 常量和缓存结构。

- `GGYGOTags::State`：状态名称常量。
- `GGYGOTags::Restriction`：阻止移动、攻击、闪避、跳跃、输入、交互和伤害等限制名称。
- `GGYGOTags::Ability`、`Cooldown`、`Event`：能力、冷却、事件名称。
- `FGGYGOTagCache::Init()`：把所有 FName 通过 `FGameplayTag::RequestGameplayTag` 转成缓存 GameplayTag。

当前 `FGASArbiter` 自己 RequestGameplayTag，没有使用一个全局 `FGGYGOTagCache` 实例。

## 6. 属性、配置、输入和逻辑数据

### 6.1 `Public/Attributes/GGYGOAttributeSet.h` / `Private/Attributes/GGYGOAttributeSet.cpp`

**`UGGYGOAttributeSet`**：GAS `UAttributeSet`，公开属性为 `Health`、`MaxHealth`、`AttackPower`、`Defense`、`MoveSpeed`、`IncomingDamage`。`ATTRIBUTE_ACCESSORS` 宏为每项生成 Getter/Setter/Initializer。

**函数**：

- `UGGYGOAttributeSet()`：当前不设置默认属性值。
- `PreAttributeChange(const FGameplayAttribute&, float&)`：调用父类；Health 使用 `ClampHealth`，MoveSpeed 使用 `ClampMoveSpeed`。
- `PostGameplayEffectExecute(const FGameplayEffectModCallbackData&)`：调用父类；修改目标是 IncomingDamage 时转给 `HandleIncomingDamage`。
- `ClampHealth(float&)`：把 Health 限制在 0 与 `GetMaxHealth()` 之间。
- `ClampStamina(float&)`：当前空函数，等待 Stamina/MaxStamina 属性；当前没有调用链。
- `ClampMoveSpeed(float&)`：把速度限制为不小于 0。
- `HandleIncomingDamage(const FGameplayEffectModCallbackData&)`：读取 IncomingDamage，清零元属性；若伤害大于 0，则扣减 Health 并再次钳制。死亡委托没有实现。

### 6.2 `Public/Data/Config/UCharConfigData.h` / `Private/Data/Config/UCharConfigData.cpp`

- `FStateTransitionBlend`：USTRUCT，保存 `BlendInTime` 和 `BlendOutTime`。
- `UCharConfigData`：角色配置 DataAsset，保存默认 Blend、按状态覆盖、循环/非循环播放速率、`FMovementConfig`、默认 Ability 和 GE 数组。

**函数**：

- `UCharConfigData()`：空构造函数。
- `GetBlendInTime(TargetState)`：查 `PerStateBlendOverrides`，命中返回覆盖值，否则返回默认 BlendIn。
- `GetBlendOutTime(SourceState)`：查状态覆盖，命中返回覆盖值，否则返回默认 BlendOut。
- `GetPlayRateForState(State)`：Idle/Moving 使用循环速率，其它状态使用非循环速率。

当前没有把 DataAsset 中的 DefaultAbilities/DefaultEffects 自动复制到 `ABaseCharacter`，也没有自动驱动 MotionDriver。

### 6.3 `Public/Movement/MovementConfig.h`

- `FMovementConfig`：header-only USTRUCT。字段包括 SprintMultiplier、WalkToRunHoldSeconds、SprintSpeed、AirControlFactor、DodgeSpeed、DodgeDuration、KnockbackDecay、RootMotionScale、TurnBackReleaseTimeSeconds、TurnBackSecondSegmentTimeSeconds、TurnBackDurationSeconds、bDebugMotion。
- 当前实际消费：`FGaitAuthorityProcessor` 读取 `WalkToRunHoldSeconds`；`FMotionDriver::Init` 读取 `RootMotionScale`；`FTurnBackPhaseProcessor::Init` 读取 TurnBack 释放时间和总时长，d1 不再由 `TurnBackSecondSegmentTimeSeconds` 时间点触发，而由动画 `CanYaw` Notify 触发。`TurnBackSecondSegmentTimeSeconds` 保留为配置兼容字段，当前运行时不消费。

### 6.4 `Public/Data/Input/InputData.h`

- `FProcessedInput`：保存处理后的 Move/Look、攻击/闪避/冲刺/强制步行持续状态和两个 Buffer Timer。
  - `IsAttackPressed()`：判断攻击计时器是否大于 0。
  - `IsDodgePressed()`：判断闪避计时器是否大于 0。
  - `ConsumeAttack()`：把攻击计时器清零。
  - `ConsumeDodge()`：把闪避计时器清零。
- `FInputData`：双缓冲容器，保存 `CurrentFrame` 和 `LastFrame`。
  - `AdvanceFrame()`：把 CurrentFrame 整体复制到 LastFrame。

`FInputPipeline` 是当前唯一写入 FInputData 的系统。

### 6.5 `Public/Data/Runtime/RuntimeData.h`

`FRuntimeData` 聚合 `Intent`、`View`、`Gait`、`Movement`、`Arbiter`、`State`、`RootMotion`、`ZZZAnim`。

- `ResetFrameIntents()`：只调用 `Intent.ResetFrameIntents`，清理帧级攻击/闪避意图，不清理跨帧 Model 状态。

### 6.6 `Public/Data/Runtime/*.h`

这些文件没有 `.cpp`，是 header-only 纯运行时 Model：

- `ArbiterRuntimeModel.h`：保存三个 Block bool 和 `ActionGranted`；无函数。
- `GaitRuntimeModel.h`：保存 `bDodgeRunPending` 和 `ResolvedGait`；无函数。
- `IntentRuntimeModel.h`：保存攻击/闪避意图和世界移动方向；`ResetFrameIntents()` 只清动作意图。
- `MovementRuntimeModel.h`：`FTurnBackRuntimeModel` 保存逻辑 TurnBack Phase、`bCanYaw`、`bSecondSegment` 和从 Frozen 入口累计的 `ElapsedSeconds`；`bCanYaw` 由 CanYaw AnimNotify 经 Pipeline 转发后写入，`bSecondSegment` 随之进入 d1；`FMovementRuntimeModel` 保存 TurnBack、速度、角度、移动/接地状态；无显式函数。
- `RootMotionRuntimeModel.h`：保存 RM_Speed、由累计 RM_Yaw 相邻采样差分得到的 AnimCurveYawDelta、RM_Dist、RM_Pos、RootMotionDelta，以及由 RM_PosX/RM_PosY 差分得到的固定动画起始根轨迹速度 `AnimCurveVelocity`；`AnimCurveVelocityDirection` 是同一根轨迹坐标（X=前、Y=右）的最终有效方向，优先来自 authored `RM_VelocityDirX/Y`，并由 `bHasAuthoredVelocityDirection` 标记来源，缺失时回退位置差分；`bHasRootMotionCurveSource` 标记当前帧是否有速度、方向或距离曲线源；`AnimCurveAngle` 与 `bHasRootMotion` 保留方向角和位置轨迹数据；无显式函数。
- `ZZZAnimRuntimeModel`（位于 `Contracts/Animation/ZZZAnimRuntimeModel.h`）：保存逻辑投影给新版动画层的步态、移动意图、Actor 局部平滑移动 `AnimBlendX/AnimBlendY`、速度字段和由 MotionDriver 提交后实际速度派生的 `ActualVelocityDirection`、`ActualVelocityBlendX/ActualVelocityBlendY`、`ActualVelocityAngle`。
- `StateRuntimeModel.h`：保存 `CurrentState`，默认 Idle；无显式函数。
- `ViewRuntimeModel.h`：保存 `ControlRotation`；无显式函数。

## 7. Pipeline 总体说明

Pipeline 不拥有独立 Tick。唯一控制根是 `FCharacterControlPipeline`；它拥有各纯 C++ 阶段对象，并在 `ABaseCharacter::Tick` 转发的 `ProcessFrame` 中显式维护 Capture → Intent → Decision → Motion Commit → Animation Publish → Reset。内部 Input/Arbiter/Intent/State/Motion 类型是阶段实现，不再分别作为 Runtime Component 的顶层所有权入口。

### 7.1 `Public/Pipeline/CharacterControlPipeline.h` / `Private/Pipeline/CharacterControlPipeline.cpp`

`Public/Contracts/Pipeline/CharacterFrameCommands.h` 定义 Pipeline 到 MotionDriver/ZZZAnimInstance 的本帧提交契约，只包含当前已经真实接线的移动和动画发布命令，不包含虚构的 GA/攻击/闪避命令。`FCharacterMovementCommand` 由 `BuildFramePlan` 写入 `bShouldMove`、`DesiredWorldMoveDir`、`TurnBackPhase`、`bTurnBackSecondSegment`、RM_Speed、`AnimCurveYawDelta`、根轨迹方向/位移和曲线源标记；MotionDriver 在 d0 应用由累计 RM_Yaw 相邻采样差分得到的 AnimCurveYawDelta 并用 Bone_Root 当前相对入口的逆 yaw 修正速度方向，普通输入与 d1 继续使用摄像机修正的世界方向。

**`FCharacterFramePlan`**：header-only 本帧结果和轻量提交包。`Reset()` 清空快照、`FStateUpdateResult` 和移动/动画命令；Pipeline 在 `BuildFramePlan(const FStateUpdateResult&)` 中从 RuntimeData 和 StateManager 输出构造它。StateManager 结果不会在 Commit 阶段重放，当前命令缓冲只包含真实接线的移动提交和 ZZZ 动画发布，不直接执行 GA。

**`FCharacterControlPipeline`**：唯一纯 C++ 所有权和时序中心。

- `FCharacterControlPipeline()`：创建 InputData、RuntimeData、InputPipeline、IntentPipeline、ArbiterPipeline、StateManager 和 MotionDriver，并把 InputPipeline 绑定到 InputData。
- `~FCharacterControlPipeline()`：默认析构，释放内部纯 C++对象。
- `Initialize(ABaseCharacter*, UAbilitySystemComponent*, USkeletalMeshComponent*)`：缓存非拥有 Unreal 依赖，按 ASC/仲裁、Intent、Motion、StateManager::Init、StateManager::InitASC 顺序初始化；重复调用直接返回。
- `ProcessFrame(float)`：按六个显式阶段调度一帧，不在 Unreal Component 或 AnimInstance 中重复调度。
- `CaptureInputAndConstraints(float)`：先执行 `FArbiterPipeline::Process`，再执行 `FInputPipeline::Process`；只采集约束和稳定输入。
- `BuildIntent()`：执行 `FIntentPipeline::ProcessIntents`，将 `FInputData` 转换为 RuntimeData 意图。
- `ResolveDecision(float)`：执行 Gait、Parameters 和 `FGYGOStateManager::Update(DeltaTime, OutResult)`，接收状态结果后生成 `FCharacterFramePlan`。
- `CommitMovement(float)`：消费 `FCharacterMovementCommand`，调用 `FMotionDriver::Process` 提交角色移动。
- `PublishAnimation(float)`：消费动画发布命令，在所有逻辑生产者结束后获取已有 `UZZZAnimInstance` 并调用 `PipelineDrive`。
- `ResetFrame()`：调用 `FRuntimeData::ResetFrameIntents`，只清理帧级动作意图。
- `BuildFramePlan(const FStateUpdateResult&)`：复制当前 Intent、Arbiter、State、Gait 和 ZZZAnim 结果，保存状态更新结果，并构造移动/动画命令；移动命令桥接 `bShouldMove`、`DesiredWorldMoveDir`、`TurnBackPhase`、`bTurnBackSecondSegment`、RM_Speed、`AnimCurveYawDelta`、`AnimCurveVelocity`、`AnimCurveVelocityDirection`、`RootMotionDelta` 和曲线源标记。`DesiredWorldMoveDir` 仍由 LocomotionIntentProcessor 按摄像机水平 Yaw 解析；TurnBack d0 在 CanYaw 前应用由累计 RM_Yaw 相邻采样差分得到的 AnimCurveYawDelta 并按当前 Bone_Root 相对入口的逆 yaw 修正速度方向，保持世界位移直线，d1 在 CanYaw 后使用玩家输入方向；Actor yaw 的 d0 写入由 MotionDriver 完成。
- `GetRuntimeData()`：返回内部 RuntimeData 的非拥有指针。
- `SetMoveInput`、`ClearMoveInput`、`SetLookInput`、`SetSprintHeld`、`SetForceWalkHeld`：将外部输入写入 InputPipeline 的 Pending 区域。
- `GetLastFramePlan()`：返回最近一次完成 Decision 阶段的只读计划、状态结果和命令快照。
- `IsInitialized()`：返回初始化标志。

当前没有 C++ GameplayAbility 激活链，因此 `FCharacterFramePlan::ActionGranted` 只记录现有 Arbiter 结果；不能把它解释为 GA 命令或已执行的 Ability。

### 7.2 Pipeline 接口

`Public/Pipeline/Interfaces/IArbiter.h`：

- `IArbiter::~IArbiter()`：虚析构，允许通过接口指针释放具体仲裁器。
- `IArbiter::Arbitrate(FRuntimeData&, float)`：纯虚接口；具体仲裁器读取外部约束并写仲裁 Model。

`Public/Pipeline/Interfaces/IIntentProcessor.h`：

- `IIntentProcessor::~IIntentProcessor()`：虚析构。
- `IIntentProcessor::Process(const FInputData&, FRuntimeData&)`：纯虚意图处理接口。

`Public/Pipeline/Interfaces/IParameterProcessor.h`：

- `IParameterProcessor::~IParameterProcessor()`：虚析构。
- `IParameterProcessor::Process(FRuntimeData&, float)`：纯虚参数处理接口。

这三个接口都不拥有数据、没有 `.cpp` 和没有 Tick。

### 7.3 `InputPipeline.h/.cpp`

**`FInputPipeline`**：把 Enhanced Input 回调写入 Pending 字段，再在 Pipeline 阶段生成稳定的 `FInputData`。

**字段**：外部引用 `InputData`；ActionBufferTime=0.15 秒；MoveFlickerBuffer=0.05 秒；Pending Move/Look、Attack/Dodge/Sprint/ForceWalk；防抖计时器和最后有效移动方向。

**函数**：

- `FInputPipeline(FInputData&)`：绑定外部 InputData，不取得所有权。
- `Process(float)`：推进双缓冲；执行移动防抖；写 Look 和持续按键；更新 Attack/Dodge Buffer；清理瞬时 Pending 输入，保留 Sprint/ForceWalk 持续状态。
- `SetMoveInput(const FVector2D&)`：更新 PendingMoveInput。
- `ClearMoveInput()`：把 PendingMoveInput 设为零。
- `SetLookInput(const FVector2D&)`：更新 PendingLookInput。
- `SetAttackPressed()`：设置 PendingAttack。
- `SetDodgePressed()`：设置 PendingDodge。
- `SetSprintHeld(bool)`：设置 PendingSprint。
- `SetForceWalkHeld(bool)`：设置 PendingForceWalk。

### 7.4 `IntentPipeline.h/.cpp`

**`FIntentPipeline`**：拥有四个 Intent Processor、三个 Parameter Processor，并单独拥有一个具名 Gait Authority。

**函数**：

- `Init(ACharacter*, USkeletalMeshComponent*)`：按固定顺序初始化 ViewRotation、Locomotion、Attack、Dodge；再初始化 Movement、RootMotion、TurnBack 参数处理器；初始化 GaitAuthority。
- `ProcessIntents(const FInputData&, FRuntimeData&)`：按数组顺序运行四个 Intent Processor。
- `ProcessGait(const FInputData&, FRuntimeData&, float)`：运行步态权威并记录当前 `GFrameCounter`。
- `ProcessParameters(FRuntimeData&, float)`：检查本帧是否已经执行 Gait；缺失时只警告且不补写，然后按 Movement → RootMotion → TurnBack 顺序运行三个参数处理器。

### 7.5 `ArbiterPipeline.h/.cpp`

**`FArbiterPipeline`**：按 GAS → Action → Health → Stamina 顺序拥有并执行 Arbiter。`HealthArbiterPtr` 是指向数组内对象的非拥有便利指针。

- `Init(UAbilitySystemComponent*, FGYGOStateManager*)`：创建并初始化四个 Arbiter；Health Arbiter 的裸指针保存给外部伤害请求使用。
- `Process(FRuntimeData&, float)`：先清除本帧三个 Block 标记并把 ActionGranted 置 Idle，然后按注册顺序调用 `Arbitrate`。
- `GetHealthArbiter()`：返回 Health Arbiter 非拥有指针。

#### `Arbiters/ActionArbiter.h/.cpp`

- `FActionArbiter::Init(ASC, StateManager)`：保存非拥有 ASC 和 StateManager。
- `Arbitrate(RuntimeData, DeltaTime)`：当前为空实现，等待 GAS/状态动作接入。
- `GetStateResistance(State, Action)`：静态脚手架函数，当前恒返回 0。
- `GetActionPriority(Action)`：静态脚手架函数，当前恒返回 50。

#### `Arbiters/GASArbiter.h/.cpp`

- `FGASArbiter::Init(ASC)`：保存 ASC 并缓存 Dead、Stunned、CantMove、CantAttack、CantDodge、CantJump、CantInput GameplayTag。
- `Arbitrate(RuntimeData, DeltaTime)`：没有 ASC 时返回；Dead 或 Stunned 时封锁移动/攻击/闪避；之后根据 Restriction Tag 逐项设置 Block 标志。CantJump 暂时复用 BlockMove，CantInput 没有对应 RuntimeData 字段，因此没有实际写入。

#### `Arbiters/HealthArbiter.h/.cpp`

- `FDamageRequest`：只保存 Damage 数值。
- `FHealthArbiter::Init(ASC)`：保存 ASC。
- `RequestDamage(const FDamageRequest&)`：将请求写入容量 16 的队列；满队列时静默丢弃。
- `Arbitrate(RuntimeData, DeltaTime)`：遍历队列的位置目前只有 TODO，不应用 GE、不读 Health、不判断死亡，最后清空队列。

#### `Arbiters/StaminaArbiter.h/.cpp`

- `FStaminaArbiter::Init(ASC)`：保存 ASC。
- `Arbitrate(RuntimeData, DeltaTime)`：当前仅 TODO，没有 Stamina 属性、消耗、恢复或 Sprint 阻断实际逻辑。
- 相关字段 `bIsStaminaDepleted`、恢复阈值、消耗速率、恢复速率目前没有生效链。

### 7.6 `Pipeline/Gait/GaitAuthorityProcessor.h/.cpp`

**`FGaitAuthorityProcessor`**：唯一写入 `RuntimeData.Gait.ResolvedGait` 和 `RuntimeData.ZZZAnim.Gait` 的步态权威。

**跨帧字段**：Owner 弱指针、WalkHoldTimer、上一帧 Gait、上一帧 Move、上一帧 BlockMove、上一帧 State 和诊断节流标志。

**函数**：

- `Init(ACharacter*)`：保存并校验 Owner；若是 BaseCharacter 则读取其配置。
- `Process(const FInputData&, FRuntimeData&, float)`：解析 Delta、当前局部步态、Walk 持续计时；在达到阈值时即时升级 Run；一次性写入两个 Gait 字段并更新跨帧基线。
- `ResolveEffectiveThreshold()`：从 BaseCharacter 配置读取 WalkToRunHoldSeconds；非法/缺失回退默认 5 秒，并限制最大 60 秒。
- `ResolveFrameDelta(DeltaTime, OutClampedDelta)`：把非法或过大的 Delta 限制到安全范围，并通过输出参数返回钳制后的值。
- `ResolveFrameGait(RuntimeData, bMoveInputPresent, bLeftMovingState)`：按阻挡、输入、DodgeRunPending、上一帧 Run 和普通 Walk 的优先级解析本帧步态；必要时消费 Dodge_Run_Contract。
- `UpdateWalkHoldTimer(FrameGait, bMoveInputPresent, bBlockMove, bMovingState, bMoveInputRising, bBlockReleased, ClampedDelta, bDeltaValid)`：根据步态、移动输入、阻挡状态、状态变化和有效 Delta 维护 Walk 计时器。
- `ShouldUpgradeToRun(FrameGait, bMoveInputPresent, bBlockMove)`：判断当前是否持续有效移动、未阻挡且达到 Run 阈值，并决定是否升级为 Run。

`GaitLog.h/.cpp` 只声明和定义 `LogGait` 日志类别。

### 7.7 Intent 处理器

#### `Intents/ViewRotationProcessor.h/.cpp`

- `Init(ACharacter*)`：缓存非拥有 Owner。
- `Process(InputData, RuntimeData)`：读取当前 Look，调用 Owner 的 `AddControllerYawInput`/`AddControllerPitchInput`，再写最终 ControlRotation 到 `RuntimeData.View`。必须排在 Locomotion 前。

#### `Intents/LocomotionIntentProcessor.h/.cpp`

- `Process(InputData, RuntimeData)`：读取当前 Move；近零时清空世界移动方向并关闭 `ZZZAnim.bShouldMove`；有输入时使用 ControlRotation 的水平 Yaw 把相机局部输入转换为世界方向，写入 DesiredWorldMoveDir 和 bShouldMove。

#### `Intents/AttackIntentProcessor.h/.cpp`

- `Process(InputData, RuntimeData)`：若 `CurrentFrame.IsAttackPressed()`，写 `Intent.bWantsToAttack=true`；当前不消费 Buffer。

#### `Intents/DodgeIntentProcessor.h/.cpp`

- `Process(InputData, RuntimeData)`：若 `CurrentFrame.IsDodgePressed()`，写 `Intent.bWantsToDodge=true`；当前不消费 Buffer。

### 7.8 Parameter 处理器

#### `Parameters/MovementParameterProcessor.h/.cpp`

- `Init(ACharacter*)`：缓存非拥有 Owner，用于读取 Actor 当前水平朝向。
- `Process(RuntimeData, DeltaTime)`：读取世界移动方向并转换为相对 Actor 的局部 X/Y（X=右，Y=前），写 `MoveAngle`，使用 `FInterpTo` 平滑，并将结果写入 `RuntimeData.ZZZAnim.AnimBlendX/AnimBlendY`，供 Snapshot 和 AnimBP 的方向 BlendSpace 使用。

#### `Parameters/RootMotionParameterProcessor.h/.cpp`

- `Init(USkeletalMeshComponent*)`：缓存 Mesh，并清理曲线采样基线。
- `Process(RuntimeData, DeltaTime)`：清零当前 RootMotion 输出；从 AnimInstance 采样 RM_PosX、RM_PosY、RM_Dist、RM_Speed、RM_Yaw、RM_VelocityDirX 和 RM_VelocityDirY；RM_Yaw 使用当前采样值减上一帧采样值做差分，首帧或重置基线输出 0，曲线值保持不变时不重复累加；处理有限值、首次采样、曲线回退和位置差分；写 AnimCurveSpeed/AnimSpeed、`AnimCurveYawDelta`、RootMotionDelta、bHasRootMotion、`bHasRootMotionCurveSource` 和距离字段，并用 `DeltaPos / DeltaTime` 写入固定动画起始根轨迹坐标中的 `AnimCurveVelocity`。有效 authored 方向曲线写入最终 `AnimCurveVelocityDirection` 并设置 `bHasAuthoredVelocityDirection`，缺失或无有效非零样本时使用 RM_PosX/RM_PosY 差分；`AnimCurveAngle` 使用最终有效方向。上述字段由 `CharacterControlPipeline::BuildFramePlan` 复制到 `FCharacterMovementCommand`。MotionDriver 普通 walkrun 无 dir 曲线，移动方向就是摄像机修正后的 `DesiredWorldMoveDir`，RM_Speed 只决定速度；TurnBack d0 在 CanYaw 前应用差分得到的 AnimCurveYawDelta，并用当前 Bone_Root 相对入口的逆 yaw 修正 RM_VelocityDirX/Y，使世界位移保持入口直线；CanYaw 后进入 d1，像 walkrun 一样朝输入方向移动。
- `ResetSampleState()`：清空上帧位置、距离和 RM_Yaw 累计曲线采样值，并关闭基线标志。

RM_Speed 是 MotionDriver 的速度主值；RM_Dist 只做诊断，不与位移额外叠加。

#### `Parameters/TurnBackPhaseProcessor.h/.cpp`

- `Init(ACharacter*)`：缓存 Owner；从 `ABaseCharacter::GetCharacterConfig()->MovementConfig` 读取 TurnBack 释放时间和总时长，缺失或非法值使用默认值；d1 不再读取 `TurnBackSecondSegmentTimeSeconds`。
- `NotifyCanYaw()`：接收 `UZZZAnimInstance::AnimNotify_CanYaw` 转发来的事件，等待下一次参数阶段消费。
- `Process(RuntimeData, DeltaTime)`：只读取 State、输入方向、Gait 和 `ZZZAnim.bShouldMove`，按逻辑时间轴维护 TurnBack Phase、`bCanYaw`、`bSecondSegment` 和累计时间；Run 且输入反向时进入 Frozen；到达释放时间进入 Released；CanYaw 到达后立即设置 d1；第一段不可被无输入打断，d1 开始后无输入可以退出，总时长到达时自然退出；不读取动画旋转曲线推进生命周期。自然完成后保留反向输入边沿锁存，必须先离开反向阈值才能再次触发，避免持续反向输入在结束帧立即开启新的 TurnBack。

当前该处理器使用规则层默认反向阈值，没有读取 `FZZZAnimTuning::TurnBackReverseInputDotThreshold`。

## 8. StateMachine

### 8.1 `Public/StateMachine/CharacterStateType.h`

枚举：

- `ECharacterStateType`：当前实际只有 None、Idle、Moving、MAX。
- `EStateGroup`：Locomotion、Action、Overlay、System、MAX。
- `EStateRelationType`：Independent、Interrupted、Blocked。
- `EMovementGait`：None、Walk、Run。
- `ETurnBackPhase`：None（不在转身）、Frozen（已触发转身，逻辑时间轴第一段且不可被输入打断）、Released（到达释放时间点，等待第二段或总时长；第二段开始后可被无输入打断）。

注释中提到的 InAir、Attacking、Dodging、HitStun、Stunned、Dead、Interacting 当前没有出现在实际枚举中。

### 8.2 `Public/StateMachine/CharacterState.h` / `Private/StateMachine/CharacterState.cpp`

**`FCharacterState`**：状态基类，保存类型、组、名字和 Active 标记；`FGYGOStateManager` 是 friend。

- `FCharacterState(ECharacterStateType, EStateGroup)`：保存类型和组；cpp 中通过 `UEnum::GetValueAsString` 生成 StateName。
- `~FCharacterState()`：虚析构。
- `Enter(FRuntimeData&)`：默认空实现，状态进入钩子。
- `Update(float, FRuntimeData&, FGYGOStateManager&)`：默认空实现，状态每帧更新钩子。
- `Exit(FRuntimeData&)`：默认空实现，状态退出钩子。
- `GetStateType()`：返回枚举类型。
- `GetStateGroup()`：返回状态组。
- `GetStateName()`：返回调试名称。
- `IsActive()`：返回 Active 标记。
- `GetEnterGameplayEffects()`：默认返回空 GE 列表。
- `GetRemoveGEsWithTag()`：默认返回空 GameplayTag。
- `GetLimitFlags()`：默认返回 0。
- `GetMovementDA()`：默认返回空 Movement DataAsset soft pointer。

### 8.3 `Public/StateMachine/GGYGOStateManager.h` / `Private/StateMachine/GGYGOStateManager.cpp`

**`FGYGOStateManager`**：纯 C++ 并行状态管理器。`AllStates` 通过 `TUniquePtr` 拥有状态；`ActiveStates` 只保存裸指针；RuntimeData 和 ASC 都是非拥有引用。

- `FGYGOStateManager()`：当前无额外逻辑。
- `~FGYGOStateManager()`：当前无额外逻辑，成员自动析构。
- `Init(FRuntimeData&, UDataTable*)`：保存 RuntimeData；注册 Idle/Moving；加载 DataTable 关系或构建默认关系；激活 Idle。
- `InitASC(UAbilitySystemComponent*)`：保存 ASC 非拥有引用。
- `Update(float)`：兼容旧调用方，调用带输出参数的更新并保存最近一次 `FStateUpdateResult`。
- `Update(float, FStateUpdateResult&)`：复制 ActiveStates 后逐个更新仍 active 的状态；状态内部请求仍由 StateManager 执行，同时通过输出结果向 Pipeline 报告已完成的激活、释放或拒绝事件。
- `GetLastUpdateResult()`：返回最近一次 Update 的状态结果。
- `RequestState(ECharacterStateType)`：已经活跃则成功返回；查找目标状态；调用 CheckRelations；Blocked 拒绝并记录拒绝事件；Interrupted 先停用；最后激活目标并记录实际转换事件。
- `ReleaseState(ECharacterStateType)`：确认目标活跃后停用；将最终 PrimaryState 同步到逻辑 State 和 ZZZAnim CurrentState，并记录释放结果。
- `IsInState(ECharacterStateType)`：遍历 ActiveStates 查询状态。
- `IsInGroup(EStateGroup)`：遍历 ActiveStates 查询组。
- `GetPrimaryState()`：返回缓存的 Locomotion 主状态。
- `GetActiveStates()`：返回 ActiveStates 只读引用。
- `ForceSetPrimaryState(ECharacterStateType)`：不检查关系矩阵，停用当前 Locomotion 状态并激活目标；在结果收集期间记录强制转换。
- `GetRelation(From, To)`：查缓存关系；未命中时安全返回 Blocked。
- `IsActionAllowed(ActionRestrictionTag)`：无 ASC 时默认允许；有 ASC 时检查限制 Tag 是否存在。
- `CanEnterState(NewState)`：检查当前 PrimaryState 到目标的关系是否不是 Blocked。
- `RegisterState<TStateClass>(Type)`：模板函数，创建具体状态并放入 AllStates。
- `LoadRelationMatrix(UDataTable*)`：通过枚举名称和反射读取 DataTable 行列；没有表时使用默认矩阵。
- `BuildDefaultRelationMatrix()`：只建立 Idle→Moving、Moving→Idle 的 Interrupted 和同态 Blocked 关系。
- `CheckRelations(NewStateType)`：遍历活跃旧状态，根据 Old→New 关系收集要中断的状态或立即拒绝。
- `BeginUpdateResult(FStateUpdateResult&)` / `FinishUpdateResult(FStateUpdateResult&)`：管理一次 Update 的结果收集生命周期，并计算最终 PrimaryState 是否变化。
- `RecordTransitionEvent(...)`：把 StateManager 已完成的状态操作写入当前结果，不改变状态规则或副作用。
- `ActivateState(FCharacterState*)`：依次执行 Enter、进入 GE、冲突 GE 移除、限制位应用、加入 ActiveStates、更新 PrimaryState，并同步 State/ZZZAnim。
- `DeactivateState(FCharacterState*)`：执行 Exit、移出 ActiveStates、清 active 标记和更新 PrimaryState；当前函数体没有真正调用移除 GE 的逻辑。
- `UpdatePrimaryState()`：从活跃 Locomotion 状态重新计算 PrimaryState，无活跃 Locomotion 时回退 Idle。
- `ApplyLimitFlags(int32)`：将状态限制位映射写入 RuntimeData.Arbiter 的 Block 标志。
- `ApplyEnterGameplayEffects(FCharacterState*)`：读取状态提供的 GE 类，创建 Spec 并通过 ASC 应用。
- `RemoveGameplayEffectsByTag(FCharacterState*)`：使用 GameplayEffectQuery 按 OwnedTags 批量移除匹配 GE。

当前 Runtime Component 没有传入 DataTable，因此默认只注册 Idle/Moving。StateManager 的状态转换、状态生命周期和状态侧 RuntimeData/GE 副作用仍由自身完成；`FStateUpdateResult` 只向 Pipeline 报告已完成结果，Pipeline 不重放状态操作。

### 8.4 `Public/Contracts/State/StateUpdateResult.h`

**`FStateTransitionEvent`**：记录一次状态激活、释放、强制切换或拒绝操作，包括请求状态、操作前后的 PrimaryState、接受结果和被中断状态列表。

**`FStateUpdateResult`**：StateManager 一次 `Update` 的聚合输出，记录起始/最终 PrimaryState、是否发生变化以及按发生顺序排列的状态事件。它是 StateManager → Pipeline 的只读结果契约，不会被 Pipeline 再次提交给 StateManager。

### 8.5 `StateMachine/State/IdleState.h/.cpp`

**`FIdleState`**：Locomotion 组的 Idle 状态。

- `FIdleState()`：将类型设为 Idle、组设为 Locomotion。
- `Enter(RuntimeData)`：当前空实现。
- `Update(DeltaTime, RuntimeData, StateManager)`：若 Arbiter ActionGranted 非 Idle，请求该状态；否则有世界移动输入且未被 BlockMove 时请求 Moving。
- `Exit(RuntimeData)`：当前空实现。

### 8.6 `StateMachine/State/MovingState.h/.cpp`

**`FMovingState`**：Locomotion 组的 Moving 状态。

- `FMovingState()`：将类型设为 Moving、组设为 Locomotion。
- `Enter(RuntimeData)`：继承基类默认空实现，当前没有自定义逻辑。
- `Update(DeltaTime, RuntimeData, StateManager)`：若 ActionGranted 是其它状态，请求该状态；否则无世界移动输入且未 BlockMove 时请求 Idle。
- `Exit(RuntimeData)`：继承基类默认空实现。

### 8.7 `StateMachine/Data/FStateRelationRow.h`

`FStateRelationRow` 是 DataTable USTRUCT，字段为 Idle、Moving、InAir、Attacking、Dodging、HitStun、Stunned、Dead、Interacting，默认关系为 Blocked。当前枚举只有 None/Idle/Moving，扩展字段尚无对应运行时状态。

## 9. MotionDriver

### `Public/Drivers/MotionDriver.h` / `Private/Drivers/MotionDriver.cpp`

**`FMotionDriver`**：纯 C++ 最终运动驱动器。缓存 Owner、CharacterMovement、Mesh 和 RootMotionScale；RM_Speed 是唯一速度主值，AnimCurveYawDelta 是由累计 RM_Yaw 相邻采样差分得到的 D0 当前帧角度增量。分两类移动：

- **普通 walkrun**（无 dir 曲线）：移动方向 = 玩家输入方向 `DesiredWorldMoveDir`（摄像机相对解析的世界方向）；Actor 朝向由 CharacterMovement/Controller 的现有配置处理。
- **TurnBack**（有固定方向曲线，dir 约定 X=左右、Y=前后）：分 d0/d1 两段，由命令中的 `bTurnBackSecondSegment` 区分。
  - d0（CanYaw 之前）：TurnBack 进入时捕获 `Bone_Root` 世界水平前向/右向；先把差分得到的 AnimCurveYawDelta 累加到 Actor yaw，再把 RM_VelocityDirX/Y 按当前 Bone_Root 相对入口的逆 yaw 修正后映射回世界，保持入口世界位移直线。
  - d1（CanYaw 之后）：像 walkrun 一样朝摄像机修正后的玩家输入方向移动，并交给现有 CharacterMovement/Controller 旋转配置接管朝向；无输入时由 `FTurnBackPhaseProcessor` 退出 TurnBack。

TurnBack Phase 和 CanYaw/d1 标记由逻辑层维护，AnimBP 只消费快照；AnimInstance 通过 `AnimNotify_CanYaw` 把信号转发给 Pipeline，MotionDriver 是 D0 Actor yaw 的唯一写入方。

- `Init(ACharacter*)`：缓存 Owner/Mesh/Movement；若 AnimInstance 已存在则将 RootMotionMode 设为 IgnoreRootMotion；从当前 `UZZZAnimInstance` 查询 TurnBack AnimSequence 时长，并从 BaseCharacter 的 CharacterConfig 读取 RootMotionScale。AnimInstance 可能晚于初始化，因此活动中的 TurnBack 还会重复确保 IgnoreRootMotion。
- `Process(float, const FCharacterMovementCommand&, FRuntimeData&)`：消费本帧移动命令；依次更新 D0 自动朝向覆盖、捕获 TurnBack 入口 Bone_Root 基准、在 D0 应用差分后的 `AnimCurveYawDelta`、更新动画计时，再按 `bShouldMove`、解析出的世界方向和 RM_Speed 提交位移。TurnBack 或有效曲线源允许动画收尾继续提交，普通零输入且无曲线源时将 MaxWalkSpeed 置零、调用 `StopMovementImmediately` 并更新黑板；BlockMove 时停止移动；最后更新 RuntimeData。
- `ResolveWorldMoveDirection(const FCharacterMovementCommand&)`：TurnBack 时委托 `ResolveTurnBackWorldMoveDirection`；普通移动直接返回玩家输入方向 `DesiredWorldMoveDir`（无 dir 曲线映射）。
- `ResolveTurnBackWorldMoveDirection(const FCharacterMovementCommand&)`：d1（`bTurnBackSecondSegment`）返回玩家输入方向 `DesiredWorldMoveDir`；d0 使用 `AnimCurveVelocityDirection`（缺失用 `RootMotionDelta`），先按当前 Bone_Root 相对入口的逆 yaw 将 X=左右/Y=前后方向转回原基准，再用当前 Bone_Root 世界前向/右向映射，保持入口世界方向；无曲线时退回入口 Bone_Root 前向。
- `UpdateTurnBackReleaseState(float, const FCharacterMovementCommand&)`：TurnBack 进入时开始累计配置动画播放时间；Phase 由 `FTurnBackPhaseProcessor` 推进后，动画完成时仅输出一次完成诊断；Phase 回到 None 后清理动画计时。
- `UpdateTurnBackDirectionIntent(const FCharacterMovementCommand&)`：进入 TurnBack 首帧捕获当前 `Bone_Root` 世界水平前向/右向，作为 d0 原始方向基准；Phase 回到 None 时清理。
- `ApplyTurnBackYaw(const FCharacterMovementCommand&)`：CanYaw 前读取命令中的 `AnimCurveYawDelta`，直接以水平增量累加 Actor yaw；CanYaw/d1 后不再应用 `AnimCurveYawDelta`。
- `UpdateTurnBackRotationMode(const FCharacterMovementCommand&)`：D0 活动期间暂时关闭 `CharacterMovement` 的 `bOrientRotationToMovement`、`bUseControllerDesiredRotation` 和 Pawn 的 `bUseControllerRotationYaw`，进入 d1 或 TurnBack 结束时恢复进入前配置。
- `ResolveTurnBackBoneRootBasis(FVector&, FVector&)`：读取当前 `Bone_Root` 的世界水平基准，缺失时回退 Actor 前向/右向。
- `EnsureRootMotionIgnored()`：在 Init 和 TurnBack 活动帧对当前 AnimInstance 设置 `ERootMotionMode::IgnoreRootMotion`，覆盖 AnimInstance 晚于 BeginPlay 实例化的情况。
- `ProcessRootMotionMovement(DeltaTime, const FCharacterMovementCommand&)`：使用 `ResolveWorldMoveDirection` 解析出的世界方向，结合有效 RM_Speed 和 RootMotionScale 构造世界速度并调用 `RequestDirectMove`；RM_Dist 不参与额外位移。
- `ProcessLocomotion(WorldDir, const FCharacterMovementCommand&)`：使用已解析的世界方向 × 有效曲线速度调用 `RequestDirectMove`。
- `UpdateRuntimeData(RuntimeData)`：在移动提交后从 Actor 水平 `Velocity` 写 `CurrentSpeed`、`ZZZAnim.VelocityLength`、`bIsMoving` 和局部 `MoveAngle`；同时将水平实际速度归一化写入 `ZZZAnim.ActualVelocityDirection`，并按 Actor 局部坐标写入 `ActualVelocityBlendX/ActualVelocityBlendY`（X=右、Y=前）和 `ActualVelocityAngle`（0=前、+90=右）。速度为零时清零这些方向字段。
- TurnBack 诊断：非 Shipping 构建在 TurnBack 活动帧读取并输出 `[TurnBack][RotationDiag]`，记录 Actor/Controller/Velocity/Acceleration 旋转、输入与曲线方向、CharacterMovement 自动朝向开关、Mesh 相对/组件旋转和 `Root`/`Bone_Root`/`Bip001` 的 Component Space Yaw；另输出 `[TurnBack][ActorYaw]` 记录每帧差分后的 AnimCurveYawDelta 和应用后的 Actor yaw；D0 同时关闭 CharacterMovement/Controller 自动朝向，d1 或 TurnBack 结束后恢复原配置。诊断日志只读，但 D0 的 Actor yaw 由 `ApplyTurnBackYaw` 唯一写入。

`FMotionDriver` 不直接决定本帧移动输入或 TurnBack Phase；这些决策字段由 `FCharacterControlPipeline::BuildFramePlan` 写入 `FCharacterMovementCommand`。普通移动方向来自摄像机相对解析后的玩家输入；TurnBack d0 用当前 Bone_Root 相对入口基准的逆 yaw 修正固定方向曲线并应用差分后的 AnimCurveYawDelta，D0 临时关闭自动朝向，d1 用玩家输入方向并恢复原有旋转配置。只有 D0 的 `ApplyTurnBackYaw` 写入 Actor yaw；`AnimCurveAngle` 和实际速度角仍分别用于曲线方向诊断与动画方向反馈。

当前没有写 `ZZZAnim.Velocity2DLength` 或 `LastInputDirectionAngle`。

## 10. 新版 ZZZAnim 动画层

新版动画层路径是 `Source/GGYGO/{Public,Private}/Animation/zzzAnim/`。旧的 `UNTEAnimInstance`、`Animation/Decisions/` 不作为新版实现依据。

### 10.1 `Animation/zzzAnim/Data/ZZZAnimEnums.h`

- `EZZZAnimLocomotionState`：None、NotMoving、Conduit、EnterMove、Moving，表示 AnimBP 移动拓扑状态。
- `EZZZAnimMovingSubState`：None、WalkRun、TurnBack，表示 Moving 内部表现子状态。

它们与逻辑 `ECharacterStateType` 不是同一套枚举。

### 10.2 `Animation/zzzAnim/Data/ZZZAnimContext.h`

- `FZZZAnimReadContext`：保存 Snapshot、Tuning、StateMemory 的 const 指针。
- `FZZZAnimReadContext::IsValid()`：检查三个指针均非空。
- `FZZZAnimWriteContext`：Snapshot/Tuning 为 const 指针，Memory 为可写指针。
- `FZZZAnimWriteContext::IsValid()`：检查三个指针均非空。
- `FZZZAnimWriteContext::ToRead()`：把可写上下文转换为只读上下文。

### 10.3 `Contracts/Animation/ZZZAnimRuntimeModel.h`

`FZZZAnimRuntimeModel` 是逻辑到新版 ZZZ 动画层的跨层游戏线程投影：逻辑 Pipeline 写入，`FZZZAnimSnapshotCapture` 读取。它位于 `Contracts/Animation`，不属于 ZZZAnim 内部表现记忆，也不拥有逻辑决策；字段包括 `Gait`、`bShouldMove`、`CurrentState`、`VelocityLength`、`Velocity2DLength`、`LastInputDirectionAngle`、Actor 局部平滑输入方向 `AnimBlendX/AnimBlendY`，以及 MotionDriver 提交后实际速度派生的 `ActualVelocityDirection`、`ActualVelocityBlendX/ActualVelocityBlendY`、`ActualVelocityAngle`。当前有写入链的是前四类字段、AnimBlendX/Y 和 ActualVelocity 方向字段，后两个旧速度兼容字段目前没有有效写入方。

### 10.4 `Animation/zzzAnim/Data/ZZZAnimSet.h`

`FZZZAnimSet` 是 USTRUCT，保存 `Sequences` 和 `BlendSpaces` 两个从 FName 到 `UAnimSequence`/`UBlendSpace` 的配置 Map；当前没有显式成员函数。

### 10.5 `Animation/zzzAnim/Data/ZZZAnimSnapshot.h`

`FZZZAnimSnapshot` 是动画侧只读快照，保存 Gait、ShouldMove、相对 Actor 的 `AnimBlendX/AnimBlendY`、由 RootMotionParameterProcessor 生成的最终有效固定动画曲线坐标 `AnimCurveVelocity`、`AnimCurveVelocityDirection`、`AnimCurveVelocityAngle`（优先 authored RM_VelocityDirX/Y，缺失时由 RM_PosX/RM_PosY 差分回退）、InputForwardDot、逻辑侧 `TurnBackPhase` 和 `bTurnBackSecondSegment`、CurrentState、VelocityLength、由 MotionDriver 写入的 `ActualVelocityDirection`、`ActualVelocityBlendX/ActualVelocityBlendY`、`ActualVelocityAngle`、Grounded 和 BlockMove；当前没有显式成员函数。Capture 当前将 `bBlockMove` 写为 false。

### 10.6 `Animation/zzzAnim/Data/ZZZAnimStateMemory.h`

`FZZZAnimStateMemory` 是动画表现层状态记忆，保存 GaitBlendY、GaitValue、StopValue、MovingSubState；当前没有显式成员函数。事件层是其唯一写入方。

### 10.7 `Animation/zzzAnim/Data/ZZZAnimTuning.h`

`FZZZAnimTuning` 保存 TurnBackReverseInputDotThreshold、LoopBlendIn、OneShotBlendOut、GaitBlendInterpSpeed；当前事件层实际读取 GaitBlendInterpSpeed，其它字段在当前 C++ 逻辑中没有消费方。

### 10.8 `Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h/.cpp`

**`FZZZAnimSnapshotCapture`**：从逻辑黑板生成动画快照，不拥有 Owner 或 RuntimeData。

- `Capture(FZZZAnimSnapshot&, ABaseCharacter*)`：先重置快照；Owner/RuntimeData 缺失则返回；复制 `RuntimeData.ZZZAnim` 中的 Gait、ShouldMove、`AnimBlendX/AnimBlendY`、ActualVelocity 方向字段、CurrentState 和 VelocityLength；从 `RuntimeData.RootMotion` 复制 `AnimCurveVelocity`、最终有效 `AnimCurveVelocityDirection` 和 `AnimCurveAngle` 到快照；复制 TurnBack Phase、`bCanYaw` 和 `bSecondSegment`；用角色前向与期望移动方向计算并钳制 InputForwardDot；读取 Grounded；当前把 BlockMove 写为 false。

### 10.9 `Animation/zzzAnim/Locomotion/ZZZLocomotionDecisions.h/.cpp`

**`FZZZLocomotionDecisions`**：只读动画过渡判定对象，保存只读 Context，不访问 Actor，不产生副作用。

- `SetContext(FZZZAnimReadContext)`：保存只读上下文。
- `NotMoving_To_Conduit()`：有快照、有移动意图且逻辑状态为 Moving 时返回 true。
- `Stop_To_Conduit()`：有快照且有移动意图时返回 true，不读取速度/步态。
- `Conduit_To_EnterMove()`：快照缺失或 Gait 不是 Run 时返回 true。
- `Conduit_To_Moving_Direct()`：快照存在且 Gait 是 Run 时返回 true。
- `ShouldStopMoving()`：有快照且当前没有移动意图时返回 true。
- `ShouldExitMoving()`：同样检查无移动意图，但 TurnBack Phase 为任一非 None 时强制 false，保证逻辑时间轴完成前不会因中途松开输入而退出顶层 Moving。
- `WalkRun_To_TurnBack()`：快照状态为 Moving 且 TurnBackPhase 不为 None 时返回 true；不要求 AnimBP 恰好在 Frozen 的短窗口内求值。

### 10.10 `Animation/zzzAnim/Locomotion/ZZZLocomotionEvents.h/.cpp`

**`FZZZLocomotionEvents`**：可写动画表现记忆推进器。

- `SetContext(FZZZAnimWriteContext)`：保存可写上下文。
- `SynchronizeMovingSubState()`：非 Moving 写 None；Moving 且无 TurnBack 写 WalkRun；其它 Moving 写 TurnBack。
- `AdvanceGaitBlend(float)`：先推进 Stop 选择，再推进离散 GaitValue 和连续 GaitBlendY。
- `AdvanceStopSelection(float)`：处理首次进入 Moving 的 0.3 秒起步早停窗口、StopValue 和窗口跨帧状态。
- `ResolveTargetFromSnapshot()`：根据 Snapshot Gait 将 Run 目标解析为 1，Walk/None 目标解析为 0；None 时沿用上次目标。
- `ReportInvalidSnapshotGaitIfNeeded()`：对非法 Gait 做节流诊断，不修改源快照。
- `AdvanceGaitBlendY(float, float)`：依据 Tuning 或默认速率将 GaitBlendY 平滑到目标，非法速率时吸附，最终限制在 0～1。

### 10.11 `Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h/.cpp`

无状态规则命名空间，定义默认 Gait/TurnBack 参数和安全 Delta 上限。

- `ResolveGaitBlendInterpSpeed(const FZZZAnimTuning*)`：空配置回退 2；非法/非正返回 0；合法值限制到最大 50。
- `ResolveTurnBackReverseInputDotThreshold(const FZZZAnimTuning*)`：空或非有限回退 -0.95；合法值限制到 [-1,0]。
- `ResolveGaitBlendTarget(EMovementGait)`：只有 Run 返回 1，其它返回 0；非法输入按 Walk 目标处理但不修改源值。

当前 TurnBackPhaseProcessor 使用规则默认阈值，没有调用 Tuning 阈值解析函数。

### 10.12 `Animation/zzzAnim/ZZZAnimInstance.h/.cpp`

**`UZZZAnimInstance`**：新版 ZZZ AnimInstance，负责管线主动驱动、快照上下文刷新、AnimBP 过渡条件和动画资产查询。

**字段**：`AnimSet`、`Tuning`、`StateMemory`、`AnimBlendX`、`AnimBlendY`、`AnimCurveVelocity`、`AnimCurveVelocityDirection`、`AnimCurveVelocityAngle`、`ActualVelocityDirection`、`ActualVelocityBlendX`、`ActualVelocityBlendY`、`ActualVelocityAngle`、`bCanYaw`、`bTurnBackSecondSegment`、保护快照 `Snap`；私有 Owner 弱指针、管线驱动标志、SnapshotCapture、Decisions、Events。AnimBlendX/Y、曲线速度方向、实际速度方向和逻辑 TurnBack CanYaw/第二段字段都由 Snapshot 在 `RefreshDecisionContext` 中同步，供 AnimBP 只读。

**函数**：

- `NativeInitializeAnimation()`：调用父类，并从 `TryGetPawnOwner` 缓存 BaseCharacter。
- `NativeUpdateAnimation(float)`：调用父类；上一帧由 PipelineDrive 驱动时清标记并跳过重复刷新，否则走降级刷新路径。
- `NativeThreadSafeUpdateAnimation(float)`：当前只调用父类，没有额外线程安全计算。
- `PipelineDrive(float)`：立即刷新快照/上下文/表现记忆，再设置 bDrivenByPipeline，供角色 Tick 末尾主动调用。
- `AnimNotify_CanYaw()`：接收动画中名为 `CanYaw` 的 AnimNotify，通过 `ABaseCharacter::NotifyCanYaw` 转发到运行时 Pipeline；不直接写动画快照或 RuntimeData。
- `RefreshDecisionContext(float)`：执行 Capture → 同步 `AnimBlendX/AnimBlendY`、`AnimCurveVelocity`/`AnimCurveVelocityDirection`/`AnimCurveVelocityAngle`、`ActualVelocityDirection`/`ActualVelocityBlendX`/`ActualVelocityBlendY`/`ActualVelocityAngle`、`bCanYaw` 和 `bTurnBackSecondSegment` → 构造读写 Context → 设置 Decisions/Events Context → 同步 MovingSubState → 推进 Stop/Gait 表现；TurnBack 时仅输出 Phase、CanYaw、第二段、输入点积和速度诊断。
- `Locomotion_NotMoving_To_Conduit()`：转发 Decisions 的 NotMoving 判定。
- `Locomotion_Stop_To_Conduit()`：转发 Stop 入口判定。
- `Locomotion_Conduit_To_EnterMove()`：转发非 Run 分支判定。
- `Locomotion_Conduit_To_Moving_Direct()`：转发 Run 直入分支判定。
- `Locomotion_Moving_To_Stop()`：转发 `ShouldExitMoving`。
- `Locomotion_EnterMove_To_Stop()`：转发 `ShouldStopMoving`。
- `Locomotion_WalkRun_To_TurnBack()`：转发 TurnBack 入口判定。
- `GetSeqByKey(FName)`：从 AnimSet.Sequences 查询动画序列，缺失返回空。
- `GetBlendSpaceByKey(FName)`：从 AnimSet.BlendSpaces 查询 BlendSpace，缺失返回空。

### 10.13 `Animation/zzzAnim/ZZZAnimLog.h/.cpp`

只声明和定义唯一的 `LogZZZAnim` 日志类别，没有其它运行时函数。

## 11. 编辑器/资产工具文件

### 11.1 `Public/Animation/AnimSyncMarkerTools.h` / `Private/Animation/AnimSyncMarkerTools.cpp`

**`UAnimSyncMarkerTools`**：编辑器用 `UBlueprintFunctionLibrary`，不参与运行时 Pipeline。

- `SetAuthoredSyncMarkers(UAnimSequence*, MarkerNames, Times)`：在 `WITH_EDITOR` 下按两个数组的较小长度清空并写入 AuthoredSyncMarkers，按时间排序，刷新同步标记缓存并标脏，返回实际写入数量；非编辑器返回 0。
- `BakeVelocityDirectionCurves(UAnimSequence*)`：通过 UE 5.8 Animation Data Model 读取 RM_PosX/RM_PosY，在 `GetNumberOfSampledKeys()`/`GetTimeAtFrame()` 采样帧上计算每帧水平位置增量的归一化方向；首帧为零，使用 `IAnimationDataController::AddCurve`/`SetCurveKeys` 覆盖真实 `RM_VelocityDirX`、`RM_VelocityDirY` FloatCurve，成功后标脏但不保存包。若静止序列缺少 RM_PosX/RM_PosY，则按其采样帧数写入全零方向曲线。
- 控制台命令 `ZZZBakeVelocityDirectionCurves <AnimSequenceObjectPath> [...]`：批量调用上述 Bake 入口；命令只标脏资产，执行后需通过编辑器保存资产。

### 11.2 `Public/SyncMarkerImporter.h` / `Private/SyncMarkerImporter.cpp`

**`USyncMarkerImporter`**：从 JSON 或目录导入 AnimSequence AuthoredSyncMarkers 的编辑器工具；cpp 另注册 `SyncMarkerImport` 控制台命令。

- `ImportSyncMarkersFromDirectory(Directory, AssetRoot, SyncGroupName)`：查找目录 JSON，逐个解析；按文件 basename 组成 UE Asset 路径并加载 AnimSequence；覆盖 AuthoredSyncMarkers、标脏并统计成功/无 Marker/未找到数量。SyncGroupName 当前只用于日志。
- `ImportSingleJson(JsonPath, AssetPath, SyncGroupName)`：解析单一 JSON，查找 AnimSequence，覆盖 Marker 并标脏；成功返回 true；SyncGroupName 不写入资产。
- `ParseMarkersFromJson(JsonObject, OutMarkers)`：查找 Type=AnimSequence 的对象，从 Properties.AuthoredSyncMarkers 读取 MarkerName/Time，写入输出数组，找到第一组后返回。

该工具和 `AnimSyncMarkerTools` 是资产处理/兼容入口，不是新版 ZZZAnim 运行时决策层。

## 12. 重要数据权威和写入关系

| 数据 | 当前权威写入方 | 读取方 |
|---|---|---|
| `RuntimeData.Arbiter` | `FArbiterPipeline::Process` 每帧先清零；`FGASArbiter::Arbitrate` 按 GameplayTag 写入；`FGYGOStateManager::ApplyLimitFlags` 在状态生命周期中写入限制位 | StateManager、Pipeline 计划和 Motion 命令构造 |
| `FInputData.CurrentFrame` | `FInputPipeline::Process` | Intent/Gait |
| `RuntimeData.View.ControlRotation` | `FViewRotationProcessor` | Locomotion/Movement |
| `RuntimeData.Intent` | Locomotion/Attack/Dodge Processors | Gait/State/Parameters |
| `RuntimeData.ZZZAnim.bShouldMove` | `FLocomotionIntentProcessor::Process` | SnapshotCapture/ZZZAnim locomotion |
| `RuntimeData.Gait.ResolvedGait` | `FGaitAuthorityProcessor` | Motion/ZZZAnim |
| `RuntimeData.ZZZAnim.AnimBlendX/AnimBlendY` | `FMovementParameterProcessor` | SnapshotCapture/UZZZAnimInstance/AnimBP 的方向 BlendSpace |
| `RuntimeData.RootMotion` | `FRootMotionParameterProcessor` | Motion/ZZZAnim diagnostics；`RootMotionDelta`、`AnimCurveVelocity`、最终 `AnimCurveVelocityDirection`、`AnimCurveYawDelta` 和 `bHasAuthoredVelocityDirection` 经 `BuildFramePlan` 桥接到 `FCharacterMovementCommand`；RM_VelocityDirX/Y authored 方向有效时优先，缺失时回退 RM_PosX/RM_PosY 差分；RootMotionDelta 与 `AnimCurveVelocity` 是动画曲线局部方向（X=前/Y=右）；普通 walkrun 无曲线时移动方向 = 玩家输入 `DesiredWorldMoveDir`，RM_Speed 只决定速度；TurnBack d0 在 CanYaw 前应用由累计 RM_Yaw 相邻采样差分得到的 AnimCurveYawDelta，并按当前 Bone_Root 相对入口的逆 yaw 修正方向保持世界直线，d1 段用玩家输入方向。`AnimCurveAngle` 保留为曲线方向角诊断。
| `RuntimeData.Movement.TurnBack` | `FTurnBackPhaseProcessor` 写入 Phase、`bCanYaw`、`bSecondSegment` 和 `ElapsedSeconds`；CanYaw 由 `UZZZAnimInstance::AnimNotify_CanYaw` 经 `ABaseCharacter`、RuntimeComponent、Pipeline 转发 | MotionDriver、ZZZAnim Snapshot；动画层只读 Phase/CanYaw/第二段，不反向写入 |
| `RuntimeData.State.CurrentState` | `FGYGOStateManager` | Animation/State consumers |
| `RuntimeData.ZZZAnim.CurrentState` | `FGYGOStateManager`，与最终主状态同步写入 | SnapshotCapture/ZZZAnim Decisions |
| `RuntimeData.Movement.CurrentSpeed` | `FMotionDriver::UpdateRuntimeData` | Base getter/ZZZAnim |
| `RuntimeData.Movement.bIsGrounded` | 当前无运行时写入方；模型默认值为 true | SnapshotCapture/基础角色查询 |
| `RuntimeData.ZZZAnim.VelocityLength` | `FMotionDriver::UpdateRuntimeData` | SnapshotCapture |
| `RuntimeData.ZZZAnim.ActualVelocityDirection`、`ActualVelocityBlendX/ActualVelocityBlendY`、`ActualVelocityAngle` | `FMotionDriver::UpdateRuntimeData` 在 Motion Commit 后从 Actor 水平 `Velocity` 派生；速度为零时清零 | `FZZZAnimSnapshotCapture` → `FZZZAnimSnapshot` → `UZZZAnimInstance`/AnimBP；ActualVelocityDirection 是世界单位方向，BlendX/Y 是相对 Actor 的 X=右/Y=前分量，Angle 为相对 Actor 角度 |
| `RuntimeData.ZZZAnim.Gait` | `FGaitAuthorityProcessor` | SnapshotCapture |
| `FZZZAnimSnapshot` | `FZZZAnimSnapshotCapture` | Decisions/Events |
| `FZZZAnimStateMemory` | `FZZZLocomotionEvents` | AnimInstance/AnimBP |
| `FStateUpdateResult` / `FStateTransitionEvent` | `FGYGOStateManager::Update` / `RecordTransitionEvent` | `FCharacterFramePlan::StateUpdate`、Pipeline 的状态结果消费；不重新提交状态 |
| `FCharacterFramePlan` | `FCharacterControlPipeline::BuildFramePlan` | 当前帧结果、状态报告和命令缓冲；不取代 RuntimeData canonical 写入 |
| `FCharacterFrameCommandBuffer` | `FCharacterControlPipeline::BuildFramePlan` | `CommitMovement` / `PublishAnimation` 的本帧真实提交命令 |
| `FCharacterMovementCommand` | `FCharacterControlPipeline::BuildFramePlan` | `FMotionDriver::Process`；消费 `bShouldMove`、`TurnBackPhase`、`bTurnBackSecondSegment`、`DesiredWorldMoveDir`、`AnimCurveYawDelta`、`AnimCurveVelocityDirection`、`RootMotionDelta` 和 RM_Speed；普通移动方向 = 玩家输入 `DesiredWorldMoveDir`、RM_Speed 决定速度；TurnBack d0 在 CanYaw 前用当前 Bone_Root 相对入口的逆 yaw 修正 dir，并应用由累计 RM_Yaw 差分得到的 `AnimCurveYawDelta`，同时关闭自动朝向；d1 段用玩家输入方向并恢复原有旋转配置；携带 `AnimCurveVelocity`、`bHasAuthoredVelocityDirection` 和来源标记供移动/诊断；普通零输入且非曲线收尾时由 MotionDriver 停速并不提交 `RequestDirectMove`；MotionDriver 回写实际速度，D0 写入 Actor yaw |
| `FCharacterAnimationPublishCommand` | `FCharacterControlPipeline::BuildFramePlan` | `FCharacterControlPipeline::PublishAnimation`；调用已有 `UZZZAnimInstance::PipelineDrive` |

## 13. 当前未实现、未接线和兼容项

以下是源码事实，不是待办承诺：

- 没有 C++ GameplayAbility 实现；移动由 `FMotionDriver` 完成，没有独立移动类；当前没有 Private 测试实现。以上能力对应的空脚手架目录已从源码树移除，需要时再新建。
- 当前玩家没有攻击/闪避 Enhanced Input 回调。
- `FActionArbiter`、`FHealthArbiter`、`FStaminaArbiter` 的核心业务仍有 TODO/空实现。
- AttributeSet 没有 Stamina 属性，`ClampStamina` 是空钩子。
- AttributeSet 没有死亡委托或死亡状态自动切换。
- `DefaultAbilities`/`DefaultEffects` 的应用函数存在，但 BeginPlay 当前不会自动调用。
- `FZZZAnimRuntimeModel.Velocity2DLength`、`LastInputDirectionAngle` 当前没有有效写入方；`AnimBlendX/AnimBlendY` 已由 `FMovementParameterProcessor` 写入并发布给 AnimBP。
- `FRuntimeData.Movement.bIsGrounded` 当前没有运行时写入方，保持模型默认值 true；SnapshotCapture 只读取该字段。
- `FZZZAnimSnapshot.bBlockMove` 当前由 Capture 固定写 false。
- `FZZZAnimTuning` 的 TurnBack 阈值、LoopBlendIn、OneShotBlendOut 当前没有完整消费链。
- `FRootMotionRuntimeModel.RootMotionDelta`、`AnimCurveDistanceDelta`、`AnimCurveYawDelta` 和 `AnimCurveAngle` 当前由参数处理器采样，其中 `RootMotionDelta`/`AnimCurveVelocityDirection` 通过移动命令作为固定根轨迹的方向和位移路径；TurnBack d0 的 `AnimCurveYawDelta` 由累计 RM_Yaw 相邻采样差分得到并由 MotionDriver 直接累加到 Actor，D0 同时关闭自动朝向，d1 恢复原有旋转配置并按输入方向移动；方向按当前 Bone_Root 相对入口的逆 yaw 修正；MotionDriver 使用 RM_Speed 驱动速度。RM_Dist 仍只作距离诊断。
- `FGYGOStateManager::DeactivateState` 注释描述了 GE 移除，但函数体当前没有真正移除状态施加的 GE。
- `FStateUpdateResult` 和 `FCharacterFrameCommandBuffer` 已接入 Pipeline，但命令缓冲当前只包含移动和 ZZZ 动画发布；状态结果是已完成操作报告，不会被 Pipeline 重放。
- `FCharacterFramePlan::ActionGranted` 仍只是 Arbiter 结果；没有 C++ GameplayAbility、攻击/闪避状态或 GA 命令执行链。
- 旧 `UNTEAnimInstance`、`Animation/Decisions/` 不作为新版 ZZZAnim 依据；当前 README 只记录当前存在的新版代码。
