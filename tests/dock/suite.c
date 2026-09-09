/*
 * dock — does this process learn that the console was docked?
 *
 * ONE CASE, AND ITS OWN .nro, FOR TWO REASONS THAT BOTH HOLD.
 *
 * It needs a console. The operator has to see the mode change happen
 * while they are holding the console, and consoleUpdate() is also what
 * makes libnx refresh the window dimensions this case reads — so it
 * cannot join `display`, which owns the display and starts no console.
 *
 * And it is driven by a person. It watches for sixty seconds and has no
 * pass condition on anybody doing anything; a run where nobody touches
 * the console records that and says so. Putting a minute of waiting for
 * a human in front of automated cases would make every suite it joined
 * something you cannot leave running.
 *
 * Copyright (c) mesa-nvk-horizon contributors
 * SPDX-License-Identifier: MIT
 */
#include "common/testfw.h"

TEST_CASE_DECL(dock, mode_change);

/* A console, and it is the point: the operator has to see this happen
 * while they are holding the console. */
TEST_SUITE("dock", false,
           TEST_CASE(dock, mode_change));
