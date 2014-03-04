/*
  Copyright (C) 2009-2013 by Andreas Lauser

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
/*!
 * \file
 *
 * \copydoc Ewoms::FvBaseNewtonMethod
 */
#ifndef EWOMS_FV_BASE_NEWTON_METHOD_HH
#define EWOMS_FV_BASE_NEWTON_METHOD_HH

#include "fvbasenewtonconvergencewriter.hh"

#include <ewoms/nonlinear/newtonmethod.hh>
#include <opm/core/utility/PropertySystem.hpp>

namespace Ewoms {

template <class TypeTag>
class FvBaseNewtonMethod;

template <class TypeTag>
class FvBaseNewtonConvergenceWriter;
} // namespace Ewoms

namespace Opm {
namespace Properties {
//! create a type tag for the ECFV specific Newton method
NEW_TYPE_TAG(FvBaseNewtonMethod, INHERITS_FROM(NewtonMethod));

//! The class dealing with the balance equations
NEW_PROP_TAG(Model);

//! The assembler for the Jacobian matrix
NEW_PROP_TAG(JacobianBaseAssembler);

//! The class storing primary variables plus pseudo primary variables
NEW_PROP_TAG(PrimaryVariables);

//! The number of balance equations.
NEW_PROP_TAG(NumEq);

//! The discretization specific part of he implementing the Newton algorithm
NEW_PROP_TAG(DiscNewtonMethod);

//! The class implementing the Newton algorithm
NEW_PROP_TAG(NewtonMethod);

//! Specifies whether the Jacobian matrix should only be reassembled
//! if the current solution deviates too much from the evaluation point
NEW_PROP_TAG(EnablePartialReassemble);

/*!
 * \brief Specifies whether the update should be done using the line search
 *        method instead of the plain Newton method.
 *
 * Whether this property has any effect depends on whether the line
 * search method is implemented for the actual model's Newton
 * method's update_() method. By default line search is not used.
 */
NEW_PROP_TAG(NewtonEnableLineSearch);

//! Enable Jacobian recycling?
NEW_PROP_TAG(EnableJacobianRecycling);

//! Enable partial reassembly?
NEW_PROP_TAG(EnablePartialReassemble);

// set default values
SET_TYPE_PROP(FvBaseNewtonMethod, DiscNewtonMethod,
              Ewoms::FvBaseNewtonMethod<TypeTag>);
SET_TYPE_PROP(FvBaseNewtonMethod, NewtonMethod,
              typename GET_PROP_TYPE(TypeTag, DiscNewtonMethod));
SET_TYPE_PROP(FvBaseNewtonMethod, NewtonConvergenceWriter,
              Ewoms::FvBaseNewtonConvergenceWriter<TypeTag>);
SET_BOOL_PROP(FvBaseNewtonMethod, NewtonEnableLineSearch, false);
}} // namespace Properties, Opm

namespace Ewoms {
/*!
 * \ingroup Discretization
 * \ingroup Newton
 *
 * \brief A Newton method for models using the ECFV discretization.
 *
 * This class is sufficient for most models which use the ECFV discretization.
 */
template <class TypeTag>
class FvBaseNewtonMethod : public NewtonMethod<TypeTag>
{
    typedef Ewoms::NewtonMethod<TypeTag> ParentType;

    typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
    typedef typename GET_PROP_TYPE(TypeTag, Problem) Problem;
    typedef typename GET_PROP_TYPE(TypeTag, Model) Model;
    typedef typename GET_PROP_TYPE(TypeTag, NewtonMethod) NewtonMethod;
    typedef typename GET_PROP_TYPE(TypeTag, GlobalEqVector) GlobalEqVector;
    typedef typename GET_PROP_TYPE(TypeTag, SolutionVector) SolutionVector;
    typedef typename GET_PROP_TYPE(TypeTag, PrimaryVariables) PrimaryVariables;

    enum { numEq = GET_PROP_VALUE(TypeTag, NumEq) };

public:
    FvBaseNewtonMethod(Problem &problem)
        : ParentType(problem)
    { }

    /*!
     * \brief Register all run-time parameters of the Newton method.
     */
    static void registerParameters()
    {
        ParentType::registerParameters();

        EWOMS_REGISTER_PARAM(TypeTag, bool, NewtonEnableLineSearch,
                             "Use the line-search update method for the "
                             "Newton method (warning: slow!)");
    }

protected:
    friend class Ewoms::NewtonMethod<TypeTag>;

    /*!
     * \brief Update the current solution with a delta vector.
     *
     * The error estimates required for the newtonConverged() and
     * newtonProceed() methods should be updated inside this method.
     *
     * Different update strategies, such as line search and chopped
     * updates can be implemented. The default behavior is just to
     * subtract deltaU from uLastIter, i.e.
     * \f[ u^{k+1} = u^k - \Delta u^k \f]
     *
     * \param uCurrentIter The solution vector after the current iteration
     * \param uLastIter The solution vector after the last iteration
     * \param deltaU The delta as calculated from solving the linear
     *               system of equations. This parameter also stores
     *               the updated solution.
     */
    void update_(SolutionVector &uCurrentIter,
                 const SolutionVector &uLastIter,
                 const GlobalEqVector &deltaU)
    {
        // make sure not to swallow non-finite values at this point
        if (!std::isfinite(deltaU.two_norm2()))
            OPM_THROW(Opm::NumericalProblem, "Non-finite update!");

        // compute the vertex and element colors for partial reassembly
        if (enablePartialReassemble_()) {
            Scalar minReasmTol = 10*this->tolerance_();
            Scalar maxReasmTol = 1e-4;

            // rationale: the newton method has quadratic convergene1
            Scalar reassembleTol = this->error_*this->error_;
            reassembleTol = std::max(minReasmTol, std::min(maxReasmTol, reassembleTol));
            //Scalar reassembleTol = minReasmTol;

            model_().jacobianAssembler().updateDiscrepancy(uLastIter, deltaU);
            model_().jacobianAssembler().computeColors(reassembleTol);
        }

        // update the solution
        for (unsigned i = 0; i < uLastIter.size(); ++i) {
            uCurrentIter[i] = uLastIter[i];
            uCurrentIter[i] -= deltaU[i];
        }
    }

    /*!
     * \brief Called if the Newton method broke down.
     */
    void failed_()
    {
        ParentType::failed_();

        model_().jacobianAssembler().reassembleAll();
    }

    /*!
     * \brief Called when the Newton method was successful.
     */
    void succeeded_()
    {
        ParentType::succeeded_();

        if (enableJacobianRecycling_())
            model_().jacobianAssembler().setMatrixReuseable(true);
        else
            model_().jacobianAssembler().reassembleAll();
    }

    /*!
     * \brief Returns a reference to the problem.
     */
    Model &model_()
    { return ParentType::model(); }

    /*!
     * \brief Returns a reference to the problem.
     */
    const Model &model_() const
    { return ParentType::model(); }

    /*!
     * \brief Returns true iff the Jacobian assembler uses partial
     *        reassembly.
     */
    bool enablePartialReassemble_() const
    { return EWOMS_GET_PARAM(TypeTag, bool, EnablePartialReassemble); }

    /*!
     * \brief Returns true iff the Jacobian assembler recycles the matrix
     *        possible.
     */
    bool enableJacobianRecycling_() const
    { return EWOMS_GET_PARAM(TypeTag, bool, EnableJacobianRecycling); }

    /*!
     * \brief Returns true iff line search update proceedure should be
     *        used instead of the normal one.
     */
    bool enableLineSearch_() const
    { return EWOMS_GET_PARAM(TypeTag, bool, NewtonEnableLineSearch); }
};
} // namespace Ewoms

#endif