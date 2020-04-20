#ifndef BBLAS_BITMAT_HPP_
#define BBLAS_BITMAT_HPP_

/* very generic code on bit matrices. We don't keep much here. More
 * concrete types such as mat64 provide most of the actual code.
 */
#include <cstdint>
#include <cstring>
#include <vector>
#include "macros.h"
#include "memory.h"     // malloc_aligned in utils

namespace bblas_bitmat_details {

    template<typename T> struct bblas_bitmat_type_supported {
        static constexpr const bool value = false;
    };
    template<typename matrix> struct bitmat_ops {
        static void fill_random(matrix & w, gmp_randstate_t rstate);
        static void add(matrix & C, matrix const & A, matrix const & B);
        static void transpose(matrix & C, matrix const & A);
        static void mul(matrix & C, matrix const & A, matrix const & B);
        static void mul_lt_ge(matrix & C, matrix const & A, matrix const & B) {
            mul(C, A, B);
        }
        static void addmul(matrix & C, matrix const & A, matrix const & B);
        static void addmul(matrix & C,
                   matrix const & A,
                   matrix const & B,
                   unsigned int i0,
                   unsigned int i1,
                   unsigned int yi0,
                   unsigned int yi1);
        static void trsm(matrix const & L,
                matrix & U,
                unsigned int yi0,
                unsigned int yi1);
        static void trsm(matrix const & L, matrix & U);
        static void extract_uppertriangular(matrix & a, matrix const & b);
        static void extract_lowertriangular(matrix & a, matrix const & b);
        protected:
        /* these are accessed as _member functions_ in the matrix type */
        static bool is_lowertriangular(matrix const & a);
        static bool is_uppertriangular(matrix const & a);
        static bool triangular_is_unit(matrix const & a);
        static void make_uppertriangular(matrix & a);
        static void make_lowertriangular(matrix & a);
        static void make_unit_uppertriangular(matrix & a);
        static void make_unit_lowertriangular(matrix & a);
        static void triangular_make_unit(matrix & a);
    };
}

template<typename T>
class bitmat
    : public bblas_bitmat_details::bitmat_ops<bitmat<T>>
{
    typedef bblas_bitmat_details::bitmat_ops<bitmat<T>> ops;
    typedef bblas_bitmat_details::bblas_bitmat_type_supported<T> S;
    static_assert(S::value, "bblas bitmap must be built on uintX_t");

    public:
    static constexpr const int width = S::width;
    typedef T datatype;
    typedef std::vector<bitmat, aligned_allocator<bitmat, S::alignment>> vector_type;
    // typedef std::vector<bitmat> vector_type;

    private:
    T x[width]; // ATTRIBUTE((aligned(64))) ;

    public:
    static inline bitmat * alloc(size_t n) {
        return (bitmat *) malloc_aligned(n * sizeof(bitmat), S::alignment);
    }
    static inline void free(bitmat * p) {
        free_aligned(p);
    }

    inline T* data() { return x; }
    inline const T* data() const { return x; }
    T& operator[](int i) { return x[i]; }
    T operator[](int i) const { return x[i]; }
    inline bool operator==(bitmat const& a) const
    {
        /* anyway we're not going to do it any smarter in instantiations,
         * so let's rather keep this as a simple and stupid inline */
        return memcmp(x, a.x, sizeof(x)) == 0;
    }
    inline bool operator!=(bitmat const& a) const { return !operator==(a); }
    bitmat() {}
    inline bitmat(bitmat const& a) { memcpy(x, a.x, sizeof(x)); }
    inline bitmat& operator=(bitmat const& a)
    {
        memcpy(x, a.x, sizeof(x));
        return *this;
    }
    inline bitmat(int a) { *this = a; }
    inline bitmat& operator=(int a)
    {
        if (a & 1) {
            T mask = 1;
            for (int j = 0; j < width; j++, mask <<= 1)
                x[j] = mask;
        } else {
            memset(x, 0, sizeof(x));
        }
        return *this;
    }
    inline bool operator==(int a) const
    {
        if (a&1) {
            T mask = a&1;
            for (int j = 0; j < width; j++, mask <<= 1)
                if (x[j]&~mask) return false;
        } else {
            for (int j = 0; j < width; j++)
                if (x[j]) return false;
        }
        return true;
    }
    inline bool operator!=(int a) const { return !operator==(a); }

    inline bool is_lowertriangular() const { return ops::is_lowertriangular(*this); }
    inline bool is_uppertriangular() const { return ops::is_uppertriangular(*this); }
    inline bool triangular_is_unit() const { return ops::triangular_is_unit(*this); }
    inline void make_uppertriangular() { ops::make_uppertriangular(*this); }
    inline void make_lowertriangular() { ops::make_lowertriangular(*this); }
    inline void make_unit_uppertriangular() { ops::make_unit_uppertriangular(*this); }
    inline void make_unit_lowertriangular() { ops::make_unit_lowertriangular(*this); }
    inline void triangular_make_unit() { ops::triangular_make_unit(*this); }
};

#endif	/* BBLAS_BITMAT_HPP_ */
