//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Treenode definitions for CREATE INDEX statements.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/pt_create_index.h"

#include "yb/client/client.h"

#include "yb/yql/cql/ql/ptree/sem_context.h"

namespace yb {
namespace ql {

using std::shared_ptr;
using std::to_string;
using client::YBColumnSchema;
using client::YBSchema;
using client::YBTableName;

//--------------------------------------------------------------------------------------------------

PTCreateIndex::PTCreateIndex(MemoryContext *memctx,
                             YBLocation::SharedPtr loc,
                             const bool is_unique,
                             const MCSharedPtr<MCString>& name,
                             const PTQualifiedName::SharedPtr& table_name,
                             const PTListNode::SharedPtr& columns,
                             const bool create_if_not_exists,
                             const PTTablePropertyListNode::SharedPtr& ordering_list,
                             const PTListNode::SharedPtr& covering)
    : PTCreateTable(memctx, loc, table_name, columns, create_if_not_exists, ordering_list),
      is_unique_(is_unique),
      name_(name),
      covering_(covering),
      is_local_(false),
      column_descs_(memctx),
      column_definitions_(memctx) {
}

PTCreateIndex::~PTCreateIndex() {
}

CHECKED_STATUS PTCreateIndex::Analyze(SemContext *sem_context) {
  // Look up indexed table.
  bool is_system_ignored;
  RETURN_NOT_OK(relation_->AnalyzeName(sem_context, OBJECT_TABLE));
  RETURN_NOT_OK(sem_context->LookupTable(relation_->ToTableName(), relation_->loc(),
                                         true /* write_table */, &table_, &is_system_ignored,
                                         &column_descs_, &num_key_columns_, &num_hash_key_columns_,
                                         &column_definitions_));

  // Save context state, and set "this" as current create-table statement in the context.
  SymbolEntry cached_entry = *sem_context->current_processing_id();
  sem_context->set_current_create_table_stmt(this);

  // Analyze index table like a regular table for the primary key definitions.
  RETURN_NOT_OK(PTCreateTable::Analyze(sem_context));

  // Add remaining primary key columns from the indexed table. For non-unique index, add the columns
  // to the primary key of the index table to make the non-unique values unique. For unique index,
  // they should be added as non-primary-key columns.
  const YBSchema& schema = table_->schema();
  for (int idx = 0; idx < num_key_columns_; idx++) {
    const MCString col_name(schema.Column(idx).name().c_str(), sem_context->PTempMem());
    PTColumnDefinition* col = sem_context->GetColumnDefinition(col_name);
    if (!col->is_primary_key()) {
      if (!is_unique_) {
        RETURN_NOT_OK(AppendPrimaryColumn(sem_context, col));
      } else {
        RETURN_NOT_OK(AppendColumn(sem_context, col));
      }
    }
  }

  // Add covering columns.
  if (covering_ != nullptr) {
    RETURN_NOT_OK((covering_->Apply<SemContext, PTName>(sem_context,
                                                        &PTName::SetupCoveringIndexColumn)));
  }

  // Check whether the index is local, i.e. whether the hash keys match (including being in the
  // same order).
  is_local_ = true;
  if (num_hash_key_columns_ != hash_columns_.size()) {
    is_local_ = false;
  } else {
    int idx = 0;
    for (const auto& column : hash_columns_) {
      if (column->yb_name() != column_descs_[idx].name()) {
        is_local_ = false;
        break;
      }
      idx++;
    }
  }

  // Verify transactions and consistency settings.
  TableProperties table_properties;
  RETURN_NOT_OK(ToTableProperties(&table_properties));
  if (table_->InternalSchema().table_properties().is_transactional()) {
    if (!table_properties.is_transactional()) {
      return sem_context->Error(this,
                                "Transactions must be enabled in an index of a "
                                "transactions-enabled table.",
                                ErrorCode::INVALID_TABLE_DEFINITION);
    }
    if (table_properties.consistency_level() == YBConsistencyLevel::USER_ENFORCED) {
      return sem_context->Error(this,
                                "User-enforced consistency level not allowed in a "
                                "transactions-enabled index.",
                                ErrorCode::INVALID_TABLE_DEFINITION);
    }
  } else {
    if (table_properties.is_transactional()) {
      return sem_context->Error(this,
                                "Transactions cannot be enabled in an index of a table without "
                                "transactions enabled.",
                                ErrorCode::INVALID_TABLE_DEFINITION);
    }
    if (table_properties.consistency_level() != YBConsistencyLevel::USER_ENFORCED) {
      return sem_context->Error(this,
                                "Consistency level must be user-enforced in an index without "
                                "transactions enabled.",
                                ErrorCode::INVALID_TABLE_DEFINITION);
    }
  }

  // TODO: create local index when co-partition table is available.
  if (is_local_) {
    LOG(WARNING) << "Creating local secondary index " << yb_table_name().ToString()
                 << " as global index.";
    is_local_ = false;
  }

  // Restore the context value as we are done with this table.
  sem_context->set_current_processing_id(cached_entry);
  if (VLOG_IS_ON(3)) {
    PrintSemanticAnalysisResult(sem_context);
  }

  return Status::OK();
}

void PTCreateIndex::PrintSemanticAnalysisResult(SemContext *sem_context) {
  PTCreateTable::PrintSemanticAnalysisResult(sem_context);
}

Status PTCreateIndex::ToTableProperties(TableProperties *table_properties) const {
  table_properties->SetTransactional(true);
  return PTCreateTable::ToTableProperties(table_properties);
}

}  // namespace ql
}  // namespace yb
