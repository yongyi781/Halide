#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRPrinter.h"
#include "IRMutator.h"
#include "Scope.h"
#include "Var.h"
#include "Debug.h"
#include "ModulusRemainder.h"
#include "Substitute.h"
#include "Bounds.h"
#include <iostream>

namespace Halide {
namespace Internal {

using std::string;
using std::map;
using std::pair;
using std::make_pair;
using std::ostringstream;

// Immediates and broadcasts of immediates
bool is_simple_const(Expr e) {
    return (!e.as<Cast>()) && is_const(e);
}

// Is a constant representable as a certain type
int do_indirect_int_cast(Type t, int x) {
    if (t == UInt(1)) {
        return x ? 1 : 0;
    } else if (t.is_int() || t.is_uint()) {
        return int_cast_constant(t, x);
    } else if (t == Float(32)) {
        return (int)((float)x);
    } else if (t == Float(64)) {
        return (int)((double)x);
    } else {
        assert(false && "Can't do an indirect int cast via this type");
        return 0;
    }
}

class Simplify : public IRMutator {
public:
    Simplify(bool r) : remove_dead_lets(r) {}
private:
    bool remove_dead_lets;

    struct VarInfo {
        Expr replacement;
        int old_uses, new_uses;
    };

    Scope<VarInfo> var_info;
    Scope<ModulusRemainder> alignment_info;
    Scope<Interval> bounds_info;

    using IRMutator::visit;

    void visit(const IntImm *op) {
        IRMutator::visit(op);
    }

    void visit(const FloatImm *op) {
        IRMutator::visit(op);
    }

    bool const_float(Expr e, float *f) {
        const FloatImm *c = e.as<FloatImm>();
        if (c) {
            *f = c->value;
            return true;
        } else {
            return false;
        }
    }

    bool const_int(Expr e, int *i) {
        const IntImm *c = e.as<IntImm>();
        if (c) {
            *i = c->value;
            return true;
        } else {
            return false;
        }
    }

    Expr is_round_up(Expr e, int *factor) {
        const Mul *mul = e.as<Mul>();
        if (!mul) return Expr();
        if (!const_int(mul->b, factor)) return Expr();
        const Div *div = mul->a.as<Div>();
        if (!div) return Expr();
        if (!is_const(div->b, *factor)) return Expr();
        const Add *add = div->a.as<Add>();
        if (!add) return Expr();
        if (!is_const(add->b, (*factor)-1)) return Expr();
        return add->a;
    }

    /* Recognise an integer or cast integer and fetch its value.
     * Only matches if the number of bits of the cast integer does not exceed
     * the number of bits of an int in the compiler, because simplification
     * uses type int for its calculations. */
    bool const_castint(Expr e, int *i) {
        const IntImm *intimm = e.as<IntImm>();
        const Cast *cast = e.as<Cast>();
        if (intimm) {
            *i = intimm->value;
            return true;
        } else if (cast && (cast->type.is_int() || cast->type.is_uint()) &&
                   cast->type.bits <= (int) (sizeof(int) * 8)) {
            const IntImm *imm = cast->value.as<IntImm>();
            if (imm) {
                // When fetching a cast integer, ensure that the return
                // value is in the correct range (i.e. the canonical value) for the cast type.
                *i = int_cast_constant(cast->type, imm->value);
                return true;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }

    void visit(const Cast *op) {
        Expr value = mutate(op->value);
        const Cast *cast = value.as<Cast>();
        float f = 0.0f;
        int i = 0;
        if (value.type() == op->type) {
            expr = value;
        } else if (op->type == Int(32) && const_float(value, &f)) {
            expr = IntImm::make((int)f);
        } else if (op->type == Float(32) && const_int(value, &i)) {
            expr = FloatImm::make((float)i);
        } else if (op->type == Int(32) && cast && const_int(cast->value, &i)) {
            // Cast to something then back to int
            expr = do_indirect_int_cast(cast->type, i);
        } else if (!op->type.is_float() &&
                   op->type.bits <= 32 &&
                   const_int(value, &i) &&
                   do_indirect_int_cast(op->type, i) != i) {
            // Rewrite things like cast(UInt(8), 256) to cast(UInt(8),
            // 0), so any later peephole matching that ignores casts
            // doesn't get confused.
            expr = Cast::make(op->type, do_indirect_int_cast(op->type, i));
        } else if (value.same_as(op->value)) {
            expr = op;
        } else {
            expr = Cast::make(op->type, value);
        }
    }

    void visit(const Variable *op) {
        if (var_info.contains(op->name)) {
            VarInfo &info = var_info.ref(op->name);

            // if replacement is defined, we should substitute it in (unless
            // it's a var that has been hidden by a nested scope).
            if (info.replacement.defined()) {
                assert(info.replacement.type() == op->type);
                expr = info.replacement;
                info.new_uses++;
            } else {
                // This expression was not something deemed
                // substitutable - no replacement is defined.
                expr = op;
                info.old_uses++;
            }
        } else {
            // We never encountered a let that defines this var. Must
            // be a uniform. Don't touch it.
            expr = op;
        }
    }

    void visit(const Add *op) {
        int ia = 0, ib = 0, ic = 0;
        float fa = 0.0f, fb = 0.0f;

        Expr a = mutate(op->a), b = mutate(op->b);

        // rearrange const + varying to varying + const, to cut down
        // on cases to check
        if (is_simple_const(a) && !is_simple_const(b)) std::swap(a, b);

        // Rearrange a + min or a + max to min + a or max + a to cut down on cases to check
        if (b.as<Min>() || b.as<Max>()) {
            std::swap(a, b);
        }

        const Ramp *ramp_a = a.as<Ramp>();
        const Ramp *ramp_b = b.as<Ramp>();
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Mul *mul_a = a.as<Mul>();
        const Mul *mul_b = b.as<Mul>();

        const Div *div_a = a.as<Div>();

        const Div *div_a_a = mul_a ? mul_a->a.as<Div>() : NULL;
        const Mod *mod_a = a.as<Mod>();
        const Mod *mod_b = b.as<Mod>();

        const Mul *mul_a_a = add_a ? add_a->a.as<Mul>(): NULL;
        const Mod *mod_a_a = add_a ? add_a->a.as<Mod>(): NULL;
        const Mul *mul_a_b = add_a ? add_a->b.as<Mul>(): NULL;
        const Mod *mod_a_b = add_a ? add_a->b.as<Mod>(): NULL;

        const Min *min_a = a.as<Min>();
        const Max *max_a = a.as<Max>();
        const Sub *sub_a_a = min_a ? min_a->a.as<Sub>() : NULL;
        const Sub *sub_a_b = min_a ? min_a->b.as<Sub>() : NULL;
        const Add *add_a_a = min_a ? min_a->a.as<Add>() : NULL;
        const Add *add_a_b = min_a ? min_a->b.as<Add>() : NULL;
        sub_a_a = max_a ? max_a->a.as<Sub>() : sub_a_a;
        sub_a_b = max_a ? max_a->b.as<Sub>() : sub_a_b;
        add_a_a = max_a ? max_a->a.as<Add>() : add_a_a;
        add_a_b = max_a ? max_a->b.as<Add>() : add_a_b;

        add_a_a = div_a ? div_a->a.as<Add>() : add_a_a;

        if (const_int(a, &ia) &&
            const_int(b, &ib)) {
            expr = ia + ib;
            // const int + const int
        } else if (const_float(a, &fa) &&
                   const_float(b, &fb)) {
            // const float + const float
            expr = fa + fb;
        } else if (is_zero(b)) {
            expr = a;
        } else if (is_zero(a)) {
            expr = b;
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, ((unsigned int) ia) + ((unsigned int) ib));
            } else {
                expr = make_const(op->type, ia + ib);
            }
        } else if (ramp_a && ramp_b) {
            // Ramp + Ramp
            expr = mutate(Ramp::make(ramp_a->base + ramp_b->base,
                                   ramp_a->stride + ramp_b->stride, ramp_a->width));
        } else if (ramp_a && broadcast_b) {
            // Ramp + Broadcast
            expr = mutate(Ramp::make(ramp_a->base + broadcast_b->value,
                                   ramp_a->stride, ramp_a->width));
        } else if (broadcast_a && ramp_b) {
            // Broadcast + Ramp
            expr = mutate(Ramp::make(broadcast_a->value + ramp_b->base,
                                   ramp_b->stride, ramp_b->width));
        } else if (broadcast_a && broadcast_b) {
            // Broadcast + Broadcast
            expr = Broadcast::make(mutate(broadcast_a->value + broadcast_b->value),
                                 broadcast_a->width);
        } else if (add_a && is_simple_const(add_a->b)) {
            // In ternary expressions, pull constants outside
            if (is_simple_const(b)) expr = mutate(add_a->a + (add_a->b + b));
            else expr = mutate((add_a->a + b) + add_a->b);
        } else if (add_b && is_simple_const(add_b->b)) {
            expr = mutate((a + add_b->a) + add_b->b);
        } else if (sub_a && is_simple_const(sub_a->a) && is_simple_const(b)) {
            expr = mutate((sub_a->a + b) - sub_a->b);

        } else if (sub_a && equal(b, sub_a->b)) {
            // Additions that cancel an inner term
            // (a - b) + b
            expr = sub_a->a;
        } else if (sub_b && equal(a, sub_b->b)) {
            // a + (b - a)
            expr = sub_b->a;

        } else if (min_a && sub_a_b && equal(sub_a_b->b, b)) {
            // min(a, b-c) + c -> min(a+c, b)
            expr = mutate(Min::make(Add::make(min_a->a, b), sub_a_b->a));
        } else if (min_a && sub_a_a && equal(sub_a_a->b, b)) {
            // min(a-c, b) + c -> min(a, b+c)
            expr = mutate(Min::make(sub_a_a->a, Add::make(min_a->b, b)));
        } else if (max_a && sub_a_b && equal(sub_a_b->b, b)) {
            // max(a, b-c) + c -> max(a+c, b)
            expr = mutate(Max::make(Add::make(max_a->a, b), sub_a_b->a));
        } else if (max_a && sub_a_a && equal(sub_a_a->b, b)) {
            // max(a-c, b) + c -> max(a, b+c)
            expr = mutate(Max::make(sub_a_a->a, Add::make(max_a->b, b)));

        } else if (min_a && add_a_b && const_int(add_a_b->b, &ia) && const_int(b, &ib) && ia + ib == 0) {
            // min(a, b + (-2)) + 2 -> min(a + 2, b)
            expr = mutate(Min::make(Add::make(min_a->a, b), add_a_b->a));
        } else if (min_a && add_a_a && const_int(add_a_a->b, &ia) && const_int(b, &ib) && ia + ib == 0) {
            // min(a + (-2), b) + 2 -> min(a, b + 2)
            expr = mutate(Min::make(add_a_a->a, Add::make(min_a->b, b)));
        } else if (max_a && add_a_b && const_int(add_a_b->b, &ia) && const_int(b, &ib) && ia + ib == 0) {
            // max(a, b + (-2)) + 2 -> max(a + 2, b)
            expr = mutate(Max::make(Add::make(max_a->a, b), add_a_b->a));
        } else if (max_a && add_a_a && const_int(add_a_a->b, &ia) && const_int(b, &ib) && ia + ib == 0) {
            // max(a + (-2), b) + 2 -> max(a, b + 2)
            expr = mutate(Max::make(add_a_a->a, Add::make(max_a->b, b)));
        } else if (div_a && add_a_a &&
                   const_int(add_a_a->b, &ia) &&
                   const_int(div_a->b, &ib) &&
                   const_int(b, &ic)) {
            // ((a + ia) / ib + ic) -> (a + (ia + ib*ic)) / ib
            expr = mutate((add_a_a->a + (ia + ib*ic)) / ib);
        } else if (mul_a && mul_b && equal(mul_a->a, mul_b->a)) {
            // Pull out common factors a*x + b*x
            expr = mutate(mul_a->a * (mul_a->b + mul_b->b));
        } else if (mul_a && mul_b && equal(mul_a->b, mul_b->a)) {
            expr = mutate(mul_a->b * (mul_a->a + mul_b->b));
        } else if (mul_a && mul_b && equal(mul_a->b, mul_b->b)) {
            expr = mutate(mul_a->b * (mul_a->a + mul_b->a));
        } else if (mul_a && mul_b && equal(mul_a->a, mul_b->b)) {
            expr = mutate(mul_a->a * (mul_a->b + mul_b->a));
        } else if (mod_a && mul_b && equal(mod_a->b, mul_b->b)) {
            // (x%3) + y*3 -> y*3 + x%3
            expr = mutate(b + a);
        } else if (mul_a && mod_b && div_a_a &&
                   equal(mul_a->b, div_a_a->b) &&
                   equal(mul_a->b, mod_b->b) &&
                   equal(div_a_a->a, mod_b->a)) {
            // (x/3)*3 + x%3 -> x
            expr = div_a_a->a;
        } else if (add_a && mul_a_a && mod_b &&
                   equal(mul_a_a->b, mod_b->b)) {
            // ((x*3) + y) + z%3 -> (x*3 + z%3) + y
            expr = mutate((add_a->a + b) + add_a->b);
        } else if (add_a && mod_a_a && mul_b &&
                   equal(mod_a_a->b, mul_b->b)) {
            // ((x%3) + y) + z*3 -> (z*3 + x%3) + y
            expr = mutate((b + add_a->a) + add_a->b);
        } else if (add_a && mul_a_b && mod_b &&
                   equal(mul_a_b->b, mod_b->b)) {
            // (y + (x*3)) + z%3 -> y + (x*3 + z%3)
            expr = mutate(add_a->a + (add_a->b + b));
        } else if (add_a && mod_a_b && mul_b &&
                   equal(mod_a_b->b, mul_b->b)) {
            // (y + (x%3)) + z*3 -> y + (z*3 + x%3)
            expr = mutate(add_a->a + (b + add_a->b));
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            // If we've made no changes, and can't find a rule to apply, return the operator unchanged.
            expr = op;
        } else {
            expr = Add::make(a, b);
        }
    }

    void visit(const Sub *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        int ia = 0, ib = 0;
        float fa = 0.0f, fb = 0.0f;

        const Ramp *ramp_a = a.as<Ramp>();
        const Ramp *ramp_b = b.as<Ramp>();
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Mul *mul_a = a.as<Mul>();
        const Mul *mul_b = b.as<Mul>();

        const Min *min_b = b.as<Min>();
        const Add *add_b_a = min_b ? min_b->a.as<Add>() : NULL;
        const Add *add_b_b = min_b ? min_b->b.as<Add>() : NULL;

        const Min *min_a = a.as<Min>();
        const Add *add_a_a = min_a ? min_a->a.as<Add>() : NULL;
        const Add *add_a_b = min_a ? min_a->b.as<Add>() : NULL;

        if (is_zero(b)) {
            expr = a;
        } else if (equal(a, b)) {
            expr = make_zero(op->type);
        } else if (const_int(a, &ia) && const_int(b, &ib)) {
            expr = ia - ib;
        } else if (const_float(a, &fa) && const_float(b, &fb)) {
            expr = fa - fb;
        } else if (const_int(b, &ib)) {
            expr = mutate(a + (-ib));
        } else if (const_float(b, &fb)) {
            expr = mutate(a + (-fb));
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, ((unsigned int) ia) - ((unsigned int) ib));
            } else {
                expr = make_const(op->type, ia - ib);
            }
        } else if (ramp_a && ramp_b) {
            // Ramp - Ramp
            expr = mutate(Ramp::make(ramp_a->base - ramp_b->base,
                                   ramp_a->stride - ramp_b->stride, ramp_a->width));
        } else if (ramp_a && broadcast_b) {
            // Ramp - Broadcast
            expr = mutate(Ramp::make(ramp_a->base - broadcast_b->value,
                                   ramp_a->stride, ramp_a->width));
        } else if (broadcast_a && ramp_b) {
            // Broadcast - Ramp
            expr = mutate(Ramp::make(broadcast_a->value - ramp_b->base,
                                   make_zero(ramp_b->stride.type())- ramp_b->stride,
                                   ramp_b->width));
        } else if (broadcast_a && broadcast_b) {
            // Broadcast + Broadcast
            expr = Broadcast::make(mutate(broadcast_a->value - broadcast_b->value),
                                 broadcast_a->width);
        } else if (add_a && equal(add_a->b, b)) {
            // Ternary expressions where a term cancels
            expr = add_a->a;
        } else if (add_a && equal(add_a->a, b)) {
            expr = add_a->b;
        } else if (add_b && equal(add_b->b, a)) {
            expr = mutate(make_zero(add_b->a.type()) - add_b->a);
        } else if (add_b && equal(add_b->a, a)) {
            expr = mutate(make_zero(add_b->a.type()) - add_b->b);
        } else if (add_a && is_simple_const(add_a->b)) {
            // In ternary expressions, pull constants outside
            if (is_simple_const(b)) expr = mutate(add_a->a + (add_a->b - b));
            else expr = mutate((add_a->a - b) + add_a->b);
        } else if (add_b && is_simple_const(add_b->b)) {
            expr = mutate((a - add_b->a) - add_b->b);
        } else if (sub_a && is_simple_const(sub_a->a) && is_simple_const(b)) {
            expr = mutate((sub_a->a - b) - sub_a->b);
        } else if (sub_b && is_simple_const(sub_b->b)) {
            if (is_simple_const(a)) expr = mutate((a + sub_b->b) - sub_b->a);
            expr = mutate((a - sub_b->a) + sub_b->b);
        } else if (mul_a && mul_b && equal(mul_a->a, mul_b->a)) {
            // Pull out common factors a*x + b*x
            expr = mutate(mul_a->a * (mul_a->b - mul_b->b));
        } else if (mul_a && mul_b && equal(mul_a->b, mul_b->a)) {
            expr = mutate(mul_a->b * (mul_a->a - mul_b->b));
        } else if (mul_a && mul_b && equal(mul_a->b, mul_b->b)) {
            expr = mutate(mul_a->b * (mul_a->a - mul_b->a));
        } else if (mul_a && mul_b && equal(mul_a->a, mul_b->b)) {
            expr = mutate(mul_a->a * (mul_a->b - mul_b->a));
        } else if (add_a && add_b && equal(add_a->b, add_b->b)) {
            // Quaternary expressions where a term cancels
            // (a + b) - (c + b) -> a - c
            expr = mutate(add_a->a - add_b->a);
        } else if (add_a && add_b && equal(add_a->a, add_b->a)) {
            // (a + b) - (a + c) -> b - c
            expr = mutate(add_a->b - add_b->b);
        } else if (add_a && add_b && equal(add_a->a, add_b->b)) {
            // (a + b) - (c + a) -> b - c
            expr = mutate(add_a->b - add_b->a);
        } else if (add_a && add_b && equal(add_a->b, add_b->a)) {
            // (b + a) - (a + c) -> b - c
            expr = mutate(add_a->a - add_b->b);
        } else if (sub_a && sub_b && equal(sub_a->b, sub_b->b)) {
            // (a - b) - (c - b) -> a - c
            expr = mutate(sub_a->a - sub_b->a);
        } else if (sub_a && sub_b && equal(sub_a->a, sub_b->a)) {
            // (b - a) - (b - c) -> c - a
            expr = mutate(sub_b->b - sub_a->b);
        } else if (min_b && add_b_a && equal(a, add_b_a->a)) {
            // Quaternary expressions involving mins where a term
            // cancels. These are important for bounds inference
            // simplifications.
            // a - min(a + b, c) -> max(-b, a-c)
            expr = mutate(max(0 - add_b_a->b, a - min_b->b));
        } else if (min_b && add_b_a && equal(a, add_b_a->b)) {
            // a - min(b + a, c) -> max(-b, a-c)
            expr = mutate(max(0 - add_b_a->a, a - min_b->b));
        } else if (min_b && add_b_b && equal(a, add_b_b->a)) {
            // a - min(c, a + b) -> max(-b, a-c)
            expr = mutate(max(0 - add_b_b->b, a - min_b->a));
        } else if (min_b && add_b_b && equal(a, add_b_b->b)) {
            // a - min(c, b + a) -> max(-b, a-c)
            expr = mutate(max(0 - add_b_b->a, a - min_b->a));
        } else if (min_a && add_a_a && equal(b, add_a_a->a)) {
            // min(a + b, c) - a -> min(b, c-a)
            expr = mutate(min(add_a_a->b, min_a->b - b));
        } else if (min_a && add_a_a && equal(b, add_a_a->b)) {
            // min(b + a, c) - a -> min(b, c-a)
            expr = mutate(min(add_a_a->a, min_a->b - b));
        } else if (min_a && add_a_b && equal(b, add_a_b->a)) {
            // min(c, a + b) - a -> min(b, c-a)
            expr = mutate(min(add_a_b->b, min_a->a - b));
        } else if (min_a && add_a_b && equal(b, add_a_b->b)) {
            // min(c, b + a) - a -> min(b, c-a)
            expr = mutate(min(add_a_b->a, min_a->a - b));
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Sub::make(a, b);
        }
    }

    void visit(const Mul *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        if (is_simple_const(a)) std::swap(a, b);

        int ia = 0, ib = 0;
        float fa = 0.0f, fb = 0.0f;

        const Ramp *ramp_a = a.as<Ramp>();
        const Ramp *ramp_b = b.as<Ramp>();
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Mul *mul_a = a.as<Mul>();

        if (is_zero(b)) {
            expr = b;
        } else if (is_one(b)) {
            expr = a;
        } else if (const_int(a, &ia) && const_int(b, &ib)) {
            expr = ia*ib;
        } else if (const_float(a, &fa) && const_float(b, &fb)) {
            expr = fa*fb;
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, ((unsigned int) ia) * ((unsigned int) ib));
            } else {
                expr = make_const(op->type, ia * ib);
            }
        } else if (broadcast_a && broadcast_b) {
            expr = Broadcast::make(mutate(broadcast_a->value * broadcast_b->value), broadcast_a->width);
        } else if (ramp_a && broadcast_b) {
            Expr m = broadcast_b->value;
            expr = mutate(Ramp::make(ramp_a->base * m, ramp_a->stride * m, ramp_a->width));
        } else if (broadcast_a && ramp_b) {
            Expr m = broadcast_a->value;
            expr = mutate(Ramp::make(m * ramp_b->base, m * ramp_b->stride, ramp_b->width));
        } else if (add_a && is_simple_const(add_a->b) && is_simple_const(b)) {
            expr = mutate(add_a->a * b + add_a->b * b);
        } else if (mul_a && is_simple_const(mul_a->b) && is_simple_const(b)) {
            expr = mutate(mul_a->a * (mul_a->b * b));
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Mul::make(a, b);
        }
    }

    void visit(const Div *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        int ia = 0, ib = 0, ic = 0;
        float fa = 0.0f, fb = 0.0f;

        const Mul *mul_a = a.as<Mul>();
        const Add *add_a = a.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Div *div_a = a.as<Div>();
        const Div *div_a_a = NULL;
        const Mul *mul_a_a = NULL;
        const Mul *mul_a_b = NULL;
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Ramp *ramp_a = a.as<Ramp>();
        const Broadcast *broadcast_b = b.as<Broadcast>();

        if (add_a) {
            div_a_a = add_a->a.as<Div>();
            mul_a_a = add_a->a.as<Mul>();
            mul_a_b = add_a->b.as<Mul>();
        } else if (sub_a) {
            mul_a_a = sub_a->a.as<Mul>();
            mul_a_b = sub_a->b.as<Mul>();
        }

        if (ramp_a) {
            mul_a_a = ramp_a->base.as<Mul>();
        }

        if (op->type == Int(32) && const_int(b, &ib) && !is_const(a)) {
            // Check for bounded numerators divided by constant
            // denominators.
            Interval bounds = bounds_of_expr_in_scope(a, bounds_info);
            if (bounds.min.defined() &&
                bounds.max.defined()) {
                bounds.min = mutate(bounds.min);
                bounds.max = mutate(bounds.max);
                int num_min = 0, num_max = 0;
                if (const_int(bounds.min, &num_min) &&
                    const_int(bounds.max, &num_max) &&
                    div_imp(num_max, ib) == div_imp(num_min, ib)) {
                    expr = div_imp(num_max, ib);
                    return;
                }
            }
        }

        if (is_zero(a)) {
            expr = a;
        } else if (is_one(b)) {
            expr = a;
        } else if (equal(a, b)) {
            expr = make_one(a.type());
        } else if (const_int(a, &ia) && const_int(b, &ib)) {
            expr = div_imp(ia,ib);
        } else if (const_float(a, &fa) && const_float(b, &fb)) {
            expr = fa/fb;
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, ((unsigned int)ia)/((unsigned int)ib));
            } else {
                expr = make_const(op->type, div_imp(ia,ib)); //Use the definition::make of division
            }
        } else if (broadcast_a && broadcast_b) {
            expr = mutate(Broadcast::make(broadcast_a->value / broadcast_b->value, broadcast_a->width));
        } else if (ramp_a && broadcast_b &&
                   const_int(broadcast_b->value, &ib) &&
                   const_int(ramp_a->stride, &ia) && ((ia % ib) == 0)) {
            // ramp(x, ia, w) / broadcast(ib, w) -> ramp(x/ib, ia/ib, w) when ib divides ia
            expr = mutate(Ramp::make(ramp_a->base/ib, ia/ib, ramp_a->width));
        } else if (ramp_a && broadcast_b &&
                   mul_a_a && const_int(mul_a_a->b, &ia) &&
                   const_int(broadcast_b->value, &ib) &&
                   const_int(ramp_a->stride, &ic) &&
                   ia == ib &&
                   (ic * (broadcast_b->width - 1)) < ia) {
            // ramp(x*a, c, w) / broadcast(a, w) -> broadcast(x, w) when c*(w-1) < a
            expr = mutate(Broadcast::make(mul_a_a->a, broadcast_b->width));
        } else if (ramp_a && broadcast_b &&
                   mul_a_a && const_int(mul_a_a->b, &ia) &&
                   const_int(broadcast_b->value, &ib) &&
                   const_int(ramp_a->stride, &ic) &&
                   (ib % ia) == 0 &&
                   (ic * (broadcast_b->width - 1)) < ia) {
            // ramp(x*a, c, w) / broadcast(b, w) -> broadcast(x / (b/a), w) when c*(w-1) < a and a divides d
            expr = mutate(Broadcast::make(mul_a_a->a / (ib / ia), broadcast_b->width));
        } else if (div_a && const_int(div_a->b, &ia) && const_int(b, &ib)) {
            // (x / 3) / 4 -> x / 12
            expr = mutate(div_a->a / (ia*ib));
        } else if (div_a_a && add_a &&
                   const_int(div_a_a->b, &ia) &&
                   const_int(add_a->b, &ib) &&
                   const_int(b, &ic)) {
            // (x / ia + ib) / ic -> (x + ia*ib) / (ia*ic)
            expr = mutate((div_a_a->a + ia*ib) / (ia*ic));
        } else if (mul_a && const_int(mul_a->b, &ia) && const_int(b, &ib) &&
                   ia && ib && (ia % ib == 0 || ib % ia == 0)) {
            if (ia % ib == 0) {
                // (x * 4) / 2 -> x * 2
                expr = mutate(mul_a->a * (ia / ib));
            } else {
                // (x * 2) / 4 -> x / 2
                expr = mutate(mul_a->a / (ib / ia));
            }
        } else if (add_a && mul_a_a && const_int(mul_a_a->b, &ia) && const_int(b, &ib) &&
                   ib && (ia % ib == 0)) {
            // Pull terms that are a multiple of the divisor out
            // (x*4 + y) / 2 -> x*2 + y/2
            expr = mutate((mul_a_a->a * (ia/ib)) + (add_a->b / b));
        } else if (add_a && mul_a_b && const_int(mul_a_b->b, &ia) && const_int(b, &ib) &&
                   ib && (ia % ib == 0)) {
            // (y + x*4) / 2 -> y/2 + x*2
            expr = mutate((add_a->a / b) + (mul_a_b->a * (ia/ib)));
        } else if (sub_a && mul_a_a && const_int(mul_a_a->b, &ia) && const_int(b, &ib) &&
                   ib && (ia % ib == 0)) {
            // Pull terms that are a multiple of the divisor out
            // (x*4 - y) / 2 -> x*2 - y/2
            expr = mutate((mul_a_a->a * (ia/ib)) - (sub_a->b / b));
        } else if (sub_a && mul_a_b && const_int(mul_a_b->b, &ia) && const_int(b, &ib) &&
                   ib && (ia % ib == 0)) {
            // (y - x*4) / 2 -> y/2 - x*2
            expr = mutate((sub_a->a / b) - (mul_a_b->a * (ia/ib)));
        } else if (b.type().is_float() && is_simple_const(b)) {
            // Convert const float division to multiplication
            // x / 2 -> x * 0.5
            expr = mutate(a * (make_one(b.type()) / b));
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Div::make(a, b);
        }
    }

    void visit(const Mod *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        int ia = 0, ib = 0;
        float fa = 0.0f, fb = 0.0f;
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Mul *mul_a = a.as<Mul>();
        const Add *add_a = a.as<Add>();
        const Mul *mul_a_a = add_a ? add_a->a.as<Mul>() : NULL;
        const Mul *mul_a_b = add_a ? add_a->b.as<Mul>() : NULL;
        const Ramp *ramp_a = a.as<Ramp>();

        // If the RHS is a constant, do modulus remainder analysis on the LHS
        ModulusRemainder mod_rem(0, 1);
        if (const_int(b, &ib) && a.type() == Int(32)) {
            // If the LHS is bounded, we can possibly bail out early
            Interval ia = bounds_of_expr_in_scope(a, bounds_info);
            if (ia.max.defined() && ia.min.defined() &&
                is_one(mutate((ia.max < b) && (ia.min >= 0)))) {
                expr = a;
                return;
            }

            mod_rem = modulus_remainder(a, alignment_info);
        }

        if (const_int(a, &ia) && const_int(b, &ib)) {
            expr = mod_imp(ia, ib);
        } else if (const_float(a, &fa) && const_float(b, &fb)) {
            expr = mod_imp(fa, fb);
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, ((unsigned int)ia) % ((unsigned int)ib));
            } else {
                expr = Cast::make(op->type, mod_imp(ia, ib));
            }
        } else if (broadcast_a && broadcast_b) {
            expr = mutate(Broadcast::make(broadcast_a->value % broadcast_b->value, broadcast_a->width));
        } else if (mul_a && const_int(b, &ib) && const_int(mul_a->b, &ia) && (ia % ib == 0)) {
            // (x * (b*a)) % b -> 0
            expr = make_zero(a.type());
        } else if (add_a && mul_a_a && const_int(mul_a_a->b, &ia) && const_int(b, &ib) && (ia % ib == 0)) {
            // (x * (b*a) + y) % b -> (y % b)
            expr = mutate(add_a->b % ib);
        } else if (add_a && mul_a_b && const_int(mul_a_b->b, &ia) && const_int(b, &ib) && (ia % ib == 0)) {
            // (y + x * (b*a)) % b -> (y % b)
            expr = mutate(add_a->a % ib);
        } else if (const_int(b, &ib) && a.type() == Int(32) && mod_rem.modulus % ib == 0) {
            // ((a*b)*x + c) % a -> c % a
            expr = mod_rem.remainder % ib;
        } else if (ramp_a && const_int(ramp_a->stride, &ia) &&
                   broadcast_b && const_int(broadcast_b->value, &ib) &&
                   ia % ib == 0) {
            // ramp(x, 4, w) % broadcast(2, w)
            expr = mutate(Broadcast::make(ramp_a->base % ib, ramp_a->width));
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Mod::make(a, b);
        }
    }

    void visit(const Min *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        // Move constants to the right to cut down on number of cases to check
        if (is_simple_const(a) && !is_simple_const(b)) {
            std::swap(a, b);
        }

        int ia = 0, ib = 0;
        float fa = 0.0f, fb = 0.0f;
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Div *div_a = a.as<Div>();
        const Div *div_b = b.as<Div>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Min *min_a = a.as<Min>();
        const Min *min_b = b.as<Min>();
        const Min *min_a_a = min_a ? min_a->a.as<Min>() : NULL;
        const Min *min_a_a_a = min_a_a ? min_a_a->a.as<Min>() : NULL;
        const Min *min_a_a_a_a = min_a_a_a ? min_a_a_a->a.as<Min>() : NULL;
        const Max *max_a = a.as<Max>();
        const Max *max_b = b.as<Max>();

        min_a_a = max_a ? max_a->a.as<Min>() : min_a_a;
        const Min *min_b_a = max_b ? max_b->a.as<Min>(): NULL;

        // Detect if the lhs or rhs is a rounding-up operation
        int a_round_up_factor = 0, b_round_up_factor = 0;
        Expr a_round_up = is_round_up(a, &a_round_up_factor);
        Expr b_round_up = is_round_up(b, &b_round_up_factor);

        if (equal(a, b)) {
            expr = a;
            return;
        } else if (const_int(a, &ia) && const_int(b, &ib)) {
            expr = std::min(ia, ib);
            return;
        } else if (const_float(a, &fa) && const_float(b, &fb)) {
            expr = std::min(fa, fb);
            return;
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, std::min(((unsigned int) ia), ((unsigned int) ib)));
            } else {
                expr = make_const(op->type, std::min(ia,ib));
            }
            return;
        } else if (const_castint(b, &ib) && ib == b.type().imax()) {
            // Compute minimum of expression of type and maximum of type --> expression
            expr = a;
            return;
        } else if (const_castint(b, &ib) && ib == b.type().imin()) {
            // Compute minimum of expression of type and minimum of type --> min of type
            expr = b;
            return;
        } else if (broadcast_a && broadcast_b) {
            expr = mutate(Broadcast::make(Min::make(broadcast_a->value, broadcast_b->value), broadcast_a->width));
            return;
        } else if (op->type == Int(32) && is_simple_const(b)) {
            // Try to remove pointless mins that splitting introduces
            Interval ia = bounds_of_expr_in_scope(a, bounds_info);
            if (ia.max.defined() &&
                is_one(mutate(ia.max <= b))) {
                expr = a;
                return;
            }
        }

        if (add_a && const_int(add_a->b, &ia) &&
            add_b && const_int(add_b->b, &ib) &&
            equal(add_a->a, add_b->a)) {
            // min(x + 3, x - 2) -> x - 2
            if (ia > ib) {
                expr = b;
            } else {
                expr = a;
            }
        } else if (add_a && const_int(add_a->b, &ia) && equal(add_a->a, b)) {
            // min(x + 5, x) -> x
            if (ia > 0) {
                expr = b;
            } else {
                expr = a;
            }
        } else if (add_b && const_int(add_b->b, &ib) && equal(add_b->a, a)) {
            // min(x, x + 5) -> x
            if (ib > 0) {
                expr = a;
            } else {
                expr = b;
            }
        } else if (sub_a && sub_b && equal(sub_a->b, sub_b->b) &&
                   const_int(sub_a->a, &ia) && const_int(sub_b->a, &ib)) {
            // min (100-x, 101-x) -> 100-x
            if (ia < ib) {
                expr = a;
            } else {
                expr = b;
            }
        } else if (a_round_up.defined() && equal(a_round_up, b)) {
            // min(((a + 3)/4)*4, a) -> a
            expr = b;
        } else if (a_round_up.defined() && max_b &&
                   equal(a_round_up, max_b->a) && equal(a_round_up_factor, max_b->b)) {
            // min(((a + 3)/4)*4, max(a, 4)) -> max(a, 4)
            expr = b;
        } else if (b_round_up.defined() && equal(b_round_up, a)) {
            // min(a, ((a + 3)/4)*4) -> a
            expr = a;
        } else if (b_round_up.defined() && max_a &&
                   equal(b_round_up, max_a->a) && equal(b_round_up_factor, max_a->b)) {
            // min(max(a, 4), ((a + 3)/4)*4) -> max(a, 4)
            expr = a;
        } else if (max_a && equal(max_a->b, b)) {
            // min(max(x, y), y) -> y
            expr = b;
        } else if (min_a && is_simple_const(min_a->b) && is_simple_const(b)) {
            // min(min(x, 4), 5) -> min(x, 4)
            expr = Min::make(min_a->a, mutate(Min::make(min_a->b, b)));
        } else if (min_a && (equal(min_a->b, b) || equal(min_a->a, b))) {
            // min(min(x, y), y) -> min(x, y)
            expr = a;
        } else if (min_b && (equal(min_b->b, a) || equal(min_b->a, a))) {
            // min(y, min(x, y)) -> min(x, y)
            expr = b;
        } else if (min_a_a && equal(min_a_a->b, b)) {
            // min(min(min(x, y), z), y) -> min(min(x, y), z)
            expr = a;
        } else if (min_a_a_a && equal(min_a_a_a->b, b)) {
            // min(min(min(min(x, y), z), w), y) -> min(min(min(x, y), z), w)
            expr = a;
        } else if (min_a_a_a_a && equal(min_a_a_a_a->b, b)) {
            // min(min(min(min(min(x, y), z), w), l), y) -> min(min(min(min(x, y), z), w), l)
            expr = a;

        } else if (max_a && max_b && equal(max_a->a, max_b->a)) {
            // Distributive law for min/max
            // min(max(x, y), max(x, z)) -> max(min(y, z), x)
            expr = mutate(Max::make(Min::make(max_a->b, max_b->b), max_a->a));
        } else if (max_a && max_b && equal(max_a->a, max_b->b)) {
            // min(max(x, y), max(z, x)) -> max(min(y, z), x)
            expr = mutate(Max::make(Min::make(max_a->b, max_b->a), max_a->a));
        } else if (max_a && max_b && equal(max_a->b, max_b->a)) {
            // min(max(y, x), max(x, z)) -> max(min(y, z), x)
            expr = mutate(Max::make(Min::make(max_a->a, max_b->b), max_a->b));
        } else if (max_a && max_b && equal(max_a->b, max_b->b)) {
            // min(max(y, x), max(z, x)) -> max(min(y, z), x)
            expr = mutate(Max::make(Min::make(max_a->a, max_b->a), max_a->b));
        } else if (min_a && min_b && equal(min_a->a, min_b->a)) {
            // min(min(x, y), min(x, z)) -> min(min(y, z), x)
            expr = mutate(Min::make(Min::make(min_a->b, min_b->b), min_a->a));
        } else if (min_a && min_b && equal(min_a->a, min_b->b)) {
            // min(min(x, y), min(z, x)) -> min(min(y, z), x)
            expr = mutate(Min::make(Min::make(min_a->b, min_b->a), min_a->a));
        } else if (min_a && min_b && equal(min_a->b, min_b->a)) {
            // min(min(y, x), min(x, z)) -> min(min(y, z), x)
            expr = mutate(Min::make(Min::make(min_a->a, min_b->b), min_a->b));
        } else if (min_a && min_b && equal(min_a->b, min_b->b)) {
            // min(min(y, x), min(z, x)) -> min(min(y, z), x)
            expr = mutate(Min::make(Min::make(min_a->a, min_b->a), min_a->b));

        } else if (max_a && min_a_a && max_b && min_b_a && equal(min_a_a->a, min_b_a->a)) {
            // Min of two different clamps of the same thing
            // min(max(min(x, z), y), max(min(x, w), v)) -> max(min(x, min(z, w)), min(y, v))
            expr = mutate(Max::make(Min::make(min_a_a->a, Min::make(min_a_a->b, min_b_a->b)),
                                    Min::make(max_a->b, max_b->b)));

        } else if (add_a && add_b && equal(add_a->b, add_b->b)) {
            // Distributive law for addition
            // min(a + b, c + b) -> min(a, c) + b
            expr = mutate(min(add_a->a, add_b->a)) + add_a->b;
        } else if (add_a && add_b && equal(add_a->a, add_b->a)) {
            // min(b + a, b + c) -> min(a, c) + b
            expr = mutate(min(add_a->b, add_b->b)) + add_a->a;
        } else if (add_a && add_b && equal(add_a->a, add_b->b)) {
            // min(b + a, c + b) -> min(a, c) + b
            expr = mutate(min(add_a->b, add_b->a)) + add_a->a;
        } else if (add_a && add_b && equal(add_a->b, add_b->a)) {
            // min(a + b, b + c) -> min(a, c) + b
            expr = mutate(min(add_a->a, add_b->b)) + add_a->b;

        } else if (div_a && div_b &&
                   const_int(div_a->b, &ia) &&
                   const_int(div_b->b, &ib) &&
                   (ia == ib)) {
            // min(a / 4, b / 4) -> min(a, b) / 4
            if (ia > 0) {
                expr = mutate(min(div_a->a, div_b->a) / ia);
            } else {
                expr = mutate(max(div_a->a, div_b->a) / ia);
            }
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Min::make(a, b);
        }
    }

    void visit(const Max *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        // Move constants to the right to cut down on number of cases to check
        if (is_simple_const(a) && !is_simple_const(b)) {
            std::swap(a, b);
        }

        int ia = 0, ib = 0; //, ic = 0, id = 0;
        float fa = 0.0f, fb = 0.0f;
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Div *div_a = a.as<Div>();
        const Div *div_b = b.as<Div>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Max *max_a = a.as<Max>();
        const Max *max_b = b.as<Max>();
        const Max *max_a_a = max_a ? max_a->a.as<Max>() : NULL;
        const Max *max_a_a_a = max_a_a ? max_a_a->a.as<Max>() : NULL;
        const Max *max_a_a_a_a = max_a_a_a ? max_a_a_a->a.as<Max>() : NULL;
        const Min *min_a = a.as<Min>();
        const Min *min_b = b.as<Min>();

        const Min *min_a_a = max_a ? max_a->a.as<Min>() : NULL;
        const Min *min_b_a = max_b ? max_b->a.as<Min>() : NULL;

        //const Add *add_a_a = div_a ? div_a->a.as<Add>() : NULL;
        //const Add *add_b_a = div_b ? div_b->a.as<Add>() : NULL;

        if (equal(a, b)) {
            expr = a;
        } else if (const_int(a, &ia) && const_int(b, &ib)) {
            expr = std::max(ia, ib);
        } else if (const_float(a, &fa) && const_float(b, &fb)) {
            expr = std::max(fa, fb);
        } else if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (op->type.is_uint()) {
                expr = make_const(op->type, std::max(((unsigned int) ia), ((unsigned int) ib)));
            } else {
                expr = make_const(op->type, std::max(ia, ib));
            }
        } else if (const_castint(b, &ib) && ib == b.type().imin()) {
            // Compute maximum of expression of type and minimum of type --> expression
            expr = a;
        } else if (const_castint(b, &ib) && ib == b.type().imax()) {
            // Compute maximum of expression of type and maximum of type --> max of type
            expr = b;
        } else if (broadcast_a && broadcast_b) {
            expr = mutate(Broadcast::make(Max::make(broadcast_a->value, broadcast_b->value), broadcast_a->width));
        } else if (add_a && const_int(add_a->b, &ia) && add_b && const_int(add_b->b, &ib) && equal(add_a->a, add_b->a)) {
            // max(x + 3, x - 2) -> x - 2
            if (ia > ib) {
                expr = a;
            } else {
                expr = b;
            }
        } else if (add_a && const_int(add_a->b, &ia) && equal(add_a->a, b)) {
            // max(x + 5, x)
            if (ia > 0) {
                expr = a;
            } else {
                expr = b;
            }
        } else if (add_b && const_int(add_b->b, &ib) && equal(add_b->a, a)) {
            // max(x, x + 5)
            if (ib > 0) {
                expr = b;
            } else {
                expr = a;
            }
        } else if (sub_a && sub_b && equal(sub_a->b, sub_b->b) &&
                   const_int(sub_a->a, &ia) && const_int(sub_b->a, &ib)) {
            // max (100-x, 101-x) -> 101-x
            if (ia > ib) {
                expr = a;
            } else {
                expr = b;
            }
        } else if (min_a && equal(min_a->b, b)) {
            // max(min(x, y), y) -> y
            expr = b;
        } else if (max_a && is_simple_const(max_a->b) && is_simple_const(b)) {
            // max(max(x, 4), 5) -> max(x, 5)
            expr = Max::make(max_a->a, mutate(Max::make(max_a->b, b)));
        } else if (max_a && (equal(max_a->b, b) || equal(max_a->a, b))) {
            // max(max(x, y), y) -> max(x, y)
            expr = a;
        } else if (max_b && (equal(max_b->b, a) || equal(max_b->a, a))) {
            // max(y, max(x, y)) -> max(x, y)
            expr = b;
        } else if (max_a_a && equal(max_a_a->b, b)) {
            // max(max(max(x, y), z), y) -> max(max(x, y), z)
            expr = a;
        } else if (max_a_a_a && equal(max_a_a_a->b, b)) {
            // max(max(max(max(x, y), z), w), y) -> max(max(max(x, y), z), w)
            expr = a;
        } else if (max_a_a_a_a && equal(max_a_a_a_a->b, b)) {
            // max(max(max(max(max(x, y), z), w), l), y) -> max(max(max(max(x, y), z), w), l)
            expr = a;

        } else if (max_a && max_b && equal(max_a->a, max_b->a)) {
            // Distributive law for min/max
            // max(max(x, y), max(x, z)) -> max(max(y, z), x)
            expr = mutate(Max::make(Max::make(max_a->b, max_b->b), max_a->a));
        } else if (max_a && max_b && equal(max_a->a, max_b->b)) {
            // max(max(x, y), max(z, x)) -> max(max(y, z), x)
            expr = mutate(Max::make(Max::make(max_a->b, max_b->a), max_a->a));
        } else if (max_a && max_b && equal(max_a->b, max_b->a)) {
            // max(max(y, x), max(x, z)) -> max(max(y, z), x)
            expr = mutate(Max::make(Max::make(max_a->a, max_b->b), max_a->b));
        } else if (max_a && max_b && equal(max_a->b, max_b->b)) {
            // max(max(y, x), max(z, x)) -> max(max(y, z), x)
            expr = mutate(Max::make(Max::make(max_a->a, max_b->a), max_a->b));
        } else if (min_a && min_b && equal(min_a->a, min_b->a)) {
            // max(min(x, y), min(x, z)) -> min(max(y, z), x)
            expr = mutate(Min::make(Max::make(min_a->b, min_b->b), min_a->a));
        } else if (min_a && min_b && equal(min_a->a, min_b->b)) {
            // max(min(x, y), min(z, x)) -> min(max(y, z), x)
            expr = mutate(Min::make(Max::make(min_a->b, min_b->a), min_a->a));
        } else if (min_a && min_b && equal(min_a->b, min_b->a)) {
            // max(min(y, x), min(x, z)) -> min(max(y, z), x)
            expr = mutate(Min::make(Max::make(min_a->a, min_b->b), min_a->b));
        } else if (min_a && min_b && equal(min_a->b, min_b->b)) {
            // max(min(y, x), min(z, x)) -> min(max(y, z), x)
            expr = mutate(Min::make(Max::make(min_a->a, min_b->a), min_a->b));

        } else if (max_a && min_a_a && max_b && min_b_a && equal(min_a_a->a, min_b_a->a)) {
            // Max of two different clamps of the same thing
            // max(max(min(x, z), y), max(min(x, w), v)) -> max(min(x, max(z, w)), max(y, v))
            expr = mutate(Max::make(Min::make(min_a_a->a, Max::make(min_a_a->b, min_b_a->b)),
                                    Max::make(max_a->b, max_b->b)));

        } else if (add_a && add_b && equal(add_a->b, add_b->b)) {
            // Distributive law for addition
            // max(a + b, c + b) -> max(a, c) + b
            expr = mutate(max(add_a->a, add_b->a)) + add_a->b;
        } else if (add_a && add_b && equal(add_a->a, add_b->a)) {
            // max(b + a, b + c) -> max(a, c) + b
            expr = mutate(max(add_a->b, add_b->b)) + add_a->a;
        } else if (add_a && add_b && equal(add_a->a, add_b->b)) {
            // max(b + a, c + b) -> max(a, c) + b
            expr = mutate(max(add_a->b, add_b->a)) + add_a->a;
        } else if (add_a && add_b && equal(add_a->b, add_b->a)) {
            // max(a + b, b + c) -> max(a, c) + b
            expr = mutate(max(add_a->a, add_b->b)) + add_a->b;

        } else if (div_a && div_b &&
                   const_int(div_a->b, &ia) &&
                   const_int(div_b->b, &ib) &&
                   (ia == ib)) {
            // max(a / 4, b / 4) -> max(a, b) / 4
            if (ia > 0) {
                expr = mutate(max(div_a->a, div_b->a) / ia);
            } else {
                expr = mutate(min(div_a->a, div_b->a) / ia);
            }
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Max::make(a, b);
        }
    }

    void visit(const EQ *op) {
        Expr a = mutate(op->a), b = mutate(op->b);
        Expr delta = mutate(a - b);

        const Ramp *ramp_a = a.as<Ramp>();
        const Ramp *ramp_b = b.as<Ramp>();
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Mul *mul_a = a.as<Mul>();
        const Mul *mul_b = b.as<Mul>();

        int ia = 0, ib = 0;

        if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (a.type().is_uint()) {
                expr = make_bool(((unsigned int) ia) == ((unsigned int) ib), op->type.width);
            } else {
                expr = make_bool(ia == ib, op->type.width);
            }
        } else if (is_zero(delta)) {
            expr = const_true(op->type.width);
        } else if (is_simple_const(delta) && ((!ramp_a && !ramp_b) || (ramp_a && ramp_b))) {
            expr = const_false(op->type.width);
        } else if (is_simple_const(a) && !is_simple_const(b)) {
            // Move constants to the right
            expr = mutate(b == a);
        } else if (broadcast_a && broadcast_b) {
            // Push broadcasts outwards
            expr = mutate(Broadcast::make(broadcast_a->value == broadcast_b->value, broadcast_a->width));
        } else if (ramp_a && ramp_b && equal(ramp_a->stride, ramp_b->stride)) {
            // Ramps with matching stride
            Expr bases_match = (ramp_a->base == ramp_b->base);
            expr = mutate(Broadcast::make(bases_match, ramp_a->width));
        } else if (add_a && add_b && equal(add_a->a, add_b->a)) {
            // Subtract a term from both sides
            expr = mutate(add_a->b == add_b->b);
        } else if (add_a && add_b && equal(add_a->a, add_b->b)) {
            expr = mutate(add_a->b == add_b->a);
        } else if (add_a && add_b && equal(add_a->b, add_b->a)) {
            expr = mutate(add_a->a == add_b->b);
        } else if (add_a && add_b && equal(add_a->b, add_b->b)) {
            expr = mutate(add_a->a == add_b->a);
        } else if (sub_a && sub_b && equal(sub_a->a, sub_b->a)) {
            // Add a term to both sides
            expr = mutate(sub_a->b == sub_b->b);
        } else if (sub_a && sub_b && equal(sub_a->b, sub_b->b)) {
            expr = mutate(sub_a->a == sub_b->a);
        } else if (add_a) {
            // Rearrange so that all adds and subs are on the rhs to cut down on further cases
            expr = mutate(add_a->a == (b - add_a->b));
        } else if (sub_a) {
            expr = mutate(sub_a->a == (b + sub_a->b));
        } else if (add_b && equal(add_b->a, a)) {
            // Subtract a term from both sides
            expr = mutate(make_zero(add_b->b.type()) == add_b->b);
        } else if (add_b && equal(add_b->b, a)) {
            expr = mutate(make_zero(add_b->a.type()) == add_b->a);
        } else if (sub_b && equal(sub_b->a, a)) {
            // Add a term to both sides
            expr = mutate(make_zero(sub_b->b.type()) == sub_b->b);
        } else if (mul_a && mul_b && is_simple_const(mul_a->b) && is_simple_const(mul_b->b) && equal(mul_a->b, mul_b->b)) {
            // Divide both sides by a constant
            assert(!is_zero(mul_a->b) && "Multiplication by zero survived constant folding");
            expr = mutate(mul_a->a == mul_b->a);
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = EQ::make(a, b);
        }
    }

    void visit(const NE *op) {
        expr = mutate(Not::make(op->a == op->b));
    }

    void visit(const LT *op) {
        Expr a = mutate(op->a), b = mutate(op->b);
        Expr delta = mutate(a - b);

        const Ramp *ramp_a = a.as<Ramp>();
        const Ramp *ramp_b = b.as<Ramp>();
        const Broadcast *broadcast_a = a.as<Broadcast>();
        const Broadcast *broadcast_b = b.as<Broadcast>();
        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Mul *mul_a = a.as<Mul>();
        const Mul *mul_b = b.as<Mul>();

        int ia = 0, ib = 0;

        // Note that the computation of delta could be incorrect if
        // ia and/or ib are large unsigned integer constants, especially when
        // int is 32 bits on the machine.
        // Explicit comparison is preferred.
        if (const_castint(a, &ia) && const_castint(b, &ib)) {
            if (a.type().is_uint()) {
                expr = make_bool(((unsigned int) ia) < ((unsigned int) ib), op->type.width);
            } else {
                expr = make_bool(ia < ib, op->type.width);
            }
        } else if (const_castint(a, &ia) && ia == a.type().imax()) {
            // Comparing maximum of type < expression of type.  This can never be true.
            expr = const_false(op->type.width);
        } else if (const_castint(b, &ib) && ib == b.type().imin()) {
            // Comparing expression of type < minimum of type.  This can never be true.
            expr = const_false(op->type.width);
        } else if (is_zero(delta) || is_positive_const(delta)) {
            expr = const_false(op->type.width);
        } else if (is_negative_const(delta)) {
            expr = const_true(op->type.width);
        } else if (broadcast_a && broadcast_b) {
            // Push broadcasts outwards
            expr = mutate(Broadcast::make(broadcast_a->value < broadcast_b->value, broadcast_a->width));
        } else if (ramp_a && ramp_b && equal(ramp_a->stride, ramp_b->stride)) {
            // Ramps with matching stride
            Expr bases_lt = (ramp_a->base < ramp_b->base);
            expr = mutate(Broadcast::make(bases_lt, ramp_a->width));
        } else if (add_a && add_b && equal(add_a->a, add_b->a)) {
            // Subtract a term from both sides
            expr = mutate(add_a->b < add_b->b);
        } else if (add_a && add_b && equal(add_a->a, add_b->b)) {
            expr = mutate(add_a->b < add_b->a);
        } else if (add_a && add_b && equal(add_a->b, add_b->a)) {
            expr = mutate(add_a->a < add_b->b);
        } else if (add_a && add_b && equal(add_a->b, add_b->b)) {
            expr = mutate(add_a->a < add_b->a);
        } else if (sub_a && sub_b && equal(sub_a->a, sub_b->a)) {
            // Add a term to both sides
            expr = mutate(sub_a->b < sub_b->b);
        } else if (sub_a && sub_b && equal(sub_a->b, sub_b->b)) {
            expr = mutate(sub_a->a < sub_b->a);
        } else if (add_a) {
            // Rearrange so that all adds and subs are on the rhs to cut down on further cases
            expr = mutate(add_a->a < (b - add_a->b));
        } else if (sub_a) {
            expr = mutate(sub_a->a < (b + sub_a->b));
        } else if (add_b && equal(add_b->a, a)) {
            // Subtract a term from both sides
            expr = mutate(make_zero(add_b->b.type()) < add_b->b);
        } else if (add_b && equal(add_b->b, a)) {
            expr = mutate(make_zero(add_b->a.type()) < add_b->a);
        } else if (add_b && const_int(a, &ia) && const_int(add_b->b, &ib)) {
            // ia < x + ib
            expr = mutate((ia - ib) < add_b->a);
        } else if (sub_b && equal(sub_b->a, a)) {
            // Add a term to both sides
            expr = mutate(sub_b->b < make_zero(sub_b->b.type()));
        } else if (mul_a && mul_b &&
                   is_positive_const(mul_a->b) && is_positive_const(mul_b->b) &&
                   equal(mul_a->b, mul_b->b)) {
            // Divide both sides by a constant
            expr = mutate(mul_a->a < mul_b->a);
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = LT::make(a, b);
        }
    }

    void visit(const LE *op) {
        expr = mutate(!(op->b < op->a));
    }

    void visit(const GT *op) {
        expr = mutate(op->b < op->a);
    }

    void visit(const GE *op) {
        expr = mutate(!(op->a < op->b));
    }

    void visit(const And *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        const LE *le_a = a.as<LE>();
        const LE *le_b = b.as<LE>();
        const LT *lt_a = a.as<LT>();
        const LT *lt_b = b.as<LT>();

        if (is_one(a)) {
            expr = b;
        } else if (is_one(b)) {
            expr = a;
        } else if (is_zero(a)) {
            expr = a;
        } else if (is_zero(b)) {
            expr = b;
        } else if (le_a && le_b && equal(le_a->a, le_b->a)) {
            // (x <= foo && x <= bar) -> x <= min(foo, bar)
            expr = mutate(le_a->a <= min(le_a->b, le_b->b));
        } else if (le_a && le_b && equal(le_a->b, le_b->b)) {
            // (foo <= x && bar <= x) -> max(foo, bar) <= x
            expr = mutate(max(le_a->a, le_b->a) <= le_a->b);
        } else if (lt_a && lt_b && equal(lt_a->a, lt_b->a)) {
            // (x <= foo && x <= bar) -> x <= min(foo, bar)
            expr = mutate(lt_a->a <= min(lt_a->b, lt_b->b));
        } else if (lt_a && lt_b && equal(lt_a->b, lt_b->b)) {
            // (foo <= x && bar <= x) -> max(foo, bar) <= x
            expr = mutate(max(lt_a->a, lt_b->a) <= lt_a->b);
        } else if (equal(a, b)) {
            // x < x
            expr = make_zero(a.type());
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = And::make(a, b);
        }
    }

    void visit(const Or *op) {
        Expr a = mutate(op->a), b = mutate(op->b);

        if (is_one(a)) {
            expr = a;
        } else if (is_one(b)) {
            expr = b;
        } else if (is_zero(a)) {
            expr = b;
        } else if (is_zero(b)) {
            expr = a;
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Or::make(a, b);
        }
    }

    void visit(const Not *op) {
        Expr a = mutate(op->a);

        if (is_one(a)) {
            expr = make_zero(a.type());
        } else if (is_zero(a)) {
            expr = make_one(a.type());
        } else if (const Not *n = a.as<Not>()) {
            // Double negatives cancel
            expr = n->a;
        } else if (const LE *n = a.as<LE>()) {
            expr = LT::make(n->b, n->a);
        } else if (const GE *n = a.as<GE>()) {
            expr = LT::make(n->a, n->b);
        } else if (const LT *n = a.as<LT>()) {
            expr = LE::make(n->b, n->a);
        } else if (const GT *n = a.as<GT>()) {
            expr = LE::make(n->a, n->b);
        } else if (const NE *n = a.as<NE>()) {
            expr = EQ::make(n->a, n->b);
        } else if (const EQ *n = a.as<EQ>()) {
            expr = NE::make(n->a, n->b);
        } else if (const Broadcast *n = a.as<Broadcast>()) {
            expr = mutate(Broadcast::make(!n->value, n->width));
        } else if (a.same_as(op->a)) {
            expr = op;
        } else {
            expr = Not::make(a);
        }
    }

    void visit(const Select *op) {
        Expr condition = mutate(op->condition);
        Expr true_value = mutate(op->true_value);
        Expr false_value = mutate(op->false_value);

        if (is_one(condition)) {
            expr = true_value;
        } else if (is_zero(condition)) {
            expr = false_value;
        } else if (equal(true_value, false_value)) {
            expr = true_value;
        } else if (const Broadcast *b = condition.as<Broadcast>()) {
            // Select of broadcast -> scalar select
            expr = mutate(Select::make(b->value, true_value, false_value));
        } else if (const NE *ne = condition.as<NE>()) {
            // Normalize select(a != b, c, d) to select(a == b, d, c)
            expr = mutate(Select::make(ne->a == ne->b, false_value, true_value));
        } else if (const LE *le = condition.as<LE>()) {
            // Normalize select(a <= b, c, d) to select(b < a, d, c)
            expr = mutate(Select::make(le->b < le->a, false_value, true_value));
        } else if (condition.same_as(op->condition) &&
                   true_value.same_as(op->true_value) &&
                   false_value.same_as(op->false_value)) {
            expr = op;
        } else {
            expr = Select::make(condition, true_value, false_value);
        }
    }

    void visit(const Load *op) {
        // Load of a broadcast should be broadcast of the load
        Expr index = mutate(op->index);
        if (const Broadcast *b = index.as<Broadcast>()) {
            Expr load = Load::make(op->type.element_of(), op->name, b->value, op->image, op->param);
            expr = Broadcast::make(load, b->width);
        } else if (index.same_as(op->index)) {
            expr = op;
        } else {
            expr = Load::make(op->type, op->name, index, op->image, op->param);
        }
    }

    void visit(const Ramp *op) {
        IRMutator::visit(op);
    }

    void visit(const Broadcast *op) {
        IRMutator::visit(op);
    }

    void visit(const Call *op) {
        // Calls implicitly depend on mins and strides of the buffer referenced
        if (op->call_type == Call::Image || op->call_type == Call::Halide) {
            for (size_t i = 0; i < op->args.size(); i++) {
                {
                    ostringstream oss;
                    oss << op->name << ".stride." << i;
                    string stride = oss.str();
                    if (var_info.contains(stride)) {
                        var_info.ref(stride).old_uses++;
                    }
                }
                {
                    ostringstream oss;
                    oss << op->name << ".min." << i;
                    string min = oss.str();
                    if (var_info.contains(min)) {
                        var_info.ref(min).old_uses++;
                    }
                }
            }
        }
        IRMutator::visit(op);
    }



    template<typename T, typename Body>
    Body simplify_let(const T *op) {
        assert(!var_info.contains(op->name) && "Simplify only works on code where every name is unique\n");

        // If the value is trivial, make a note of it in the scope so
        // we can subs it in later
        Expr value = mutate(op->value);
        Body body = op->body;

        // Iteratively peel off certain operations from the let value and push them inside.
        Expr new_value = value;
        string new_name = op->name + ".s";
        Expr new_var = Variable::make(new_value.type(), new_name);
        Expr replacement = new_var;

        debug(4) << "simplify let " << op->name << " = " << value << " in ... " << op->name << " ...\n";

        while (1) {
            const Variable *var = new_value.as<Variable>();
            const Add *add = new_value.as<Add>();
            const Sub *sub = new_value.as<Sub>();
            const Mul *mul = new_value.as<Mul>();
            const Div *div = new_value.as<Div>();
            const Mod *mod = new_value.as<Mod>();
            const Ramp *ramp = new_value.as<Ramp>();
            const Broadcast *broadcast = new_value.as<Broadcast>();

            const Variable *var_b = NULL;
            if (add) {
                var_b = add->b.as<Variable>();
            } else if (sub) {
                var_b = sub->b.as<Variable>();
            } else if (mul) {
                var_b = mul->b.as<Variable>();
            }

            if (is_const(new_value)) {
                replacement = substitute(new_name, new_value, replacement);
                new_value = Expr();
                break;
            } else if (var) {
                replacement = substitute(new_name, var, replacement);
                new_value = Expr();
                break;
            } else if (add && (is_const(add->b) || var_b)) {
                replacement = substitute(new_name, Add::make(new_var, add->b), replacement);
                new_value = add->a;
            } else if (mul && (is_const(mul->b) || var_b)) {
                replacement = substitute(new_name, Mul::make(new_var, mul->b), replacement);
                new_value = mul->a;
            } else if (div && is_const(div->b)) {
                replacement = substitute(new_name, Div::make(new_var, div->b), replacement);
                new_value = div->a;
            } else if (sub && (is_const(sub->b) || var_b)) {
                replacement = substitute(new_name, Sub::make(new_var, sub->b), replacement);
                new_value = sub->a;
            } else if (mod && is_const(mod->b)) {
                replacement = substitute(new_name, Mod::make(new_var, mod->b), replacement);
                new_value = mod->a;
            } else if (ramp && is_const(ramp->stride)) {
                new_var = Variable::make(new_value.type().element_of(), new_name);
                replacement = substitute(new_name, Ramp::make(new_var, ramp->stride, ramp->width), replacement);
                new_value = ramp->base;
            } else if (broadcast) {
                new_var = Variable::make(new_value.type().element_of(), new_name);
                replacement = substitute(new_name, Broadcast::make(new_var, broadcast->width), replacement);
                new_value = broadcast->value;
            } else {
                break;
            }
        }

        if (new_value.same_as(value)) {
            // Nothing to substitute
            new_value = Expr();
            replacement = Expr();
        } else {
            debug(4) << "new let " << new_name << " = " << new_value << " in ... " << replacement << " ...\n";
        }

        VarInfo info;
        info.old_uses = 0;
        info.new_uses = 0;
        info.replacement = replacement;

        var_info.push(op->name, info);

        // Before we enter the body, track the alignment info
        bool new_value_tracked = false;
        if (new_value.defined() && new_value.type() == Int(32)) {
            ModulusRemainder mod_rem = modulus_remainder(new_value, alignment_info);
            alignment_info.push(new_name, mod_rem);
            new_value_tracked = true;
        }
        bool value_tracked = false;
        if (value.type() == Int(32)) {
            ModulusRemainder mod_rem = modulus_remainder(value, alignment_info);
            alignment_info.push(op->name, mod_rem);
            value_tracked = true;
        }

        body = mutate(body);

        if (value_tracked) {
            alignment_info.pop(op->name);
        }
        if (new_value_tracked) {
            alignment_info.pop(new_name);
        }

        info = var_info.get(op->name);
        var_info.pop(op->name);

        Body result = body;

        if (new_value.defined() && info.new_uses > 0) {
            // The new name/value may be used
            result = T::make(new_name, new_value, result);
        }

        if (info.old_uses > 0 || !remove_dead_lets) {
            // The old name is still in use. We'd better keep it as well.
            result = T::make(op->name, value, result);
        }

        // Don't needlessly make a new Let/LetStmt node.  (Here's a
        // piece of template syntax I've never needed before).
        const T *new_op = result.template as<T>();
        if (new_op &&
            new_op->name == op->name &&
            new_op->body.same_as(op->body) &&
            new_op->value.same_as(op->value)) {
            return op;
        }

        return result;

    }


    void visit(const Let *op) {
        expr = simplify_let<Let, Expr>(op);
    }

    void visit(const LetStmt *op) {
        stmt = simplify_let<LetStmt, Stmt>(op);
    }

    void visit(const AssertStmt *op) {
        Expr condition = mutate(op->condition);

        if (is_const(condition, 0)) {
            std::cerr << "This pipeline is guaranteed to fail an assertion at runtime: \n"
                      << Stmt(op) << "\n";
            assert(false);
        } else if (condition.same_as(op->condition)) {
            stmt = op;
        } else {
            stmt = AssertStmt::make(condition, op->message);
        }
    }

    void visit(const Pipeline *op) {
        IRMutator::visit(op);
    }

    void visit(const For *op) {
        Expr new_min = mutate(op->min);
        Expr new_extent = mutate(op->extent);

        const IntImm *new_min_int = new_min.as<IntImm>();
        const IntImm *new_extent_int = new_extent.as<IntImm>();
        bool bounds_tracked = new_min_int && new_extent_int;
        if (bounds_tracked) {
            Interval i = Interval(new_min, new_min_int->value + new_extent_int->value - 1);
            bounds_info.push(op->name, i);
        }

        Stmt new_body = mutate(op->body);

        if (bounds_tracked) {
            bounds_info.pop(op->name);
        }

        if (op->min.same_as(new_min) &&
            op->extent.same_as(new_extent) &&
            op->body.same_as(new_body)) {
            stmt = op;
        } else {
            stmt = For::make(op->name, new_min, new_extent, op->for_type, new_body);
        }
    }

    void visit(const Store *op) {
        IRMutator::visit(op);
    }

    void visit(const Provide *op) {
        // Provides implicitly depend on mins and strides of the buffer referenced
        for (size_t i = 0; i < op->args.size(); i++) {
            {
                ostringstream oss;
                oss << op->name << ".stride." << i;
                string stride = oss.str();
                if (var_info.contains(stride)) {
                    var_info.ref(stride).old_uses++;
                }
            }
            {
                ostringstream oss;
                oss << op->name << ".min." << i;
                string min = oss.str();
                if (var_info.contains(min)) {
                    var_info.ref(min).old_uses++;
                }
            }
        }

        IRMutator::visit(op);
    }

    void visit(const Allocate *op) {
        IRMutator::visit(op);
    }

    void visit(const Realize *op) {
        IRMutator::visit(op);
    }

    void visit(const Block *op) {
        Stmt first = mutate(op->first);

        if (!op->rest.defined()) {
            stmt = first;
        } else {
            Stmt rest = mutate(op->rest);

            // Check if both halves start with a let statement.
            const LetStmt *let_first = first.as<LetStmt>();
            const LetStmt *let_rest = rest.as<LetStmt>();

            // Check if first is a no-op.
            const AssertStmt *noop = first.as<AssertStmt>();
            if (noop && is_const(noop->condition, 1)) {
                stmt = rest;
            } else if (let_first && let_rest &&
                       equal(let_first->value, let_rest->value)) {

                // Do both first and rest start with the same let statement (occurs when unrolling).
                Stmt new_block = mutate(Block::make(let_first->body, let_rest->body));

                // We're just going to use the first name, so if the
                // second name is different we need to rewrite it.
                if (let_rest->name != let_first->name) {
                    new_block = substitute(let_rest->name,
                                           Variable::make(let_first->value.type(), let_first->name),
                                           new_block);
                }

                stmt = LetStmt::make(let_first->name, let_first->value, new_block);
            } else if (op->first.same_as(first) && op->rest.same_as(rest)) {
                stmt = op;
            } else {
                stmt = Block::make(first, rest);
            }

        }
    }
};

Expr simplify(Expr e, bool remove_dead_lets) {
    return Simplify(remove_dead_lets).mutate(e);
}

Stmt simplify(Stmt s, bool remove_dead_lets) {
    return Simplify(remove_dead_lets).mutate(s);
}

void check(Expr a, Expr b) {
    //debug(0) << "Checking that " << a << " -> " << b << "\n";
    Expr simpler = simplify(a);
    if (!equal(simpler, b)) {
        std::cout << std::endl << "Simplification failure: " << std::endl;
        std::cout << "Input: " << a << std::endl;
        std::cout << "Output: " << simpler << std::endl;
        std::cout << "Expected output: " << b << std::endl;
        assert(false);
    }
}

void simplify_test() {
    Expr x = Var("x"), y = Var("y"), z = Var("z"), w = Var("w"), v = Var("v");
    Expr xf = cast<float>(x);
    Expr yf = cast<float>(y);

    // Check the type casting operations.
    assert((int_cast_constant(Int(8), 128) == (int8_t) 128) && "Simplify test failed: int_cast_constant");
    assert((int_cast_constant(UInt(8), -1) == (uint8_t) -1) && "Simplify test failed: int_cast_constant");
    assert((int_cast_constant(Int(16), 65000) == (int16_t) 65000) && "Simplify test failed: int_cast_constant");
    assert((int_cast_constant(UInt(16), 128000) == (uint16_t) 128000) && "Simplify test failed: int_cast_constant");
    assert((int_cast_constant(UInt(16), -53) == (uint16_t) -53) && "Simplify test failed: int_cast_constant");
    assert((int_cast_constant(UInt(32), -53) == (int)((uint32_t) -53)) && "Simplify test failed: int_cast_constant");
    assert((int_cast_constant(Int(32), -53) == -53) && "Simplify test failed: int_cast_constant");

    check(Cast::make(Int(32), Cast::make(Int(32), x)), x);
    check(Cast::make(Float(32), 3), 3.0f);
    check(Cast::make(Int(32), 5.0f), 5);

    check(Cast::make(Int(32), Cast::make(Int(8), 3)), 3);
    check(Cast::make(Int(32), Cast::make(Int(8), 1232)), -48);

    // Check evaluation of constant expressions involving casts
    check(cast(UInt(16), 53) + cast(UInt(16), 87), cast(UInt(16), 140));
    check(cast(Int(8), 127) + cast(Int(8), 1), cast(Int(8), -128));
    check(cast(UInt(16), -1) - cast(UInt(16), 1), cast(UInt(16), 65534));
    check(cast(Int(16), 4) * cast(Int(16), -5), cast(Int(16), -20));
    check(cast(Int(16), 16) / cast(Int(16), 4), cast(Int(16), 4));
    check(cast(Int(16), 23) % cast(Int(16), 5), cast(Int(16), 3));
    check(min(cast(Int(16), 30000), cast(Int(16), -123)), cast(Int(16), -123));
    check(max(cast(Int(16), 30000), cast(Int(16), 65000)), cast(Int(16), 30000));
    check(cast(UInt(16), -1) == cast(UInt(16), 65535), const_true());
    check(cast(UInt(16), 65) == cast(UInt(16), 66), const_false());
    check(cast(UInt(16), -1) < cast(UInt(16), 65535), const_false());
    check(cast(UInt(16), 65) < cast(UInt(16), 66), const_true());
    // Specific checks for 32 bit unsigned expressions - ensure simplifications are actually unsigned.
    // 4000000000 (4 billion) is less than 2^32 but more than 2^31.  As an int, it is negative.
    check(cast(UInt(32), (int) 4000000000UL) + cast(UInt(32), 5), cast(UInt(32), (int) 4000000005UL));
    check(cast(UInt(32), (int) 4000000000UL) - cast(UInt(32), 5), cast(UInt(32), (int) 3999999995UL));
    check(cast(UInt(32), (int) 4000000000UL) / cast(UInt(32), 5), cast(UInt(32), 800000000));
    check(cast(UInt(32), 800000000) * cast(UInt(32), 5), cast(UInt(32), (int) 4000000000UL));
    check(cast(UInt(32), (int) 4000000023UL) % cast(UInt(32), 100), cast(UInt(32), 23));
    check(min(cast(UInt(32), (int) 4000000023UL) , cast(UInt(32), 1000)), cast(UInt(32), (int) 1000));
    check(max(cast(UInt(32), (int) 4000000023UL) , cast(UInt(32), 1000)), cast(UInt(32), (int) 4000000023UL));
    check(cast(UInt(32), (int) 4000000023UL) < cast(UInt(32), 1000), const_false());
    check(cast(UInt(32), (int) 4000000023UL) == cast(UInt(32), 1000), const_false());

    // Check some specific expressions involving div and mod
    check(Expr(23) / 4, Expr(5));
    check(Expr(-23) / 4, Expr(-6));
    check(Expr(-23) / -4, Expr(5));
    check(Expr(23) / -4, Expr(-6));
    check(Expr(-2000000000) / 1000000001, Expr(-2));
    check(Expr(23) % 4, Expr(3));
    check(Expr(-23) % 4, Expr(1));
    check(Expr(-23) % -4, Expr(-3));
    check(Expr(23) % -4, Expr(-1));
    check(Expr(-2000000000) % 1000000001, Expr(2));

    check(3 + x, x + 3);
    check(Expr(3) + Expr(8), 11);
    check(Expr(3.25f) + Expr(7.75f), 11.0f);
    check(x + 0, x);
    check(0 + x, x);
    check(Expr(Ramp::make(x, 2, 3)) + Expr(Ramp::make(y, 4, 3)), Ramp::make(x+y, 6, 3));
    check(Expr(Broadcast::make(4.0f, 5)) + Expr(Ramp::make(3.25f, 4.5f, 5)), Ramp::make(7.25f, 4.5f, 5));
    check(Expr(Ramp::make(3.25f, 4.5f, 5)) + Expr(Broadcast::make(4.0f, 5)), Ramp::make(7.25f, 4.5f, 5));
    check(Expr(Broadcast::make(3, 3)) + Expr(Broadcast::make(1, 3)), Broadcast::make(4, 3));
    check((x + 3) + 4, x + 7);
    check(4 + (3 + x), x + 7);
    check((x + 3) + y, (x + y) + 3);
    check(y + (x + 3), (y + x) + 3);
    check((3 - x) + x, 3);
    check(x + (3 - x), 3);
    check(x*y + x*z, x*(y+z));
    check(x*y + z*x, x*(y+z));
    check(y*x + x*z, x*(y+z));
    check(y*x + z*x, x*(y+z));

    check(x - 0, x);
    check((x/y) - (x/y), 0);
    check(x - 2, x + (-2));
    check(Expr(Ramp::make(x, 2, 3)) - Expr(Ramp::make(y, 4, 3)), Ramp::make(x-y, -2, 3));
    check(Expr(Broadcast::make(4.0f, 5)) - Expr(Ramp::make(3.25f, 4.5f, 5)), Ramp::make(0.75f, -4.5f, 5));
    check(Expr(Ramp::make(3.25f, 4.5f, 5)) - Expr(Broadcast::make(4.0f, 5)), Ramp::make(-0.75f, 4.5f, 5));
    check(Expr(Broadcast::make(3, 3)) - Expr(Broadcast::make(1, 3)), Broadcast::make(2, 3));
    check((x + y) - x, y);
    check((x + y) - y, x);
    check(x - (x + y), 0 - y);
    check(x - (y + x), 0 - y);
    check((x + 3) - 2, x + 1);
    check((x + 3) - y, (x - y) + 3);
    check((x - 3) - y, (x - y) + (-3));
    check(x - (y - 2), (x - y) + 2);
    check(3 - (y - 2), 5 - y);
    check(x*y - x*z, x*(y-z));
    check(x*y - z*x, x*(y-z));
    check(y*x - x*z, x*(y-z));
    check(y*x - z*x, x*(y-z));

    check(x*0, 0);
    check(0*x, 0);
    check(x*1, x);
    check(1*x, x);
    check(Expr(2.0f)*4.0f, 8.0f);
    check(Expr(2)*4, 8);
    check((3*x)*4, x*12);
    check(4*(3+x), x*4 + 12);
    check(Expr(Broadcast::make(4.0f, 5)) * Expr(Ramp::make(3.0f, 4.0f, 5)), Ramp::make(12.0f, 16.0f, 5));
    check(Expr(Ramp::make(3.0f, 4.0f, 5)) * Expr(Broadcast::make(2.0f, 5)), Ramp::make(6.0f, 8.0f, 5));
    check(Expr(Broadcast::make(3, 3)) * Expr(Broadcast::make(2, 3)), Broadcast::make(6, 3));

    check(0/x, 0);
    check(x/1, x);
    check(x/x, 1);
    check(Expr(7)/3, 2);
    check(Expr(6.0f)/2.0f, 3.0f);
    check((x / 3) / 4, x / 12);
    check((x*4)/2, x*2);
    check((x*2)/4, x/2);
    check((x*4 + y)/2, x*2 + y/2);
    check((y + x*4)/2, y/2 + x*2);
    check((x*4 - y)/2, x*2 - y/2);
    check((y - x*4)/2, y/2 - x*2);
    check((x + 3)/2 + 7, (x + 17)/2);
    check((x/2 + 3)/5, (x + 6)/10);

    check(xf / 4.0f, xf * 0.25f);
    check(Expr(Broadcast::make(y, 4)) / Expr(Broadcast::make(x, 4)),
          Expr(Broadcast::make(y/x, 4)));
    check(Expr(Ramp::make(x, 4, 4)) / 2, Ramp::make(x/2, 2, 4));

    check(Expr(Ramp::make(4*x, 1, 4)) / 4, Broadcast::make(x, 4));
    check(Expr(Ramp::make(x*4, 1, 3)) / 4, Broadcast::make(x, 3));
    check(Expr(Ramp::make(x*8, 2, 4)) / 8, Broadcast::make(x, 4));
    check(Expr(Ramp::make(x*8, 3, 3)) / 8, Broadcast::make(x, 3));

    check(Expr(7) % 2, 1);
    check(Expr(7.25f) % 2.0f, 1.25f);
    check(Expr(-7.25f) % 2.0f, 0.75f);
    check(Expr(-7.25f) % -2.0f, -1.25f);
    check(Expr(7.25f) % -2.0f, -0.75f);
    check(Expr(Broadcast::make(x, 4)) % Expr(Broadcast::make(y, 4)),
          Expr(Broadcast::make(x % y, 4)));
    check((x*8) % 4, 0);
    check((x*8 + y) % 4, y % 4);
    check((y + x*8) % 4, y % 4);
    check((y*16 + 13) % 2, 1);
    check(Expr(Ramp::make(x, 2, 4)) % (Broadcast::make(2, 4)),
          Broadcast::make(x % 2, 4));
    check(Expr(Ramp::make(2*x+1, 4, 4)) % (Broadcast::make(2, 4)),
          Broadcast::make(1, 4));

    check(Min::make(7, 3), 3);
    check(Min::make(4.25f, 1.25f), 1.25f);
    check(Min::make(Broadcast::make(x, 4), Broadcast::make(y, 4)),
          Broadcast::make(Min::make(x, y), 4));
    check(Min::make(x, x+3), x);
    check(Min::make(x+4, x), x);
    check(Min::make(x-1, x+2), x+(-1));
    check(Min::make(7, Min::make(x, 3)), Min::make(x, 3));
    check(Min::make(Min::make(x, y), x), Min::make(x, y));
    check(Min::make(Min::make(x, y), y), Min::make(x, y));
    check(Min::make(x, Min::make(x, y)), Min::make(x, y));
    check(Min::make(y, Min::make(x, y)), Min::make(x, y));

    check(Max::make(7, 3), 7);
    check(Max::make(4.25f, 1.25f), 4.25f);
    check(Max::make(Broadcast::make(x, 4), Broadcast::make(y, 4)),
          Broadcast::make(Max::make(x, y), 4));
    check(Max::make(x, x+3), x+3);
    check(Max::make(x+4, x), x+4);
    check(Max::make(x-1, x+2), x+2);
    check(Max::make(7, Max::make(x, 3)), Max::make(x, 7));
    check(Max::make(Max::make(x, y), x), Max::make(x, y));
    check(Max::make(Max::make(x, y), y), Max::make(x, y));
    check(Max::make(x, Max::make(x, y)), Max::make(x, y));
    check(Max::make(y, Max::make(x, y)), Max::make(x, y));

    Expr t = const_true(), f = const_false();
    check(x == x, t);
    check(x == (x+1), f);
    check(x-2 == y+3, x == y+5);
    check(x+y == y+z, x == z);
    check(y+x == y+z, x == z);
    check(x+y == z+y, x == z);
    check(y+x == z+y, x == z);
    check((y+x)*17 == (z+y)*17, x == z);
    check(x*0 == y*0, t);
    check(x == x+y, y == 0);
    check(x+y == x, y == 0);

    check(x < x, f);
    check(x < (x+1), t);
    check(x-2 < y+3, x < y+5);
    check(x+y < y+z, x < z);
    check(y+x < y+z, x < z);
    check(x+y < z+y, x < z);
    check(y+x < z+y, x < z);
    check((y+x)*17 < (z+y)*17, x < z);
    check(x*0 < y*0, f);
    check(x < x+y, 0 < y);
    check(x+y < x, y < 0);

    check(select(x < 3, 2, 2), 2);
    check(select(x < (x+1), 9, 2), 9);
    check(select(x > (x+1), 9, 2), 2);
    // Selects of comparisons should always become selects of LT or selects of EQ
    check(select(x != 5, 2, 3), select(x == 5, 3, 2));
    check(select(x >= 5, 2, 3), select(x < 5, 3, 2));
    check(select(x <= 5, 2, 3), select(5 < x, 3, 2));
    check(select(x > 5, 2, 3), select(5 < x, 2, 3));

    // Check that simplifier can recognise instances where the extremes of the
    // datatype appear as constants in comparisons, Min and Max expressions.
    // The result of min/max with extreme is known to be either the extreme or
    // the other expression.  The result of < or > comparison is known to be true or false.
    check(x <= Int(32).max(), const_true());
    check(Cast::make(Int(16), x) >= Int(16).min(), const_true());
    check(x < Int(32).min(), const_false());
    check(Min::make(Cast::make(UInt(16), x), Cast::make(UInt(16), 65535)), Cast::make(UInt(16), x));
    check(Min::make(x, Int(32).max()), x);
    check(Min::make(Int(32).min(), x), Int(32).min());
    check(Max::make(Cast::make(Int(8), x), Cast::make(Int(8), -128)), Cast::make(Int(8), x));
    check(Max::make(x, Int(32).min()), x);
    check(Max::make(x, Int(32).max()), Int(32).max());
    // Check that non-extremes do not lead to incorrect simplification
    check(Max::make(Cast::make(Int(8), x), Cast::make(Int(8), -127)), Max::make(Cast::make(Int(8), x), Cast::make(Int(8), -127)));

    // Check an optimization important for fusing dimensions
    check((x/3)*3 + x%3, x);
    check(x%3 + (x/3)*3, x);

    check(((x/3)*3 + y) + x%3, x + y);
    check((x%3 + y) + (x/3)*3, x + y);

    check((y + x%3) + (x/3)*3, y + x);
    check((y + (x/3*3)) + x%3, y + x);


    // Some quaternary rules with cancellations
    check((x + y) - (z + y), x - z);
    check((x + y) - (y + z), x - z);
    check((y + x) - (z + y), x - z);
    check((y + x) - (y + z), x - z);

    check((x - y) - (z - y), x - z);
    check((y - z) - (y - x), x - z);

    check(x - min(x + y, z), max(-y, x-z));
    check(x - min(y + x, z), max(-y, x-z));
    check(x - min(z, x + y), max(-y, x-z));
    check(x - min(z, y + x), max(-y, x-z));

    check(min(x + y, z) - x, min(y, z-x));
    check(min(y + x, z) - x, min(y, z-x));
    check(min(z, x + y) - x, min(y, z-x));
    check(min(z, y + x) - x, min(y, z-x));

    check(min(x + y, z + y), min(x, z) + y);
    check(min(y + x, z + y), min(x, z) + y);
    check(min(x + y, y + z), min(x, z) + y);
    check(min(y + x, y + z), min(x, z) + y);

    check(min(123 - x, 1 - x), 1 - x);
    check(max(123 - x, 1 - x), 123 - x);

    //check(max(x, 16) - 16, max(x + -16, 0));
    //check(min(x, -4) + 7, min(x + 7, 3));

    // Min and max of clamped expressions
    check(min(clamp(x+1, y, z), clamp(x-1, y, z)), clamp(x+(-1), y, z));
    check(max(clamp(x+1, y, z), clamp(x-1, y, z)), clamp(x+1, y, z));

    // Additions that cancel a term inside a min or max
    check(x + min(y - x, z), min(y, z + x));
    check(x + max(y - x, z), max(y, z + x));
    check(min(y + (-2), z) + 2, min(y, z + 2));
    check(max(y + (-2), z) + 2, max(y, z + 2));

    check(x + min(y - x, z), min(y, z + x));
    check(x + max(y - x, z), max(y, z + x));
    check(min(y + (-2), z) + 2, min(y, z + 2));
    check(max(y + (-2), z) + 2, max(y, z + 2));

    // Min/Max distributive law
    check(max(max(x, y), max(x, z)), max(max(y, z), x));
    check(min(max(x, y), max(x, z)), max(min(y, z), x));
    check(min(min(x, y), min(x, z)), min(min(y, z), x));
    check(max(min(x, y), min(x, z)), min(max(y, z), x));

    // Mins of expressions and rounded up versions of them
    check(min(((x+7)/8)*8, x), x);
    check(min(x, ((x+7)/8)*8), x);

    check(min(((x+7)/8)*8, max(x, 8)), max(x, 8));
    check(min(max(x, 8), ((x+7)/8)*8), max(x, 8));

    // Pull constants all the way outside of a clamp
    //check(clamp(x + 1, -10, 15), clamp(x, -11, 14) + 1);
    //check(clamp(x + 1, y - 10, 15), clamp(x, y + (-11), 14) + 1);

    // The min of two matching clamps is the clamp of the mins
    check(min(clamp(x, -10, 14), clamp(y, -10, 14)), clamp(min(x, y), -10, 14));

    // The min of two clamps that match in the first arg is the clamp using the min of the bounds
    check(min(clamp(x, y, z), clamp(x, v, w)),
          clamp(x, min(y, v), min(z, w)));

    check(max(clamp(x, y, z), clamp(x, v, w)),
          clamp(x, max(y, v), max(z, w)));

    check(Ramp::make(0, 1, 4) == Broadcast::make(2, 4),
          Ramp::make(0, 1, 4) == Broadcast::make(2, 4));

    check(min(x/4, y/4), min(x, y)/4);
    check(max(x/4, y/4), max(x, y)/4);

    check(min(x/(-4), y/(-4)), max(x, y)/(-4));
    check(max(x/(-4), y/(-4)), min(x, y)/(-4));

    check(!f, t);
    check(!t, f);
    check(!(x < y), y <= x);
    check(!(x > y), x <= y);
    check(!(x >= y), x < y);
    check(!(x <= y), y < x);
    check(!(x == y), x != y);
    check(!(x != y), x == y);
    check(!(!(x == 0)), x == 0);
    check(!Expr(Broadcast::make(x > y, 4)),
          Broadcast::make(x <= y, 4));

    check(t && (x < 0), x < 0);
    check(f && (x < 0), f);
    check(t || (x < 0), t);
    check(f || (x < 0), x < 0);

    Expr vec = Variable::make(Int(32, 4), "vec");
    // Check constants get pushed inwards
    check(Let::make("x", 3, x+4), 7);

    // Check ramps in lets get pushed inwards
    check(Let::make("vec", Ramp::make(x*2+7, 3, 4), vec + Expr(Broadcast::make(2, 4))),
          Ramp::make(x*2+9, 3, 4));

    // Check broadcasts in lets get pushed inwards
    check(Let::make("vec", Broadcast::make(x, 4), vec + Expr(Broadcast::make(2, 4))),
          Broadcast::make(x+2, 4));

    // Check that dead lets get stripped
    check(Let::make("x", 3*y*y*y, 4), 4);
    check(Let::make("x", 0, 0), 0);

    std::cout << "Simplify test passed" << std::endl;
}
}
}
