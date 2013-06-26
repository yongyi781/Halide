#include <Halide.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <cmath> 

#ifdef _WIN32
#define ISFINITE(x) _finite(x)
#else
#define ISFINITE(x) std::isfinite(x)
#endif

using namespace Halide;

// Make some functions for turning types into strings
template<typename A>
const char *string_of_type();

#define DECL_SOT(name)                                          \
    template<>                                                  \
    const char *string_of_type<name>() {return #name;}          

DECL_SOT(uint8_t);    
DECL_SOT(int8_t);    
DECL_SOT(uint16_t);    
DECL_SOT(int16_t);    
DECL_SOT(uint32_t);    
DECL_SOT(int32_t);    
DECL_SOT(float);    
DECL_SOT(double);    

template<typename A>
A mod(A x, A y);

template<>
float mod(float x, float y) {
    return fmod(x, y);
}

template<>
double mod(double x, double y) {
    return fmod(x, y);
}

template<typename A>
A mod(A x, A y) {
    return x % y;
}

template<typename A>
bool close_enough(A x, A y) {
    return x == y;
}

template<>
bool close_enough<float>(float x, float y) {
    return fabs(x-y) < 1e-4;
}

template<>
bool close_enough<double>(double x, double y) {
    return fabs(x-y) < 1e-5;
}

template<typename T> 
T divide(T x, T y) {
    return (x - (((x % y) + y) % y)) / y;
}

template<>
float divide(float x, float y) {
    return x/y;
}

template<>
double divide(double x, double y) {
    return x/y;
}

int mantissa(float x) {
    int bits = 0;
    memcpy(&bits, &x, 4);
    return bits & 0x007fffff;
}

template<typename A>
bool test(int vec_width) {
    const int W = 320;
    const int H = 16;
    
    const int verbose = false;

    printf("Testing %sx%d\n", string_of_type<A>(), vec_width);

    Image<A> input(W+16, H+16);
    for (int y = 0; y < H+16; y++) {
        for (int x = 0; x < W+16; x++) {
            input(x, y) = (A)((rand() % 1024)*0.125 + 1.0);
            if ((A)(-1) < 0) {
                input(x, y) -= 10;
            }
        }
    }
    Var x, y;

    // Add
    if (verbose) printf("Add\n");
    Func f1;
    f1(x, y) = input(x, y) + input(x+1, y);
    f1.vectorize(x, vec_width);
    Image<A> im1 = f1.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(x, y) + input(x+1, y);
            if (im1(x, y) != correct) {
                printf("im1(%d, %d) = %f instead of %f\n", x, y, (double)(im1(x, y)), (double)(correct));
                return false;
            }
        }
    }
    
    // Sub
    if (verbose) printf("Subtract\n");
    Func f2;
    f2(x, y) = input(x, y) - input(x+1, y);
    f2.vectorize(x, vec_width);
    Image<A> im2 = f2.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(x, y) - input(x+1, y);
            if (im2(x, y) != correct) {
                printf("im2(%d, %d) = %f instead of %f\n", x, y, (double)(im2(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // Mul
    if (verbose) printf("Multiply\n");
    Func f3;
    f3(x, y) = input(x, y) * input(x+1, y);
    f3.vectorize(x, vec_width);
    Image<A> im3 = f3.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(x, y) * input(x+1, y);
            if (im3(x, y) != correct) {
                printf("im3(%d, %d) = %f instead of %f\n", x, y, (double)(im3(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // select
    if (verbose) printf("Select\n");
    Func f4;
    f4(x, y) = select(input(x, y) > input(x+1, y), input(x+2, y), input(x+3, y));
    f4.vectorize(x, vec_width);
    Image<A> im4 = f4.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(x, y) > input(x+1, y) ? input(x+2, y) : input(x+3, y);
            if (im4(x, y) != correct) {
                printf("im4(%d, %d) = %f instead of %f\n", x, y, (double)(im4(x, y)), (double)(correct));
                return false;
            }
        }
    }


    // Gather
    if (verbose) printf("Gather\n");
    Func f5;
    Expr xCoord = clamp(cast<int>(input(x, y)), 0, W-1);
    Expr yCoord = clamp(cast<int>(input(x+1, y)), 0, H-1);
    f5(x, y) = input(xCoord, yCoord);
    f5.vectorize(x, vec_width);
    Image<A> im5 = f5.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int xCoord = (int)(input(x, y));
            if (xCoord >= W) xCoord = W-1;
            if (xCoord < 0) xCoord = 0;

            int yCoord = (int)(input(x+1, y));
            if (yCoord >= H) yCoord = H-1;
            if (yCoord < 0) yCoord = 0;

            A correct = input(xCoord, yCoord);

            if (im5(x, y) != correct) {
                printf("im5(%d, %d) = %f instead of %f\n", x, y, (double)(im5(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // Gather and scatter with constant but unknown stride
    Func f5a;
    f5a(x, y) = input(x, y)*cast<A>(2);
    f5a.vectorize(y, vec_width);
    Image<A> im5a = f5a.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(x, y) * ((A)(2));
            if (im5a(x, y) != correct) {
                printf("im5a(%d, %d) = %f instead of %f\n", x, y, (double)(im5a(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // Scatter
    if (verbose) printf("Scatter\n");
    Func f6;
    RDom i(0, H);
    // Set one entry in each row high
    xCoord = clamp(cast<int>(input(2*i, i)), 0, W-1);
    f6(x, y) = 0;
    f6(xCoord, i) = 1;

    f6.vectorize(x, vec_width);

    Image<int> im6 = f6.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        int xCoord = (int)(input(2*y, y));
        if (xCoord >= W) xCoord = W-1;
        if (xCoord < 0) xCoord = 0;
        for (int x = 0; x < W; x++) {
            int correct = x == xCoord ? 1 : 0;
            if (im6(x, y) != correct) {
                printf("im6(%d, %d) = %d instead of %d\n", x, y, im6(x, y), correct);
                return false;
            }
        }
    }

    // Min/max
    if (verbose) printf("Min/max\n");
    Func f7;
    f7(x, y) = clamp(input(x, y), cast<A>(10), cast<A>(20));
    f7.vectorize(x, vec_width);
    Image<A> im7 = f7.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (im7(x, y) < (A)10 || im7(x, y) > (A)20) {
                printf("im7(%d, %d) = %f\n", x, y, (double)(im7(x, y)));
                return false;
            }
        }
    }

    // Extern function call
    if (verbose) printf("External call to hypot\n");
    Func f8;
    f8(x, y) = hypot(1.1f, cast<float>(input(x, y)));
    f8.vectorize(x, vec_width);
    Image<float> im8 = f8.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float correct = hypot(1.1f, (float)input(x, y));
            if (!close_enough(im8(x, y), correct)) {
                printf("im8(%d, %d) = %f instead of %f\n", 
                       x, y, (double)im8(x, y), correct);
                return false;
            }
        }
    }
    
    // Div
    if (verbose) printf("Division\n");
    Func f9;
    f9(x, y) = input(x, y) / clamp(input(x+1, y), cast<A>(1), cast<A>(3));
    f9.vectorize(x, vec_width);
    Image<A> im9 = f9.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A clamped = input(x+1, y);
            if (clamped < (A)1) clamped = (A)1;
            if (clamped > (A)3) clamped = (A)3;
            A correct = divide(input(x, y), clamped);
            // We allow floating point division to take some liberties with accuracy
            if (!close_enough(im9(x, y), correct)) {
                printf("im9(%d, %d) = %f/%f = %f instead of %f\n", 
                       x, y, 
                       (double)input(x, y), (double)clamped,
                       (double)(im9(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // Divide by small constants
    if (verbose) printf("Dividing by small constants\n");
    for (int c = 2; c < 16; c++) {
	Func f10;
	f10(x, y) = (input(x, y)) / cast<A>(Expr(c));
	f10.vectorize(x, vec_width);
	Image<A> im10 = f10.realize(W, H);
	
	for (int y = 0; y < H; y++) {
	    for (int x = 0; x < W; x++) {	  
                A correct = divide(input(x, y), (A)c);

                if (!close_enough(im10(x, y), correct)) {
		    printf("im10(%d, %d) = %f/%d = %f instead of %f\n", x, y, 
			   (double)(input(x, y)), c,
			   (double)(im10(x, y)), 
			   (double)(correct));
		    printf("Error when dividing by %d\n", c);
		    return false;
		}
	    }
	}    
    }

    // Interleave
    if (verbose) printf("Interleaving store\n");
    Func f11;
    f11(x, y) = select((x%2)==0, input(x/2, y), input(x/2, y+1));
    f11.vectorize(x, vec_width);
    Image<A> im11 = f11.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = ((x%2)==0) ? input(x/2, y) : input(x/2, y+1);
            if (im11(x, y) != correct) {
                printf("im11(%d, %d) = %f instead of %f\n", x, y, (double)(im11(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // Reverse
    if (verbose) printf("Reversing\n");
    Func f12;
    f12(x, y) = input(W-1-x, H-1-y);
    f12.vectorize(x, vec_width);
    Image<A> im12 = f12.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(W-1-x, H-1-y);
            if (im12(x, y) != correct) {
                printf("im12(%d, %d) = %f instead of %f\n", x, y, (double)(im12(x, y)), (double)(correct));
                return false;
            }
        }
    }

    // Unaligned load with known shift
    if (verbose) printf("Unaligned load\n");
    Func f13;
    f13(x, y) = input(x+3, y);
    f13.vectorize(x, vec_width);
    Image<A> im13 = f13.realize(W, H);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            A correct = input(x+3, y);
            if (im13(x, y) != correct) {
                printf("im13(%d, %d) = %f instead of %f\n", x, y, (double)(im13(x, y)), (double)(correct));
            }
        }
    }
    
    // Absolute value
    if (!type_of<A>().is_uint()) {
        if (verbose) printf("Absolute value\n");
        Func f14;
        f14(x, y) = abs(input(x, y));
        Image<A> im14 = f14.realize(W, H);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                A correct = input(x, y);
                if (correct < 0) correct = -correct;
                if (im14(x, y) != correct) {
                    printf("im14(%d, %d) = %f instead of %f\n", x, y, (double)(im14(x, y)), (double)(correct));
                }
            }
        }
    }

    // Fast exp, log, and pow
    if (type_of<A>() == Float(32)) {
        if (verbose) printf("Fast transcendentals\n");
        Func f15, f16, f17, f18, f19, f20;
        Expr a = input(x, y) * 0.5f;
        Expr b = input((x+1)%W, y) * 0.5f;
        f15(x, y) = log(a);
        f16(x, y) = exp(b);
        f17(x, y) = pow(a, b/16.0f);
        f18(x, y) = fast_log(a);
        f19(x, y) = fast_exp(b);
        f20(x, y) = fast_pow(a, b/16.0f);
        Image<float> im15 = f15.realize(W, H);
        Image<float> im16 = f16.realize(W, H);
        Image<float> im17 = f17.realize(W, H);
        Image<float> im18 = f18.realize(W, H);
        Image<float> im19 = f19.realize(W, H);
        Image<float> im20 = f20.realize(W, H);

        float worst_log_error = 1e20;
        float worst_exp_error = 1e20;
        float worst_pow_error = 1e20;
        float worst_fast_log_error = 1e20;
        float worst_fast_exp_error = 1e20;
        float worst_fast_pow_error = 1e20;

        int worst_log_mantissa = 0;
        int worst_exp_mantissa = 0;
        int worst_pow_mantissa = 0;
        int worst_fast_log_mantissa = 0;
        int worst_fast_exp_mantissa = 0;
        int worst_fast_pow_mantissa = 0;

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float a = input(x, y) * 0.5f;
                float b = input((x+1)%W, y) * 0.5f;
                float correct_log = logf(a);
                float correct_exp = expf(b);
                float correct_pow = powf(a, b/16.0f);

                int correct_log_mantissa = mantissa(correct_log);
                int correct_exp_mantissa = mantissa(correct_exp);
                int correct_pow_mantissa = mantissa(correct_pow);

                int log_mantissa = mantissa(im15(x, y));
                int exp_mantissa = mantissa(im16(x, y));
                int pow_mantissa = mantissa(im17(x, y));

                int fast_log_mantissa = mantissa(im18(x, y));
                int fast_exp_mantissa = mantissa(im19(x, y));
                int fast_pow_mantissa = mantissa(im20(x, y));

                int log_mantissa_error = abs(log_mantissa - correct_log_mantissa);
                int exp_mantissa_error = abs(exp_mantissa - correct_exp_mantissa);
                int pow_mantissa_error = abs(pow_mantissa - correct_pow_mantissa);
                int fast_log_mantissa_error = abs(fast_log_mantissa - correct_log_mantissa);
                int fast_exp_mantissa_error = abs(fast_exp_mantissa - correct_exp_mantissa);
                int fast_pow_mantissa_error = abs(fast_pow_mantissa - correct_pow_mantissa);

                worst_log_mantissa = std::max(worst_log_mantissa, log_mantissa_error);
                worst_exp_mantissa = std::max(worst_exp_mantissa, exp_mantissa_error);

                if (a >= 0) 
                    worst_pow_mantissa = std::max(worst_pow_mantissa, pow_mantissa_error);
                
                if (ISFINITE(correct_log))
                    worst_fast_log_mantissa = std::max(worst_fast_log_mantissa, fast_log_mantissa_error);
                
                if (ISFINITE(correct_exp))
                    worst_fast_exp_mantissa = std::max(worst_fast_exp_mantissa, fast_exp_mantissa_error);
                
                if (ISFINITE(correct_pow) && a > 0)
                    worst_fast_pow_mantissa = std::max(worst_fast_pow_mantissa, fast_pow_mantissa_error);

                if (log_mantissa_error > 2) {
                    printf("log(%f) = %1.10f instead of %1.10f (mantissa: %d vs %d)\n", 
                           a, im15(x, y), correct_log, correct_log_mantissa, log_mantissa);
                }
                if (exp_mantissa_error > 2) {
                    printf("exp(%f) = %1.10f instead of %1.10f (mantissa: %d vs %d)\n", 
                           b, im16(x, y), correct_exp, correct_exp_mantissa, exp_mantissa);
                }
                if (a >= 0 && pow_mantissa_error > 32) {
                    printf("pow(%f, %f) = %1.10f instead of %1.10f (mantissa: %d vs %d)\n", 
                           a, b/16.0f, im17(x, y), correct_pow, correct_pow_mantissa, pow_mantissa);
                }
                if (ISFINITE(correct_log) && fast_log_mantissa_error > 64) {
                    printf("fast_log(%f) = %1.10f instead of %1.10f (mantissa: %d vs %d)\n", 
                           a, im18(x, y), correct_log, correct_log_mantissa, fast_log_mantissa);
                }
                if (ISFINITE(correct_exp) && fast_exp_mantissa_error > 64) {
                    printf("fast_exp(%f) = %1.10f instead of %1.10f (mantissa: %d vs %d)\n", 
                           b, im19(x, y), correct_exp, correct_exp_mantissa, fast_exp_mantissa);
                }
                if (a >= 0 && ISFINITE(correct_pow) && fast_pow_mantissa_error > 128) {
                    printf("fast_pow(%f, %f) = %1.10f instead of %1.10f (mantissa: %d vs %d)\n",
                           a, b/16.0f, im20(x, y), correct_pow, correct_pow_mantissa, fast_pow_mantissa);
                }
            }
        }

        /*
        printf("log mantissa error: %d\n", worst_log_mantissa);
        printf("exp mantissa error: %d\n", worst_exp_mantissa);
        printf("pow mantissa error: %d\n", worst_pow_mantissa);
        printf("fast_log mantissa error: %d\n", worst_fast_log_mantissa);
        printf("fast_exp mantissa error: %d\n", worst_fast_exp_mantissa);
        printf("fast_pow mantissa error: %d\n", worst_fast_pow_mantissa);
        */
    }

    return true;
}

int main(int argc, char **argv) {

    bool ok = true;

    // Only native vector widths - llvm doesn't handle others well
    ok = ok && test<float>(4);
    ok = ok && test<float>(8);
    ok = ok && test<double>(2);
    ok = ok && test<uint8_t>(16);
    ok = ok && test<int8_t>(16);
    ok = ok && test<uint16_t>(8);
    ok = ok && test<int16_t>(8);
    ok = ok && test<uint32_t>(4);
    ok = ok && test<int32_t>(4);

    if (!ok) return -1;
    printf("Success!\n");
    return 0;
}
