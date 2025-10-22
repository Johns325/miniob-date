#pragma once

class SQLStageEvent;
#include "common/sys/rc.h"

class DropTableExecutor {
 public:
  DropTableExecutor() = default;
  ~DropTableExecutor() = default;
  RC execute(SQLStageEvent *event);
};