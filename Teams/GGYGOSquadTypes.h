/**
 * @file GGYGOSquadTypes.h
 * @brief 队伍层的公共常量
 */
#pragma once

#include "CoreMinimal.h"

/**
 * 一支队伍的位置数量上限。
 *
 * 运行时的实际队伍规模由玩法配置（`UGGYGOExperienceDefinition::SquadMembers`）决定，
 * 本常量只作上限校验与复制数组的预分配依据。
 *
 * 为什么需要一个上限而不是完全动态：每个位置带一个 ASC，配置写错时的代价是
 * 成倍的复制开销（4 人房 × 每队 N 个 ASC）。有上限时越界配置在装配阶段就被拒绝，
 * 而不是变成一份跑起来才发现的网络账单。
 */
#define GGYGO_MAX_SQUAD_SIZE 4
