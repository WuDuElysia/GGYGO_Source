# GGYGO Source

模块速查：目录职责、关键类、以及改动前需要知道的架构约束。

**设计理由不写在这里**，写在各文件的头注释里（为什么这样拆、改了会坏什么）。
这份文档只回答"东西在哪、谁负责什么"。

## 项目形态

多角色队伍制动作游戏。一名玩家同时拥有若干角色，随时切换出战。
架构是 Lyra 风格的组件化 + GAS：角色类只负责把组件接起来，
具体职责由组件与 GAS 承担。

## 模块布局

`Source/GGYGO` 没有 `Public/` / `Private/` 划分，头文件与 `.cpp` 按职责子目录并列，
`ModuleDirectory` 是唯一 include 根。

| 目录 | 内容 |
|---|---|
| `AbilitySystem/` | GAS 核心层。ASC、AbilitySet、Tag 关系表、GE 上下文 |
| `AbilitySystem/Abilities/` | 能力基类、能力代价、失败消息 |
| `AbilitySystem/Attributes/` | `UGGYGOAttributeSet`（基类）、`UGGYGOHealthSet`、`UGGYGOCombatSet` |
| `AbilitySystem/Cues/` | Cue 管理器与命中 Cue |
| `AbilitySystem/Executions/` | 伤害计算 |
| `AbilitySystem/Groups/` | 能力组并发规则 |
| `AbilitySystem/Tasks/` | Montage + 事件等待任务 |
| `Animation/` | `AnimSyncMarkerTools`（同步标记工具） |
| `Animation/zzzAnim/` | ZZZ 动画层。`Data/` 数据、`Capture/` 快照采集、`Locomotion/` 过渡判定 |
| `Camera/` | 相机组件与相机模式栈 |
| `Character/` | `AGGYGOCharacterBase`、`AGGYGOHeroCharacter` |
| `Character/Components/` | CMC、生命、输入（Hero）、初始化协调（PawnExtension）、动画曲线采样 |
| `Character/Data/` | `UGGYGOPawnData`、移动参数集与移动类型 |
| `Combat/HitDetection/` | 近战判定组件 |
| `GameModes/` | GameMode 与 Experience 定义 |
| `Input/` | InputConfig 与 `UEnhancedInputComponent` 派生 |
| `Messages/` | 跨系统消息载荷 |
| `Physics/` | 带 Tag 的物理材质 |
| `Player/` | PlayerController、PlayerState、LocalPlayer |
| `System/` | AssetManager、GameData、GameplayTags |
| `Teams/` | 队伍位置、编队组件、编队预设存档 |

`Development/` 与 `Feedback/` 目前是空目录（只有 `.gitkeep`）。

## 架构约束

改动这些地方前先确认影响。它们的共同点是**出错时不报错，只是行为静默不对**。

### ASC 挂在队伍位置上，不在 Pawn 上

`AGGYGOCharacterSlot`（`AInfo` 派生）持有该角色的 ASC 与 `HealthSet` / `CombatSet`，
都是默认子对象。Pawn 只是这个 ASC 的 Avatar。
所以 `AGGYGOCharacterBase::GetAbilitySystemComponent()` **在注入完成前返回 nullptr**。

这样待命角色的冷却、Buff、血量与 Pawn 的显隐甚至是否存在都无关。

**位置的 Owner 必须是 PlayerController**：GAS 靠 `GetOwnerActor()->GetNetOwningPlayer()`
找玩家连接。设错时预测键生成不出来，所有 `LocalPredicted` 能力退化为纯服务器执行
（输入延迟一个 RTT），且不报任何错。

位置用 `bAlwaysRelevant` + `Mixed` 复制模式。相关性是全有或全无的，
用 `bOnlyRelevantToOwner` 会让旁观者收不到该角色的 Cue 与 Tag。

### 编队分三层

| 层 | 是什么 | 在哪 | 生命周期 |
|---|---|---|---|
| 编队预设 | 玩家存的 N 套编队 + 出战选择 | `UGGYGOSquadPresets` | 落盘，跨关卡 |
| 编队名单 | 这一局带谁 | `UGGYGOSquadComponent::Roster` | 一局，装配后锁定 |
| 位置 | ASC 与属性集的宿主 | `Slots[]` | 一局 |

装配取名单按三级回落：已设好的 `Roster` → 本地玩家存档里的出战编队 →
`UGGYGOExperienceDefinition::SquadMembers`（默认编队）。全空则报错不生成。

编队预设**必须**经 `UGGYGOLocalPlayer::GetSquadPresets()` 取。
`LoadOrCreateSaveGameForLocalPlayer` 每次调用返回新对象，
绕过缓存会让各调用方各持一份副本互相覆盖，症状是"编好的队不生效"且不报错。

存档里成员存 `FPrimaryAssetId`。因此 `GGYGOPawnData` 必须注册为 PrimaryAssetType
且 `CookRule=AlwaysCook`（见 `DefaultGame.ini`）——存档里的 Id 不构成资产引用，
没有硬引用链会把角色带进包。

### 角色分两类

`AGGYGOCharacterBase` 是所有角色（含 AI）的底座，挂 PawnExtension 与 Health 两个组件。
`AGGYGOHeroCharacter` 在其上多挂 `UGGYGOHeroComponent`（输入）与 `UGGYGOCameraComponent`。

分开的原因：HeroComponent 是 InitState 上的一个 feature，等不到 PlayerController 就会卡住，
而它卡住会让**整个角色**的初始化停住。AI 用基类就不会带这个组件。

相机挂角色而不是 Controller，因为换人时镜头参数应该跟着角色走。

### 属性集拆成承受侧与输出侧

`UGGYGOHealthSet` 是目标侧（生命、韧性，以及 Damage / Healing / PoiseDamage 元属性），
`UGGYGOCombatSet` 是来源侧（BaseDamage / BaseHeal / BasePoiseDamage）。

拆分不是形式主义：ExecutionCalculation 要同时捕获"源的 CombatSet"与"目标的 HealthSet"，
混在一个 Set 里就无法区分同一个 Set 是作为源还是目标被捕获。

元属性链路是 Execution 只写元属性 → `PostGameplayEffectExecute` 转成 Health/Poise 增减
→ Clamp → 广播 → 清零。

### 动画层单向依赖

ZZZ 动画层只读 CMC，通过 Snapshot 消费逻辑结果。
唯一的反向通道是 AnimNotify，且只发事件不写状态。
AnimBP / AnimInstance 不作为任何逻辑字段的权威写入者。

## 装配流程

`AGGYGOGameMode` 接管角色生成，不调 `Super::HandleStartingNewPlayer`。

装配前先等 GameFeature 插件激活完成（`PendingGameFeatureCount` 归零）。
插件里的 `GameFeatureAction` 可能往角色类注入组件或授予能力，那些动作在激活完成时才执行，
早生成的角色会缺内容且不报错。插件列表为空时计数恒为零，装配路径不变。
插件加载失败也照常放行，只报错 —— 一个装不上的插件不该让所有玩家卡在没有角色的状态。

分两阶段：先为名单里每份 PawnData 建位置（此时属性与能力已就绪），
再生成 Pawn 并 `InitializeAbilitySystem(位置的 ASC, 位置)` + `Slot->SetAvatar(Pawn)`。

顺序反过来就会遇到"属性集晚于读它的组件"。

全员生成在同一出生点，`SpawnCollisionHandling` 用 `AdjustIfPossibleButAlwaysSpawn`，
否则第二三个成员会因重叠生成失败。非出战成员立刻隐藏关碰撞。

换人是转移 Controller 的附身目标，顺序为先 `UnPossess` 旧的再 `Possess` 新的。

## 当前缺口

- 编成面板未做。开发期入口是控制台命令 `GGYGOSetSquadMember` /
  `GGYGOSetActiveSquadPreset` / `GGYGODumpSquadPresets` / `GGYGOClearSquadPresets`
- 联机时远程玩家的编队上报未做。编队存在客户端自己的磁盘上，服务器读不到，
  目前远程玩家走默认编队
- 队伍级 ASC 未建，等出现真正的共享属性再说
- `UGGYGOGameplayAbility` 只有抽象基类，没有具体派生类
- 联机预测与多客户端行为尚未实测

## 相关文档

架构设计与实施计划在 Obsidian 笔记 `lyra学习笔记/GGYGO架构规划/`
（`计划蓝图.md`、`模块参考.md` 与若干 canvas 分图），不在本仓库内。
