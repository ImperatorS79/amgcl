#ifndef AMGCL_MPI_AMG_HPP
#define AMGCL_MPI_AMG_HPP

/*
The MIT License

Copyright (c) 2012-2018 Denis Demidov <dennis.demidov@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   amgcl/mpi/amg.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  Distributed memory AMG preconditioner.
 */

#include <iostream>
#include <iomanip>
#include <list>

#include <boost/io/ios_state.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/foreach.hpp>

#include <amgcl/backend/interface.hpp>
#include <amgcl/value_type/interface.hpp>
#include <amgcl/mpi/util.hpp>
#include <amgcl/mpi/distributed_matrix.hpp>

namespace amgcl {
namespace mpi {

template <
    class Backend,
    class Coarsening,
    template <class> class Relax
    >
class amg {
    public:
        typedef Backend                                    backend_type;
        typedef typename Backend::params                   backend_params;
        typedef typename Backend::value_type               value_type;
        typedef typename math::scalar_of<value_type>::type scalar_type;
        typedef Relax<Backend>                             relax_type;
        typedef distributed_matrix<Backend>                matrix;
        typedef typename Backend::vector                   vector;

        struct params {
            typedef typename Coarsening::params coarsening_params;
            typedef typename relax_type::params relax_params;

            coarsening_params coarsening;   ///< Coarsening parameters.
            relax_params      relax;        ///< Relaxation parameters.

            /// Specifies when level is coarse enough to be solved directly.
            /**
             * If number of variables at a next level in the hierarchy becomes
             * lower than this threshold, then the hierarchy construction is
             * stopped and the linear system is solved directly at this level.
             */
            unsigned coarse_enough;

            /// Maximum number of levels.
            /** If this number is reached while the size of the last level is
             * greater that `coarse_enough`, then the coarsest level will not
             * be solved exactly, but will use a smoother.
             */
            unsigned max_levels;

            /// Number of pre-relaxations.
            unsigned npre;

            /// Number of post-relaxations.
            unsigned npost;

            /// Number of cycles (1 for V-cycle, 2 for W-cycle, etc.).
            unsigned ncycle;

            /// Number of cycles to make as part of preconditioning.
            unsigned pre_cycles;

            params() :
                coarse_enough(1024),
                max_levels( std::numeric_limits<unsigned>::max() ),
                npre(1), npost(1), ncycle(1), pre_cycles(1)
            {}

            params(const boost::property_tree::ptree &p)
                : AMGCL_PARAMS_IMPORT_CHILD(p, coarsening),
                  AMGCL_PARAMS_IMPORT_CHILD(p, relax),
                  AMGCL_PARAMS_IMPORT_VALUE(p, coarse_enough),
                  AMGCL_PARAMS_IMPORT_VALUE(p, max_levels),
                  AMGCL_PARAMS_IMPORT_VALUE(p, npre),
                  AMGCL_PARAMS_IMPORT_VALUE(p, npost),
                  AMGCL_PARAMS_IMPORT_VALUE(p, ncycle),
                  AMGCL_PARAMS_IMPORT_VALUE(p, pre_cycles)
            {
                AMGCL_PARAMS_CHECK(p, (coarsening)(relax)(coarse_enough)
                        (max_levels)(npre)(npost)(ncycle)(pre_cycles));

                amgcl::precondition(max_levels > 0, "max_levels should be positive");
            }

            void get(
                    boost::property_tree::ptree &p,
                    const std::string &path = ""
                    ) const
            {
                AMGCL_PARAMS_EXPORT_CHILD(p, path, coarsening);
                AMGCL_PARAMS_EXPORT_CHILD(p, path, relax);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, coarse_enough);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, max_levels);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, npre);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, npost);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, ncycle);
                AMGCL_PARAMS_EXPORT_VALUE(p, path, pre_cycles);
            }
        } prm;

        template <class Matrix>
        amg(
                communicator comm,
                const Matrix &A,
                const params &prm = params(),
                const backend_params &bprm = backend_params()
           ) : prm(prm)
        {
            init(boost::make_shared<matrix>(comm, A, backend::rows(A), bprm), bprm);
        }

        template <class Vec1, class Vec2>
        void cycle(
                const Vec1 &rhs,
#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES
                Vec2       &x
#else
                Vec2       &&x
#endif
                ) const
        {
            cycle(levels.begin(), rhs, x);
        }

        template <class Vec1, class Vec2>
        void apply(
                const Vec1 &rhs,
#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES
                Vec2       &x
#else
                Vec2       &&x
#endif
                ) const
        {
            if (prm.pre_cycles) {
                backend::clear(x);
                for(unsigned i = 0; i < prm.pre_cycles; ++i)
                    cycle(levels.begin(), rhs, x);
            } else {
                backend::copy(rhs, x);
            }
        }

        /// Returns the system matrix from the finest level.
        boost::shared_ptr<matrix> system_matrix_ptr() const {
            return levels.front().A;
        }

        const matrix& system_matrix() const {
            return *system_matrix_ptr();
        }
    private:
        struct level {
            boost::shared_ptr<matrix>     A, P, R;
            boost::shared_ptr<vector>     f, u, t;
            boost::shared_ptr<relax_type> relax;

            level() {}

            level(
                    boost::shared_ptr<matrix> A,
                    params &prm,
                    const backend_params &bprm
                 )
                : A(A),
                  f(Backend::create_vector(A->loc_rows(), bprm)),
                  u(Backend::create_vector(A->loc_rows(), bprm)),
                  t(Backend::create_vector(A->loc_rows(), bprm)),
                  relax(boost::make_shared<relax_type>(*A, prm.relax, bprm))
            {
            }

            boost::shared_ptr<matrix> step_down(params &prm)
            {
                boost::tie(P, R) = Coarsening::transfer_operators(*A, prm.coarsening);

                if (P->glob_cols() == 0) {
                    // Zero-sized coarse level in amgcl (diagonal matrix?)
                    return boost::shared_ptr<matrix>();
                }

                return Coarsening::coarse_operator(*A, *P, *R, prm.coarsening);
            }

            void move_to_backend() {
                if (A) A->move_to_backend();
                if (P) P->move_to_backend();
                if (R) R->move_to_backend();
            }
        };

        typedef typename std::list<level>::const_iterator level_iterator;

        std::list<level> levels;

        void init(boost::shared_ptr<matrix> A, const backend_params &bprm)
        {
            mpi::precondition(A->comm(), A->glob_rows() == A->glob_cols(),
                    "Matrix should be square!");

            while(A->glob_rows() > prm.coarse_enough && levels.size() < prm.max_levels) {
                levels.push_back( level(A, prm, bprm) );
                if (levels.size() >= prm.max_levels) break;

                A = levels.back().step_down(prm);
                levels.back().move_to_backend();

                if (!A) {
                    // Zero-sized coarse level. Probably the system matrix on
                    // this level is diagonal, should be easily solvable with a
                    // couple of smoother iterations.
                    break;
                }
            }

            if (A) {
                levels.push_back(level(A, prm, bprm));
                levels.back().move_to_backend();
            }
        }

        template <class Vec1, class Vec2>
        void cycle(level_iterator lvl, const Vec1 &rhs, Vec2 &x) const {
            level_iterator nxt = lvl, end = levels.end();
            ++nxt;

            if (nxt == end) {
                lvl->relax->apply_pre(*lvl->A, rhs, x, *lvl->t, prm.relax);
                lvl->relax->apply_post(*lvl->A, rhs, x, *lvl->t, prm.relax);
            } else {
                for (size_t j = 0; j < prm.ncycle; ++j) {
                    for(size_t i = 0; i < prm.npre; ++i)
                        lvl->relax->apply_pre(*lvl->A, rhs, x, *lvl->t, prm.relax);

                    backend::residual(rhs, *lvl->A, x, *lvl->t);

                    backend::spmv(math::identity<scalar_type>(), *lvl->R, *lvl->t, math::zero<scalar_type>(), *nxt->f);

                    backend::clear(*nxt->u);
                    cycle(nxt, *nxt->f, *nxt->u);

                    backend::spmv(math::identity<scalar_type>(), *lvl->P, *nxt->u, math::identity<scalar_type>(), x);

                    for(size_t i = 0; i < prm.npost; ++i)
                        lvl->relax->apply_post(*lvl->A, rhs, x, *lvl->t, prm.relax);
                }
            }
        }

    template <class B, class C, template <class> class R>
    friend std::ostream& operator<<(std::ostream &os, const amg<B, C, R> &a);
};

template <class B, class C, template <class> class R>
std::ostream& operator<<(std::ostream &os, const amg<B, C, R> &a)
{
    typedef typename amg<B, C, R>::level level;
    boost::io::ios_all_saver stream_state(os);

    size_t sum_dof = 0;
    size_t sum_nnz = 0;

    BOOST_FOREACH(const level &lvl, a.levels) {
        sum_dof += lvl.A->glob_rows();
        sum_nnz += lvl.A->glob_nonzeros();
    }

    os << "Number of levels:    "   << a.levels.size()
        << "\nOperator complexity: " << std::fixed << std::setprecision(2)
        << 1.0 * sum_nnz / a.levels.front().A->glob_nonzeros()
        << "\nGrid complexity:     " << std::fixed << std::setprecision(2)
        << 1.0 * sum_dof / a.levels.front().A->glob_rows()
        << "\n\nlevel     unknowns       nonzeros\n"
        << "---------------------------------\n";

    size_t depth = 0;
    BOOST_FOREACH(const level &lvl, a.levels) {
        os << std::setw(5)  << depth++
           << std::setw(13) << lvl.A->glob_rows()
           << std::setw(15) << lvl.A->glob_nonzeros() << " ("
           << std::setw(5) << std::fixed << std::setprecision(2)
           << 100.0 * lvl.A->glob_nonzeros() / sum_nnz
           << "%)" << std::endl;
    }

    return os;
}
} // namespace mpi
} // namespace amgcl

#endif