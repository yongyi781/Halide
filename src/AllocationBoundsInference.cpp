#include "AllocationBoundsInference.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Bounds.h"
#include "CSE.h"
#include "Simplify.h"
#include "IREquality.h"

namespace Halide {
namespace Internal {

using std::map;
using std::string;
using std::vector;
using std::pair;
using std::make_pair;
using std::set;

// Figure out the region touched of each buffer, and deposit them as
// let statements outside of each realize node, or at the top level if
// they're not internal allocations.

class AllocationInference : public IRMutator {
    using IRMutator::visit;

    const map<string, Function> &env;
    set<string> touched_by_extern;

    void visit(const Realize *op) {
        map<string, Function>::const_iterator iter = env.find(op->name);
        assert (iter != env.end());
        Function f = iter->second;

        Box b = box_touched(op->body, op->name);

        if (touched_by_extern.count(f.name())) {
            // The region touched is at least the region required at this
            // loop level of the first stage (this is important for inputs
            // and outputs to extern stages).
            Box required(op->bounds.size());
            for (size_t i = 0; i < required.size(); i++) {
                string prefix = op->name + ".s0." + f.args()[i];
                required[i] = Interval(Variable::make(Int(32), prefix + ".min"),
                                       Variable::make(Int(32), prefix + ".max"));
            }

            merge_boxes(b, required);
        }

        Stmt new_body = mutate(op->body);
        stmt = Realize::make(op->name, op->types, op->bounds, new_body);

        assert(b.size() == op->bounds.size());
        for (size_t i = 0; i < b.size(); i++) {
            string prefix = op->name + "." + f.args()[i];
            string min_name = prefix + ".min_realized";
            string max_name = prefix + ".max_realized";
            string extent_name = prefix + ".extent_realized";
            Expr min = simplify(b[i].min);
            Expr max = simplify(b[i].max);
            Expr extent = simplify((max - min) + 1);
            stmt = LetStmt::make(extent_name, extent, stmt);
            stmt = LetStmt::make(min_name, min, stmt);
            stmt = LetStmt::make(max_name, max, stmt);
        }
    }

public:
    AllocationInference(const map<string, Function> &e) : env(e) {
        // Figure out which buffers are touched by extern stages
        for (map<string, Function>::const_iterator iter = e.begin();
             iter != e.end(); ++iter) {
            Function f = iter->second;
            if (f.has_extern_definition()) {
                touched_by_extern.insert(f.name());
                for (size_t i = 0; i < f.extern_arguments().size(); i++) {
                    ExternFuncArgument arg = f.extern_arguments()[i];
                    if (!arg.is_func()) continue;
                    Function input(arg.func);
                    touched_by_extern.insert(input.name());
                }
            }
        }
    }
};

Stmt allocation_bounds_inference(Stmt s, const map<string, Function> &env) {
    AllocationInference inf(env);
    s = inf.mutate(s);
    return s;
}

}
}
