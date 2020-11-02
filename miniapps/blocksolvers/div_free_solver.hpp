#include "mfem.hpp"
#include <memory>

using namespace std;
using namespace mfem;

/// Parameters for iterative solver
struct IterSolveParameters
{
   int print_level = 0;
   int max_iter = 500;
   double abs_tol = 1e-12;
   double rel_tol = 1e-9;
};

/// Parameters for the divergence free solver
struct DFSParameters : IterSolveParameters
{
   bool verbose = false;
   bool B_has_nullity_one = false;   // whether B has a 1D nullspace
   bool coupled_solve = false;       // whether to solve all unknowns together
   IterSolveParameters BBT_solve_param;
};

/// Data for the divergence free solver
struct DFSData
{
   Array<OperatorPtr> agg_hdivdof;    // agglomerates to H(div) dofs table
   Array<OperatorPtr> agg_l2dof;      // agglomerates to L2 dofs table
   Array<OperatorPtr> P_hdiv;         // Interpolation matrix for H(div) space
   Array<OperatorPtr> P_l2;           // Interpolation matrix for L2 space
   Array<OperatorPtr> P_hcurl;        // Interpolation for kernel space of div
   Array<OperatorPtr> Q_l2;           // Q_l2[l] = (W_{l+1})^{-1} P_l2[l]^T W_l
   Array<int> coarsest_ess_hdivdofs;  // coarsest level essential H(div) dofs
   Array<OperatorPtr> C;              // discrete curl: ND -> RT
   DFSParameters param;
};

/// Finite element spaces concerning divergence free solver.
/// The main usage of this class is to collect data needed for the solver.
class DFSSpaces
{
   RT_FECollection hdiv_fec_;
   L2_FECollection l2_fec_;
   ND_FECollection hcurl_fec_;
   L2_FECollection l2_0_fec_;

   unique_ptr<ParFiniteElementSpace> coarse_hdiv_fes_;
   unique_ptr<ParFiniteElementSpace> coarse_l2_fes_;
   unique_ptr<ParFiniteElementSpace> coarse_hcurl_fes_;
   unique_ptr<ParFiniteElementSpace> l2_0_fes_;

   unique_ptr<ParFiniteElementSpace> hdiv_fes_;
   unique_ptr<ParFiniteElementSpace> l2_fes_;
   unique_ptr<ParFiniteElementSpace> hcurl_fes_;

   Array<SparseMatrix> el_l2dof_;
   const Array<int>& ess_bdr_attr_;
   Array<int> all_bdr_attr_;

   int level_;
   DFSData data_;

   void MakeDofRelationTables(int level);
   void DataFinalize();
public:
   DFSSpaces(int order, int num_refine, ParMesh *mesh,
             const Array<int>& ess_attr, const DFSParameters& param);

   /** This should be called each time when the mesh (where the FE spaces are
       defined) is refined. The spaces will be updated, and the prolongation for
       the spaces and other data needed for the div-free solver are stored. */
   void CollectDFSData();

   const DFSData& GetDFSData() const { return data_; }
   ParFiniteElementSpace* GetHdivFES() const { return hdiv_fes_.get(); }
   ParFiniteElementSpace* GetL2FES() const { return l2_fes_.get(); }
};

/// Abstract solver class for Darcy's flow
class DarcySolver : public Solver
{
protected:
   Array<int> offsets_;
public:
   DarcySolver(int size0, int size1) : Solver(size0 + size1), offsets_(3)
   { offsets_[0] = 0; offsets_[1] = size0; offsets_[2] = height; }
   virtual int GetNumIterations() const = 0;
};

/// Solver for B * B^T
/// Compute the product B * B^T and solve it with CG preconditioned by BoomerAMG
class BBTSolver : public Solver
{
   OperatorPtr BBT_;
   OperatorPtr BBT_prec_;
   CGSolver BBT_solver_;
   bool B_has_nullity_one_;
public:
   BBTSolver(const HypreParMatrix &B, bool B_has_nullity_one=false,
             IterSolveParameters param=IterSolveParameters());
   virtual void Mult(const Vector &x, Vector &y) const;
   virtual void SetOperator(const Operator &op) { }
};

/// Block diagonal solver for A, each block is inverted by direct solver
class SymBlkDiagSolver : public BlockDiagSolver
{
public:
   SymBlkDiagSolver(const SparseMatrix& A, const SparseMatrix& block_dof)
      : BlockDiagSolver(A, block_dof) { }
   virtual void MultTranspose(const Vector &x, Vector &y) const { Mult(x, y); }
};

/// Solver for local problems in SchwarzSmoother
class LocalSolver : public Solver
{
   DenseMatrix local_system_;
   DenseMatrixInverse local_solver_;
   const int offset_;
public:
   LocalSolver(const DenseMatrix &M, const DenseMatrix &B);
   virtual void Mult(const Vector &x, Vector &y) const;
   virtual void SetOperator(const Operator &op) { }
};

/// non-overlapping additive Schwarz for saddle point problems
class SaddleSchwarzSmoother : public Solver
{
   const SparseMatrix& agg_hdivdof_;
   const SparseMatrix& agg_l2dof_;
   OperatorPtr coarse_l2_projector_;

   Array<int> offsets_;
   mutable Array<int> offsets_loc_;
   mutable Array<int> hdivdofs_loc_;
   mutable Array<int> l2dofs_loc_;
   Array<OperatorPtr> solvers_loc_;
public:
   SaddleSchwarzSmoother(const HypreParMatrix& M,
                         const HypreParMatrix& B,
                         const SparseMatrix& agg_hdivdof,
                         const SparseMatrix& agg_l2dof,
                         const HypreParMatrix& P_l2,
                         const HypreParMatrix& Q_l2);
   virtual void Mult(const Vector &x, Vector &y) const;
   virtual void MultTranspose(const Vector &x, Vector &y) const { Mult(x, y); }
   virtual void SetOperator(const Operator &op) { }
};

/** This smoother does relaxations on an auxiliary space (determined by a map
    from the original space to the auxiliary space provided by the user).
    For example, the space can be the nullspace of div/curl, in which case the
    smoother can be used to construct a Hiptmair smoother. */
class AuxSpaceSmoother : public Solver
{
   OperatorPtr aux_map_;
   OperatorPtr aux_system_;
   OperatorPtr aux_smoother_;
   void Mult(const Vector &x, Vector &y, bool transpose) const;
public:
   AuxSpaceSmoother(const HypreParMatrix &op, HypreParMatrix *aux_map,
                    bool own_aux_map = false);
   virtual void Mult(const Vector &x, Vector &y) const { Mult(x, y, false); }
   virtual void MultTranspose(const Vector &x, Vector &y) const { Mult(x, y, true); }
   virtual void SetOperator(const Operator &op) { }
   HypreSmoother& GetSmoother() { return *aux_smoother_.As<HypreSmoother>(); }
};

/** Divergence free solver.
    The basic idea of the solver is to exploit a multilevel decomposition of
    Raviart-Thomas space to find a particular solution satisfying the divergence
    constraint, and then solve the remaining (divergence-free) component in the
    kernel space of the discrete divergence operator.

    For more details see
    1. Vassilevski, Multilevel Block Factorization Preconditioners (Appendix F.3),
       Springer, 2008.
    2. Voronin, Lee, Neumuller, Sepulveda, Vassilevski, Space-time discretizations
       using constrained first-order system least squares (CFOSLS).
       J. Comput. Phys. 373: 863-876, 2018. */
class DivFreeSolver : public DarcySolver
{
   const DFSData& data_;
   OperatorPtr BT_;
   BBTSolver BBT_solver_;
   Array<Array<int>> ops_offsets_;
   Array<BlockOperator*> ops_;
   Array<BlockOperator*> blk_Ps_;
   Array<Solver*> smoothers_;
   OperatorPtr prec_;
   OperatorPtr solver_;

   void SolveParticular(const Vector& rhs, Vector& sol) const;
   void SolveDivFree(const Vector& rhs, Vector& sol) const;
   void SolvePotential(const Vector &rhs, Vector& sol) const;
public:
   DivFreeSolver(const HypreParMatrix& M, const HypreParMatrix &B,
                 const DFSData& data);
   virtual void Mult(const Vector & x, Vector & y) const;
   virtual void SetOperator(const Operator &op) { }
   virtual int GetNumIterations() const
   {
      return solver_.As<IterativeSolver>()->GetNumIterations();
   }
};

/// Wrapper for the block-diagonal-preconditioned MINRES defined in ex5p.cpp
class BDPMinresSolver : public DarcySolver
{
   BlockOperator op_;
   BlockDiagonalPreconditioner prec_;
   OperatorPtr BT_;
   OperatorPtr S_;   // S_ = B diag(M)^{-1} B^T
   MINRESSolver solver_;
   Array<int> ess_zero_dofs_;
public:
   BDPMinresSolver(HypreParMatrix& M, HypreParMatrix& B,
                   IterSolveParameters param);
   virtual void Mult(const Vector & x, Vector & y) const;
   virtual void SetOperator(const Operator &op) { }
   void SetEssZeroDofs(const Array<int>& dofs) { dofs.Copy(ess_zero_dofs_); }
   const BlockOperator& GetOperator() const { return op_; }
   virtual int GetNumIterations() const { return solver_.GetNumIterations(); }
};