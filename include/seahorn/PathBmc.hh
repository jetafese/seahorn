#pragma once
#include "seahorn/config.h"

#include "seahorn/Analysis/CutPointGraph.hh"
#include "seahorn/Expr/Expr.hh"
#include "seahorn/Expr/Smt/Solver.hh"
#include "seahorn/LegacyOperationalSemantics.hh"
#include "seahorn/Support/SeaAssert.h"
#include "seahorn/Support/SeaLog.hh"
#include "seahorn/Bmc.hh"

#include "llvm/ADT/SmallVector.h"
#include <vector>

namespace llvm {
class TargetLibraryInfo;
class TargetLibraryInfoWrapperPass;
class DataLayout;
class raw_ostream;
} // namespace llvm
namespace seadsa {
class ShadowMem;
class SeaMemorySSA;
}

namespace seahorn {
class PathBmcEngine;
using PathBmcTrace = BmcTrace<PathBmcEngine, solver::Solver::model_ref>;
namespace solver {
class Model;
} // end namespace solver
} // end namespace seahorn

#ifndef HAVE_CLAM
namespace seahorn {

/* Dummy class for PathBmcEngine */
class PathBmcEngine {
  LegacyOperationalSemantics &m_sem;
  llvm::SmallVector<const CutPoint *, 8> m_cps;
  llvm::SmallVector<const CpEdge *, 8> m_cp_edges;
  std::vector<SymStore> m_states;
  ExprVector m_side;

public:
  PathBmcEngine(seahorn::LegacyOperationalSemantics &sem,
                llvm::TargetLibraryInfoWrapperPass &tli, seadsa::ShadowMem &sm)
      : m_sem(sem) {}

  virtual ~PathBmcEngine() {}

  void addCutPoint(const CutPoint &cp) {}

  void encode() {}

  virtual solver::SolverResult solve();

  PathBmcTrace getTrace();

  raw_ostream &toSmtLib(raw_ostream &out) { return out; }

  solver::SolverResult result() { return solver::SolverResult::UNKNOWN; }

  LegacyOperationalSemantics &sem() { return m_sem; }

  ExprFactory &efac() { return m_sem.efac(); }
  
  const SmallVector<const CutPoint *, 8> &getCps() const { return m_cps; }

  const SmallVector<const CpEdge *, 8> &getEdges() const { return m_cp_edges; }

  std::vector<SymStore> &getStates() { return m_states; }

  Expr getSymbReg(const llvm::Value &v) { return Expr(); }

  const ExprVector &getFormula() const { return m_side; }
};
} // namespace seahorn
#else

#include "clam/Clam.hh"
#include "seahorn/LiveSymbols.hh"

#include <memory>
#include <queue>
#include <unordered_set>

namespace clam {
class CrabBuilderManager;
} // namespace clam

/*
  Instead of building a monolithic precise encoding of the program and
  check its satisfiability, this BMC engine enumerates symbolically
  all paths. This enumeration continues until a path is satisfiable or
  no more paths exist.
 */
namespace seahorn {

class PathBmcEngine {
public:
  PathBmcEngine(LegacyOperationalSemantics &sem,
                llvm::TargetLibraryInfoWrapperPass &tli, seadsa::ShadowMem &sm);

  PathBmcEngine(const PathBmcEngine &engine) = delete;
  
  virtual ~PathBmcEngine();

  void addCutPoint(const CutPoint &cp);

  /// Enumerate paths until a path is satisfiable or there is no
  /// more paths.
  virtual solver::SolverResult solve();

  /// Returns the BMC trace (if available)
  PathBmcTrace getTrace();

  /// Output the precise encoding generated by the encode method in
  /// SMT-LIB2 format
  raw_ostream &toSmtLib(raw_ostream &out);

  /// returns the latest result from solve()
  solver::SolverResult result() { return m_result; }

  /// return the operational semantics
  LegacyOperationalSemantics &sem() {
    return static_cast<LegacyOperationalSemantics &>(m_sem);
  }

  /// return Expression factory
  ExprFactory &efac() { return m_sem.efac(); }

  /// get cut-point trace
  const SmallVector<const CutPoint *, 8> &getCps() const { return m_cps; }

  /// get edges from the cut-point trace
  const SmallVector<const CpEdge *, 8> &getEdges() const { return m_edges; }

  /// get symbolic states corresponding to the cutpoint trace
  std::vector<SymStore> &getStates() { return m_states; }

  Expr getSymbReg(const llvm::Value &v) {
    Expr reg;
    if (m_semCtx) {
      return m_sem.getSymbReg(v, *m_semCtx);
    }
    return reg;
  }

  const ExprVector &getFormula() const { return m_precise_side; }

  
protected:
  /// symbolic operational semantics
  OperationalSemantics &m_sem;
  /// context for OperationalSemantics
  OpSemContextPtr m_semCtx;
  /// cut-point trace
  SmallVector<const CutPoint *, 8> m_cps;
  /// symbolic states corresponding to m_cps
  std::vector<SymStore> m_states;
  /// edge-trace corresponding to m_cps
  SmallVector<const CpEdge *, 8> m_edges;
  // cutpoint graph for m_fn
  const CutPointGraph *m_cpg;
  // the function
  const llvm::Function *m_fn;
  // live symbols
  std::unique_ptr<LiveSymbols> m_ls;
  // symbolic store
  SymStore m_ctxState;
  /// precise encoding of m_cps
  ExprVector m_precise_side;

  // solver used to enumerate paths from the boolean abstraction
  std::unique_ptr<solver::Solver> m_boolean_solver;
  // solver used to solve a path formula over arrays, bitvectors, etc
  std::unique_ptr<solver::Solver> m_smt_path_solver;
  // model of a path formula
  solver::Solver::model_ref m_model;
  /// last result of the main solver (m_boolean_solver)
  solver::SolverResult m_result;

  // generalized path to be excluded from the Boolean abstraction
  ExprVector m_gen_path;
  // sanity check: bookeeping of all generated blocking clauses.
  ExprSet m_blocking_clauses;

  // queue for unsolved path formulas
  std::queue<std::pair<unsigned, ExprVector>> m_unsolved_path_formulas;
  // count number of path
  unsigned m_num_paths;

  //// Crab stuff
  llvm::TargetLibraryInfoWrapperPass &m_tli;
  // shadow mem pass
  seadsa::ShadowMem &m_sm;
  seadsa::SeaMemorySSA *m_mem_ssa;
  // crab's cfg builder manager
  std::unique_ptr<clam::CrabBuilderManager> m_cfg_builder_man;
  // crab instance to solve paths
  std::unique_ptr<clam::IntraClam> m_crab_path_solver;

  /****************** Helpers ****************/
  using expr_invariants_map_t = DenseMap<const BasicBlock *, ExprVector>;
  using crab_invariants_map_t = clam::IntraClam::abs_dom_map_t;

  /// Construct the precise (monolithic) encoding. The result is
  /// stored in m_precise_side.
  void encode();

  /// Check for satisfiability of the boolean abstraction kept in
  /// m_boolean_solver. The result is stored in m_result.
  void solveBoolAbstraction();

  /// Refine the boolean abstraction by removing a generalization of
  /// the last visited path which is already in m_gen_path. Return
  /// false if some error happened.
  bool refineBoolAbstraction();

  /// Check feasibility of a path induced by trace using SMT solver.
  /// Return true (sat), false (unsat), or indeterminate (inconclusive).
  /// If unsat then it produces a blocking clause stored in m_gen_path.
  template<class BmcTrace>
  solver::SolverResult
  solvePathWithSmt(const BmcTrace &trace,
		   const expr_invariants_map_t &invariants,
		   const expr_invariants_map_t &path_constraints);

  /// Check feasibility of a path induced by trace using abstract
  /// interpretation.
  /// Return true (sat) or false (unsat). If unsat then it produces a
  /// blocking clause.
  ///
  /// If keep_path_constraints then path_constraints contains the
  /// post-state produced for each block along the cex.
  template<class BmcTrace>  
  bool
  solvePathWithCrab(BmcTrace &trace, bool keep_path_constraints,
		    crab_invariants_map_t &crab_path_constraints,
		    expr_invariants_map_t &path_constraints);

  /// Encode the cex generated by Crab as a conjunction of Boolean
  /// literals that represents the path.
  template<class BmcTrace>    
  bool encodeBoolPathFromCrabCex(BmcTrace &cex,
				 const std::vector<clam::statement_t *> &cex_stmts,
				 ExprSet &path);

  /// Given a sequence of basic blocks, extract the invariants per
  /// block and convert them to Expr's
  void extractPostConditionsFromCrabCex(
      const std::vector<const llvm::BasicBlock *> &cex,
      const crab_invariants_map_t &invariants, expr_invariants_map_t &out);
  
  // Build Crab CFG, run pre-analyses, etc
  void initializeCrab();
  
  // Run Crab on the whole program and assert invariants them as
  // implications (bb_i => inv_i) in the precise encoding.
  void addWholeProgramCrabInvariants(expr_invariants_map_t &invariants);

  /// out contains all invariants (per block) inferred by crab.
  void loadCrabInvariants(const clam::IntraClam &analysis,
                          DenseMap<const BasicBlock *, ExprVector> &out);

  /// Add the crab invariants in m_side after applying the symbolic store s.
  void assertCrabInvariants(const expr_invariants_map_t &invariants,
                            SymStore &s);

  /// Evaluate an expression using the symbolic store
  /// Needed when Crab adds blocking clauses into the boolean abstraction.
  /// Assume that encode() has been executed already.
  Expr eval(Expr e);

  /// For debugging
  void toSmtLib(const ExprVector &path, std::string prefix = "");
};

} // end namespace seahorn
#endif
