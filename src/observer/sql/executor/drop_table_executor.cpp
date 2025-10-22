#include "sql/executor/drop_table_executor.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "sql/stmt/drop_table_stmt.h"
#include "session/session.h"
#include "storage/db/db.h"
RC DropTableExecutor::execute(SQLStageEvent *event) {
  SessionEvent *session_event = event->session_event();
  Session      *session       = session_event->session();
  ASSERT(event->stmt()->type() == StmtType::DROP_TABLE , "drop table executor can not run this command: %d", static_cast<int>(event->stmt()->type()));
  auto db = session->get_current_db();
  auto stmt = static_cast<DropTableStmt*>(event->stmt());
  return db->drop_table(stmt->table_name());
}