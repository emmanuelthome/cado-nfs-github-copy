#ifndef LAS_FILL_IN_BUCKETS_HPP_
#define LAS_FILL_IN_BUCKETS_HPP_

#include <array>
#include <memory>
#include "fb-types.h"
#include "las-plattice.hpp"
#include "tdict.hpp"
#include "threadpool.hpp"
#include "las-threads-work-data.hpp"

#include "las-forwardtypes.hpp"

// This one is used for keeping information of middle primes.
struct precomp_plattice_t {
    typedef std::vector<plattices_vector_t> vec_type;
    std::array<std::array<vec_type, FB_MAX_PARTS>, 2> v;
    precomp_plattice_t(precomp_plattice_t const&) = delete;
    precomp_plattice_t() = default;
    void push(int side, int level, plattices_vector_t&& x) {
        v[side][level].push_back(std::move(x));
    }
    ~precomp_plattice_t() = default;
    std::vector<plattices_vector_t> & operator()(int side, int level) {
        return v[side][level];
    }
    std::vector<plattices_vector_t> const & operator()(int side, int level) const {
        return v[side][level];
    }
};


template <int LEVEL>
void
downsort_tree(
        nfs_work &ws,
        std::shared_ptr<nfs_work_cofac> wc_p,
        std::shared_ptr<nfs_aux> aux_p,
        thread_pool &pool,
        uint32_t bucket_index,
        uint32_t first_region0_index,
        precomp_plattice_t & precomp_plattice,
        where_am_I & w);

void fill_in_buckets_toplevel(
        nfs_work &ws,
        nfs_aux &aux,
        thread_pool &pool,
        int side,
        where_am_I & w);

void fill_in_buckets_prepare_precomp_plattice(
        nfs_work &ws,
        thread_pool &pool,
        int side,
        int level,
        precomp_plattice_t & precomp_plattice);

#endif
