#ifndef SLAVE_ARGUMENTS_HPP_
#define SLAVE_ARGUMENTS_HPP_

#include "corners.hpp"
#include "arguments.hpp"
#include "fmt.hpp"

struct slave_arguments {
	corner b, e;
	unsigned int scol;
	std::string task;
	int nt;
	slave_arguments() {
		scol = (unsigned int) -1;
		nt = -1;
	}

	bool parse(argparser::situation& s) {
		if (s("--sc", scol)) return true;
		if (s("--task", task)) return true;
#ifdef	ENABLE_PTHREADS
		if (s("--nthreads", nt)) return true;
#endif
		if (b.unset() && s(NULL, b)) return true;
		if (e.unset() && s(NULL, e)) return true;
		if (b.unset() && e.unset() && s(NULL, b.second)) {
			b.first = 0;
			e.second = b.second;
			e.first = -1;
			return true;
		}
		return false;
	}
	void doc(std::ostream& o) {
		o << "--task [ slave | mksol ]\ttask type\n";
		o << "--sc <col>\t[mksol] solution column to try\n";
#ifdef	ENABLE_PTHREADS
		o << "--nthreads <n>\tnumber of threads to start\n";
#endif
		o << "<i0>,<j0>\trow/column start indices\n";
		o << "<i1>,<j1>\trow/column end indices\n";
		o << "<j>\tcolumn to work with\n";
	}
	bool check(std::ostream& o) {
		bool ok = true;
		if (b.unset() || e.unset()) {
			o << "<i0>,<j0> and <i1>,<j1> are mandatory\n";
			ok = false;
		}
		if (!(b.second == e.second && b.first == 0)) {
			if (b.first > e.first || b.first != 0) {
				o << fmt("bad row/column range: % .. %\n")%b%e;
				ok = false;
			}
		}
		if (b.second != e.second) {
			/* should work, ideally, but for now this is
			 * completely unexpected in the source code
			 */
			o << "come back someday when nbys != 1 is supported\n";
			ok = false;
		}
		if (task == "mksol") {
			if (scol == (unsigned int) -1) {
				o << "mksol requires --sc\n";
				ok = false;
			}
		} else {
			if (task != "slave") {
				o << "--task is \"slave\" or \"mksol\"\n";
				ok = false;
			}
			if (scol != (unsigned int) -1) {
				o << "--sc is mksol-only\n";
				ok = false;
			}
		}
#ifdef	ENABLE_PTHREADS
		if (nt == -1) {
			o << "--nthreads not specified, will use default\n";
		}
#endif
		return ok;
	}
	void trigger() {
		if (nt == -1) {
#ifdef	ENABLE_PTHREADS
			/* Defaulting to two threads */
			nt = 2;
#else
			nt = 1;
#endif
		}
	}
};

#endif	/* SLAVE_ARGUMENTS_HPP_ */
