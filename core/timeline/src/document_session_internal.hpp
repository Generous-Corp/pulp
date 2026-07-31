#pragma once

#include <pulp/timeline/document_session.hpp>

namespace pulp::timeline::detail {

// Internal control-side preview used by adapters that must validate an output
// boundary before publishing a history operation. The returned candidate is
// reduced from the current snapshot without changing revision or undo state.
class DocumentSessionPreviewAccess {
  public:
    static runtime::Result<ReducedTransaction, TransactionError>
    undo(const DocumentSession& session);
    static runtime::Result<ReducedTransaction, TransactionError>
    redo(const DocumentSession& session);

  private:
    enum class Direction { Undo, Redo };

    static runtime::Result<ReducedTransaction, TransactionError>
    preview(const DocumentSession& session, Direction direction);
};

} // namespace pulp::timeline::detail
