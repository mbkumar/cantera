//! @file IDA_Solver.cpp

// This file is part of Cantera. See License.txt in the top-level directory or
// at https://cantera.org/license.txt for license and copyright information.

#include "cantera/numerics/IDA_Solver.h"
#include "cantera/numerics/NumUtil.h"
#include "cantera/base/stringUtils.h"

#include "sundials/sundials_types.h"
#include "sundials/sundials_math.h"
#include "idas/idas.h"
#if CT_SUNDIALS_VERSION >= 30
    #if CT_SUNDIALS_USE_LAPACK
        #include "sunlinsol/sunlinsol_lapackdense.h"
        #include "sunlinsol/sunlinsol_lapackband.h"
    #else
        #include "sunlinsol/sunlinsol_dense.h"
        #include "sunlinsol/sunlinsol_band.h"
    #endif
    #include "sunlinsol/sunlinsol_spgmr.h"
    #include "idas/idas_direct.h"
    #include "idas/idas_spils.h"
#else
    #include "idas/idas_direct.h"
    #include "idas/idas_dense.h"
    #include "idas/idas_spgmr.h"
    #include "idas/idas_band.h"
#endif
#include "nvector/nvector_serial.h"

using namespace std;

#if CT_SUNDIALS_VERSION < 25
typedef int sd_size_t;
#else
typedef long int sd_size_t;
#endif

namespace Cantera
{

//! A simple class to hold an array of parameter values and a pointer to an
//! instance of a subclass of ResidEval.
class ResidData
{
public:
    ResidData(ResidJacEval* f, IDA_Solver* s, int npar = 0) {
        m_func = f;
        m_solver = s;
    }

    virtual ~ResidData() {
    }

    ResidJacEval* m_func;
    IDA_Solver* m_solver;
};
}

extern "C" {
    //! Function called by IDA to evaluate the residual, given y and ydot.
    /*!
     * IDA allows passing in a void* pointer to access external data. Instead of
     * requiring the user to provide a residual function directly to IDA (which
     * would require using the sundials data types N_Vector, etc.), we define
     * this function as the single function that IDA always calls. The real
     * evaluation of the residual is done by an instance of a subclass of
     * ResidEval, passed in to this function as a pointer in the parameters.
     *
     * FROM IDA WRITEUP -> What the IDA solver expects as a return flag from its
     * residual routines:
     *
     * A IDAResFn res should return a value of 0 if successful, a positive value
     * if a recoverable error occured (e.g. yy has an illegal value), or a
     * negative value if a nonrecoverable error occured. In the latter case, the
     * program halts. If a recoverable error occured, the integrator will
     * attempt to correct and retry.
     */
    static int ida_resid(realtype t, N_Vector y, N_Vector ydot, N_Vector r, void* f_data)
    {
        Cantera::ResidData* d = (Cantera::ResidData*) f_data;
        Cantera::ResidJacEval* f = d->m_func;
        Cantera::IDA_Solver* s = d->m_solver;
        double delta_t = s->getCurrentStepFromIDA();
        // TODO evaluate evalType. Assumed to be Base_ResidEval
        int flag = f->evalResidNJ(t, delta_t, NV_DATA_S(y), NV_DATA_S(ydot),
                                  NV_DATA_S(r));
        if (flag < 0) {
            // This signals to IDA that a nonrecoverable error has occurred.
            return flag;
        } else {
            return 0;
        }
    }

    //! Function called by IDA to evaluate the quadratures, given y and ydot.
    /*!
     * IDA allows passing in a void* pointer to access external data. Instead of
     * requiring the user to provide a integrand function directly to IDA (which
     * would require using the sundials data types N_Vector, etc.), we define
     * this function as the single function that IDA always calls. The real
     * evaluation of the integrands is done by an instance of a subclass of
     * ResidEval, passed in to this function as a pointer in the parameters.
     *
     * FROM IDA WRITEUP -> What the IDA solver expects as a return flag from its
     * residual routines:
     *
     * A IDAQuadRhsFn res should return a value of 0 if successful, a positive value
     * if a recoverable error occured (e.g. yy has an illegal value), or a
     * negative value if a nonrecoverable error occured. In the latter case, the
     * program halts. If a recoverable error occured, the integrator will
     * attempt to correct and retry.
     */
    static int ida_quad_rhs(realtype t, N_Vector y, N_Vector ydot, N_Vector rhsQ, void* f_data)
    {
        Cantera::ResidData* d = (Cantera::ResidData*) f_data;
        Cantera::ResidEval* f = d->m_func;
        Cantera::IDA_Solver* s = d->m_solver;
        double delta_t = s->getCurrentStepFromIDA();
        // TODO evaluate evalType. Assumed to be Base_ResidEval
        int flag = f->evalQuadRhs(t, NV_DATA_S(y), NV_DATA_S(ydot),
                                  NV_DATA_S(rhsQ));
        if (flag < 0) {
            // This signals to IDA that a nonrecoverable error has occurred.
            return flag;
        } else {
            return 0;
        }
    }

    //! Function called by by IDA to evaluate the Jacobian, given y and ydot.
    /*!
     * typedef int (*IDADlsDenseJacFn)(sd_size_t N, realtype t, realtype c_j,
     *                             N_Vector y, N_Vector yp, N_Vector r,
     *                             DlsMat Jac, void *user_data,
     *                             N_Vector tmp1, N_Vector tmp2, N_Vector tmp3);
     *
     * A IDADlsDenseJacFn should return
     * - 0 if successful,
     * - a positive int if a recoverable error occurred, or
     * - a negative int if a nonrecoverable error occurred.
     *
     * In the case of a recoverable error return, the integrator will attempt to
     * recover by reducing the stepsize (which changes cj).
     */
#if CT_SUNDIALS_VERSION >= 30
    static int ida_jacobian(realtype t, realtype c_j, N_Vector y, N_Vector yp,
                            N_Vector r, SUNMatrix Jac, void *f_data,
                            N_Vector tmp1, N_Vector tmp2, N_Vector tmp3)
    {
        Cantera::ResidData* d = (Cantera::ResidData*) f_data;
        Cantera::ResidJacEval* f = d->m_func;
        Cantera::IDA_Solver* s = d->m_solver;
        double delta_t = s->getCurrentStepFromIDA();
        double** cols;
        if (SUNMatGetID(Jac) == SUNMATRIX_DENSE) {
            cols = SM_COLS_D(Jac);
        } else if (SUNMatGetID(Jac) == SUNMATRIX_BAND) {
            cols = SM_COLS_B(Jac);
        } else {
            return 1; // Unknown SUNMatrix type
        }
        f->evalJacobianDP(t, delta_t, c_j, NV_DATA_S(y), NV_DATA_S(yp),
                          cols, NV_DATA_S(r));
        return 0;
    }
#else
    static int ida_jacobian(sd_size_t nrows, realtype t, realtype c_j, N_Vector y, N_Vector ydot, N_Vector r,
                            DlsMat Jac, void* f_data, N_Vector tmp1, N_Vector tmp2, N_Vector tmp3)
    {
        Cantera::ResidData* d = (Cantera::ResidData*) f_data;
        Cantera::ResidJacEval* f = d->m_func;
        Cantera::IDA_Solver* s = d->m_solver;
        double delta_t = s->getCurrentStepFromIDA();
        f->evalJacobianDP(t, delta_t, c_j, NV_DATA_S(y), NV_DATA_S(ydot),
                          Jac->cols, NV_DATA_S(r));
        return 0;
    }
#endif

}

namespace Cantera
{

IDA_Solver::IDA_Solver(ResidJacEval& f) :
    DAE_Solver(f),
    m_ida_mem(0),
    m_t0(0.0),
    m_y(0),
    m_ydot(0),
    m_id(0),
    m_constraints(0),
    m_abstol(0),
    m_type(0),
    m_itol(IDA_SS),
    m_iter(0),
    m_reltol(1.e-8),
    m_abstols(1.e-10),
    m_reltolsens(1.e-5),
    m_abstolsens(1.e-7),
    m_nabs(0),
    m_hmax(0.0),
    m_hmin(0.0),
    m_h0(0.0),
    m_maxsteps(20000),
    m_maxord(0),
    m_formJac(0),
    m_tstop(0.0),
    m_told_old(0.0),
    m_told(0.0),
    m_tcurrent(0.0),
    m_deltat(0.0),
    m_maxErrTestFails(-1),
    m_maxNonlinIters(0),
    m_maxNonlinConvFails(-1),
    m_setSuppressAlg(0),
    m_mupper(0),
    m_mlower(0),
    m_yS(nullptr),
    m_ySdot(nullptr),
    m_sens_ok(false),
    m_yQ(nullptr),
    m_yQdot(nullptr),
    m_reltolQuad(0.0),
    m_abstolQuad(0.0)
{
}

IDA_Solver::~IDA_Solver()
{
    if (m_ida_mem) {
        IDAFree(&m_ida_mem);
    }
    if (m_y) {
        N_VDestroy_Serial(m_y);
    }
    if (m_ydot) {
        N_VDestroy_Serial(m_ydot);
    }
    if (m_abstol) {
        N_VDestroy_Serial(m_abstol);
    }
    if (m_yS) {
        N_VDestroyVectorArray_Serial(m_yS, static_cast<sd_size_t>(m_ns));
    }
    if (m_ySdot) {
        N_VDestroyVectorArray_Serial(m_ySdot, static_cast<sd_size_t>(m_ns));
    }
    if (m_yQ) {
        N_VDestroy_Serial(m_yQ);
    }
    if (m_yQdot) {
        N_VDestroy_Serial(m_yQdot);
    }

    if (m_constraints) {
        N_VDestroy_Serial(m_constraints);
    }
}

doublereal IDA_Solver::solution(int k) const
{
    return NV_Ith_S(m_y,k);
}

const doublereal* IDA_Solver::solutionVector() const
{
    return NV_DATA_S(m_y);
}

doublereal IDA_Solver::derivative(int k) const
{
    return NV_Ith_S(m_ydot,k);
}

const doublereal* IDA_Solver::derivativeVector() const
{
    return NV_DATA_S(m_ydot);
}


void IDA_Solver::setTolerances(double reltol, double* abstol)
{
    m_itol = IDA_SV;
    if (!m_abstol) {
        m_abstol = N_VNew_Serial(m_neq);
    }
    for (int i = 0; i < m_neq; i++) {
        NV_Ith_S(m_abstol, i) = abstol[i];
    }
    m_reltol = reltol;
    if (m_ida_mem) {
        int flag = IDASVtolerances(m_ida_mem, m_reltol, m_abstol);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::setTolerances",
                               "Memory allocation failed.");
        }
    }
}

void IDA_Solver::setTolerances(doublereal reltol, doublereal abstol)
{
    m_itol = IDA_SS;
    m_reltol = reltol;
    m_abstols = abstol;
    if (m_ida_mem) {
        int flag = IDASStolerances(m_ida_mem, m_reltol, m_abstols);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::setTolerances",
                               "Call to IDASStolerances failed.");
        }
    }
}

void IDA_Solver::setSensitivityTolerances(doublereal rtol_sens, 
                                          doublereal atol_sens)
{
    m_reltolsens = rtol_sens;
    m_abstolsens = atol_sens;
}

void IDA_Solver::setLinearSolverType(int solverType)
{
    m_type = solverType;
}

void IDA_Solver::setDenseLinearSolver()
{
    setLinearSolverType(0);
}

void IDA_Solver::setBandedLinearSolver(int m_upper, int m_lower)
{
    m_type = 2;
    m_mupper = m_upper;
    m_mlower = m_lower;
}

void IDA_Solver::setMaxOrder(int n)
{
    m_maxord = n;
}

void IDA_Solver::setMaxNumSteps(int n)
{
    m_maxsteps = n;
}

void IDA_Solver::setInitialStepSize(doublereal h0)
{
    m_h0 = h0;
}

void IDA_Solver::setStopTime(doublereal tstop)
{
    m_tstop = tstop;
}

doublereal IDA_Solver::getCurrentStepFromIDA()
{
    doublereal hcur;
    IDAGetCurrentStep(m_ida_mem, &hcur);
    return hcur;
}

void IDA_Solver::setJacobianType(int formJac)
{
    m_formJac = formJac;
    if (m_ida_mem && m_formJac == 1) {
        #if CT_SUNDIALS_VERSION >= 30
            int flag = IDADlsSetJacFn(m_ida_mem, ida_jacobian);
        #else
            int flag = IDADlsSetDenseJacFn(m_ida_mem, ida_jacobian);
        #endif
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::setJacobianType",
                               "IDADlsSetDenseJacFn failed.");
        }
    }
}

/*
bool checkFlag(const int constraintFlag)
{
    auto cflag = constraintFlag;
    bool valid_cflag = false;
    if (cflag == c_NONE || cflag == c_GE_ZERO || cflag == c_GT_ZERO || 
        cflag == c_LE_ZERO || cflag == c_LT_ZERO)
        valid_cflag = true;
    return valid_cflag;
}
*/

void IDA_Solver::setConstraint(const int k, const int constraintFlag)
{
    if (checkFlag(constraintFlag)){
        if(!m_constraints) {
            m_constraints = N_VNew_Serial(m_neq);
        }
        NV_Ith_S(m_constraints, k) = constraintFlag;
        if (m_ida_mem){
            auto flag = IDASetConstraints(m_ida_mem, m_constraints);
            if (flag != IDA_SUCCESS) {
                throw CanteraError("IDA_Solver::setConstraint", 
                                   "IDASetConstraint failed.");
            }
        }
    } else { 
        throw CanteraError("IDA_Solver::setConstraint", 
                           "Invalid Constraint vaue");
    }
}

void IDA_Solver::setConstraints(const int * const constraintFlags)
{
    if(!m_constraints) {
        m_constraints = N_VNew_Serial(m_neq);
    }
    for(size_t i = 0; i < m_neq; i++){
        auto cflag = constraintFlags[i];
        cout << i << " cflag " << cflag << endl;
        if(!checkFlag(cflag)){
            throw CanteraError("IDA_Solver::setConstraints", 
                               "Invalid Constraint vaue detected");
        }
        NV_Ith_S(m_constraints, i) = cflag;
    }
    if (m_ida_mem){
        auto flag = IDASetConstraints(m_ida_mem, m_constraints);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::setConstraints", 
                               "IDASetConstraint failed.");
        }
    }
}

void IDA_Solver::setMaxErrTestFailures(int maxErrTestFails)
{
    m_maxErrTestFails = maxErrTestFails;
}

void IDA_Solver::setMaxNonlinIterations(int n)
{
    m_maxNonlinIters = n;
}

void IDA_Solver::setMaxNonlinConvFailures(int n)
{
    m_maxNonlinConvFails = n;
}

void IDA_Solver::inclAlgebraicInErrorTest(bool yesno)
{
    if (yesno) {
        m_setSuppressAlg = 0;
    } else {
        m_setSuppressAlg = 1;
    }
}

void IDA_Solver::sensInit(double t0)
{
    m_sens_ok = false;
    m_yS = N_VCloneVectorArray_Serial(static_cast<sd_size_t>(m_ns), m_y);
    for (size_t n = 0; n < m_ns; n++) {
        N_VConst(0.0, m_yS[n]);
    }
    m_ySdot = N_VCloneVectorArray_Serial(static_cast<sd_size_t>(m_ns), m_y);
    for (size_t n = 0; n < m_ns; n++) {
        N_VConst(0.0, m_ySdot[n]);
    }


    int flag = IDASensInit(m_ida_mem, static_cast<sd_size_t>(m_ns),
                           IDA_STAGGERED, IDASensResFn(0), m_yS, m_ySdot);

    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::sensInit", "Error in IDASensInit");
    }
    vector_fp atol(m_ns);
    for (size_t n = 0; n < m_ns; n++) {
        // This scaling factor is tuned so that reaction and species enthalpy
        // sensitivities can be computed simultaneously with the same abstol.
        atol[n] = m_abstolsens / m_resid.m_paramScales[n];
    }
    flag = IDASensSStolerances(m_ida_mem, m_reltolsens, atol.data());
}

void IDA_Solver::init(doublereal t0)
{
    m_t0 = t0;
    m_told = t0;
    m_told_old = t0;
    m_tcurrent = t0;
    if (m_y) {
        N_VDestroy_Serial(m_y);
    }
    if (m_ydot) {
        N_VDestroy_Serial(m_ydot);
    }
    if (m_id) {
        N_VDestroy_Serial(m_id);
    }
    if (m_constraints) {
        N_VDestroy_Serial(m_constraints);
    }
    if (m_yQ) {
        N_VDestroy_Serial(m_yQ);
    }
    if (m_yQdot) {
        N_VDestroy_Serial(m_yQdot);
    }

    m_y = N_VNew_Serial(m_neq);
    m_ydot = N_VNew_Serial(m_neq);
    m_constraints = N_VNew_Serial(m_neq);

    for (int i=0; i<m_neq; i++) {
        NV_Ith_S(m_y, i) = 0.0;
        NV_Ith_S(m_ydot, i) = 0.0;
        NV_Ith_S(m_constraints, i) = 0.0;
    }

    if (nQuadEquations()){
        m_yQ = N_VNew_Serial(nQuadEquations());
        m_yQdot = N_VNew_Serial(nQuadEquations());
    }
    for (int i = 0; i < nQuadEquations(); i++) {
        NV_Ith_S(m_yQ, i) = 0.0;
        NV_Ith_S(m_yQdot, i) = 0.0;
    }

    // get the initial conditions
    m_resid.getInitialConditions(m_t0, NV_DATA_S(m_y), NV_DATA_S(m_ydot));

    if (m_ida_mem) {
        IDAFree(&m_ida_mem);
    }

    /* Call IDACreate */
    m_ida_mem = IDACreate();

    int flag = IDAInit(m_ida_mem, ida_resid, m_t0, m_y, m_ydot);
    if (flag != IDA_SUCCESS) {
        if (flag == IDA_MEM_FAIL) {
            throw CanteraError("IDA_Solver::init",
                               "Memory allocation failed.");
        } else if (flag == IDA_ILL_INPUT) {
            throw CanteraError("IDA_Solver::init",
                "Illegal value for IDAInit input argument.");
        } else {
            throw CanteraError("IDA_Solver::init", "IDAInit failed.");
        }
    }
    if (m_itol == IDA_SV) {
        flag = IDASVtolerances(m_ida_mem, m_reltol, m_abstol);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "Memory allocation failed.");
        }
    } else {
        flag = IDASStolerances(m_ida_mem, m_reltol, m_abstols);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "Memory allocation failed.");
        }
    }

    // set the linear solver type
    if (m_type == 1 || m_type == 0) {
        long int N = m_neq;
        int flag;
        #if CT_SUNDIALS_VERSION >= 30
            //SUNLinSolFree((SUNLinearSolver) m_linsol);
            //SUNMatDestroy((SUNMatrix) m_linsol_matrix);
            m_linsol_matrix = SUNDenseMatrix(N, N);
            if (m_linsol_matrix == nullptr) {
                throw CanteraError("IDA_Solver::init",
                    "Unable to create SUNDenseMatrix of size {0} x {0}", N);
            }
            #if CT_SUNDIALS_USE_LAPACK
                m_linsol = SUNLapackDense(m_y, (SUNMatrix) m_linsol_matrix);
            #else
                m_linsol = SUNDenseLinearSolver(m_y, (SUNMatrix) m_linsol_matrix);
            #endif
            flag = IDADlsSetLinearSolver(m_ida_mem, (SUNLinearSolver) m_linsol,
                                         (SUNMatrix) m_linsol_matrix);
        #else
            flag = IDADense(m_ida_mem, N);
        #endif
        if (flag) {
            throw CanteraError("IDA_Solver::init", "IDADense failed");
        }
    } else if (m_type == 2) {
        long int N = m_neq;
        long int nu = m_mupper;
        long int nl = m_mlower;
        #if CT_SUNDIALS_VERSION >= 30
            SUNLinSolFree((SUNLinearSolver) m_linsol);
            SUNMatDestroy((SUNMatrix) m_linsol_matrix);
            #if CT_SUNDIALS_VERSION < 40
                m_linsol_matrix = SUNBandMatrix(N, nu, nl, nu+nl);
            #else
                m_linsol_matrix = SUNBandMatrix(N, nu, nl);
            #endif
            if (m_linsol_matrix == nullptr) {
                throw CanteraError("IDA_Solver::init",
                    "Unable to create SUNBandMatrix of size {} with bandwidths "
                    "{} and {}", N, nu, nl);
            }
            #if CT_SUNDIALS_USE_LAPACK
                m_linsol = SUNLapackBand(m_y, (SUNMatrix) m_linsol_matrix);
            #else
                m_linsol = SUNBandLinearSolver(m_y, (SUNMatrix) m_linsol_matrix);
            #endif
            IDADlsSetLinearSolver(m_ida_mem, (SUNLinearSolver) m_linsol,
                                  (SUNMatrix) m_linsol_matrix);
        #else
            IDABand(m_ida_mem, N, nu, nl);
        #endif
    } else {
        throw CanteraError("IDA_Solver::init",
                           "unsupported linear solver type");
    }

    if (m_formJac == 1) {
        #if CT_SUNDIALS_VERSION >= 30
            flag = IDADlsSetJacFn(m_ida_mem, ida_jacobian);
        #else
            flag = IDADlsSetDenseJacFn(m_ida_mem, ida_jacobian);
        #endif
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init",
                               "IDADlsSetDenseJacFn failed.");
        }
    }

    // pass a pointer to func in m_data
    m_fdata.reset(new ResidData(&m_resid, this, m_resid.nparams()));
    flag = IDASetUserData(m_ida_mem, m_fdata.get());
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::init", "IDASetUserData failed.");
    }

    // Sensitivity
    if (m_ns > 0) {
        sensInit(t0);
        flag = IDASetSensParams(m_ida_mem, m_resid.m_sens_params.data(), 
                                m_resid.m_paramScales.data(), NULL);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init",
                               "IDASetSensParams failed.");
        }

        // Applying forward difference scheme. Central difference scheme is 
        // giving results off by factor of 2
        // TODO: Get the forward difference scheme working. Temporarily the 
        // the deficiency of central difference scheme is overcome by multiplying
        // the sensitivity coefficient with 2.
        /*flag = IDASetSensDQMethod(m_ida_mem, IDA_FORWARD, 1.0);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init",
                               "IDASetSensDQMethod failed.");
        }*/

    }

    // set options
    if (m_maxord > 0) {
        flag = IDASetMaxOrd(m_ida_mem, m_maxord);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDASetMaxOrd failed.");
        }
    }
    if (m_maxsteps > 0) {
        flag = IDASetMaxNumSteps(m_ida_mem, m_maxsteps);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDASetMaxNumSteps failed.");
        }
    }
    if (m_h0 > 0.0) {
        flag = IDASetInitStep(m_ida_mem, m_h0);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDASetInitStep failed.");
        }
    }
    if (m_tstop > 0.0) {
        flag = IDASetStopTime(m_ida_mem, m_tstop);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDASetStopTime failed.");
        }
    }
    if (m_maxErrTestFails >= 0) {
        flag = IDASetMaxErrTestFails(m_ida_mem, m_maxErrTestFails);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init",
                               "IDASetMaxErrTestFails failed.");
        }
    }
    if (m_maxNonlinIters > 0) {
        flag = IDASetMaxNonlinIters(m_ida_mem, m_maxNonlinIters);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init",
                               "IDASetmaxNonlinIters failed.");
        }
    }
    if (m_maxNonlinConvFails >= 0) {
        flag = IDASetMaxConvFails(m_ida_mem, m_maxNonlinConvFails);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init",
                               "IDASetMaxConvFails failed.");
        }
    }
    if (m_setSuppressAlg != 0) {
        flag = IDASetSuppressAlg(m_ida_mem, m_setSuppressAlg);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDASetSuppressAlg failed.");
        }
    }
    if (m_resid.nConstraints()){ // Constraints are defined. 
        for (size_t i = 0; i < m_neq; i++) {
            NV_Ith_S(m_constraints, i) = m_resid.constraint(i);
        }
        flag = IDASetConstraints(m_ida_mem, m_constraints);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDASetConstraints failed");
        }
    }

    // Work on quadrature setup
    if (nQuadEquations()){
        flag = IDAQuadInit(m_ida_mem, ida_quad_rhs, m_yQ);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::init", "IDAQuadInit failed");
        }
    }
}

void IDA_Solver::correctInitial_Y_given_Yp(doublereal* y, doublereal* yp, doublereal tout)
{
    doublereal tout1 = tout;
    if (tout == 0.0) {
        double h0 = 1.0E-5;
        if (m_h0 > 0.0) {
            h0 = m_h0;
        }
        tout1 = m_t0 + h0;
    }

    int flag = IDACalcIC(m_ida_mem, IDA_Y_INIT, tout1);
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::correctInitial_Y_given_Yp",
                           "IDACalcIC failed: error = {}", flag);
    }

    flag = IDAGetConsistentIC(m_ida_mem, m_y, m_ydot);
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::correctInitial_Y_given_Yp",
                           "IDAGetSolution failed: error = {}", flag);
    }
    for (int i = 0; i < m_neq; i++) {
        y[i] = NV_Ith_S(m_y, i);
        yp[i] = NV_Ith_S(m_ydot, i);
    }

}

void IDA_Solver::correctInitial_YaYp_given_Yd(doublereal* y, doublereal* yp, doublereal tout)
{
    int icopt = IDA_YA_YDP_INIT;
    doublereal tout1 = tout;
    if (tout == 0.0) {
        double h0 = 1.0E-5;
        if (m_h0 > 0.0) {
            h0 = m_h0;
        }
        tout1 = m_t0 + h0;
    }

    int flag = IDACalcIC(m_ida_mem, icopt, tout1);
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::correctInitial_YaYp_given_Yd",
                           "IDACalcIC failed: error = {}", flag);
    }

    flag = IDAGetConsistentIC(m_ida_mem, m_y, m_ydot);
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::correctInitial_YaYp_given_Yd",
                           "IDAGetConsistentIC failed: error = {}", flag);
    }
    for (int i = 0; i < m_neq; i++) {
        y[i] = NV_Ith_S(m_y, i);
        yp[i] = NV_Ith_S(m_ydot, i);
    }
}

void IDA_Solver::correctSensInitial_Y(doublereal* yS, doublereal* ypS)
{
    /*
    auto flag = IDAGetSensConsistentIC(m_ida_mem, m_yS, m_ySdot);
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::correctSensInitial_Y",
                           "IDAGetSensConsistentIC failed: error = {}", flag);
    }
    for (int i = 0; i < m_ns; i++) {
        yS[i] = NV_Ith_S(m_yS, i);
        ypS[i] = NV_Ith_S(m_ySdot, i);
    }
    */
}

int IDA_Solver::solve(double tout)
{
    double tretn = tout - 1000;
    int flag;
    flag = IDASetStopTime(m_ida_mem, tout);
    if (flag != IDA_SUCCESS) {
        throw CanteraError("IDA_Solver::solve", "IDA error encountered.");
    }
    while (tretn < tout) {
        if (tout <= m_tcurrent) {
            cout << "Simulation end time reached\n";
            break;
            //throw CanteraError("IDA_Solver::solve", "tout <= tcurrent");
        }
        m_told_old = m_told;
        m_told = m_tcurrent;
        flag = IDASolve(m_ida_mem, tout, &tretn, m_y, m_ydot, IDA_NORMAL);
        if (flag < 0) {
            throw CanteraError("IDA_Solver::solve", "IDA error encountered.");
        } else if (flag == IDA_TSTOP_RETURN) {
            // we've reached our goal, and have actually integrated past it
        } else if (flag == IDA_ROOT_RETURN) {
            // not sure what to do with this yet
        } else if (flag == IDA_WARNING) {
            cout << "DAE Solver WARNING!\n";
            //throw CanteraError("IDA_Solver::solve", "IDA Warning encountered.");
        }
        m_tcurrent = tretn;
        m_deltat = m_tcurrent - m_told;
    };

    if (flag != IDA_SUCCESS && flag != IDA_TSTOP_RETURN) {
        throw CanteraError("IDA_Solver::solve", "IDA error encountered.");
    }
    m_sens_ok = false;
    return flag;
}

double IDA_Solver::step(double tout)
{
    double t;
    int flag;
    if (tout <= m_tcurrent) {
        throw CanteraError("IDA_Solver::step", "tout <= tcurrent");
    }
    m_told_old = m_told;
    m_told = m_tcurrent;
    flag = IDASolve(m_ida_mem, tout, &t, m_y, m_ydot, IDA_ONE_STEP);
    if (flag < 0) {
        throw CanteraError("IDA_Solver::step", "IDA error encountered.");
    } else if (flag == IDA_TSTOP_RETURN) {
        // we've reached our goal, and have actually integrated past it
    } else if (flag == IDA_ROOT_RETURN) {
        // not sure what to do with this yet
    } else if (flag == IDA_WARNING) {
        throw CanteraError("IDA_Solver::step", "IDA Warning encountered.");
    }
    m_tcurrent = t;
    m_deltat = m_tcurrent - m_told;
    m_sens_ok = false;
    return t;
}

const doublereal* IDA_Solver::quadratureVector() const 
{
    if (nQuadEquations()){
        double tret;
        auto flag = IDAGetQuad(m_ida_mem, &tret, m_yQ);
        if (flag != IDA_SUCCESS) {
            throw CanteraError("IDA_Solver::quadratureVector",
                               "IDAGetQuad failed: error = {}", flag);
        }
        return NV_DATA_S(m_yQ);
    }
    return nullptr;
}

double IDA_Solver::getCurrentTimeFromIDA(){
    return m_tcurrent;
}

double IDA_Solver::sensitivity(size_t k, size_t p)
{
    if (!m_sens_ok && m_ns){
        if (m_tcurrent = m_t0){
            auto flag = IDAGetSensConsistentIC(m_ida_mem, m_yS, m_ySdot);
            if (flag != IDA_SUCCESS) {
                throw CanteraError("IDA_Solver::sensitivity",
                                   "IDAGetSensConsistentIC failed: error = {}", flag);
            }
        } else {
            auto flag = IDAGetSens(m_ida_mem, &m_tcurrent, m_yS);
            if (flag != IDA_SUCCESS) {
                throw CanteraError("IDA_Solver::sensitivity",
                                   "IDAGetSens failed: error = {}", flag);
            }
        }
        m_sens_ok = true;
    }

    if (k >= m_neq) {
        throw CanteraError("IDA_Solver::sensitivity",
                           "sensitivity: equation index out of range ({})", k);
    }
    if (p >= m_ns) {
        throw CanteraError("IDA_Solver::sensitivity",
                           "sensitivity: parameter index out of range ({})", p);
    }
    return NV_Ith_S(m_yS[p], k);
}


doublereal IDA_Solver::getOutputParameter(int flag) const
{
    long int lenrw, leniw;
    switch (flag) {
    case REAL_WORKSPACE_SIZE:
        flag = IDAGetWorkSpace(m_ida_mem, &lenrw, &leniw);
        return doublereal(lenrw);
        break;
    }
    return 0.0;
}

}
