#include <FImage.h>
#include <sys/time.h>

using namespace FImage;

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

double currentTime() {
    timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0f;
}

template<typename A>
bool test(int vec_width, int attempts = 0) {
    
    int W = vec_width*1;
    int H = 40000;

    Image<A> input(W, H+20);
    for (int y = 0; y < H+20; y++) {
        for (int x = 0; x < W; x++) {
            input(x, y) = (A)(rand()*0.125 + 1.0);
        }
    }

    Var x, y;
    Func f, g;

    Expr e = input(x, y);
    for (int i = 1; i < 10; i++) {
        e = e + input(x, y+i);
    }

    for (int i = 10; i >= 0; i--) {
        e = e + input(x, y+i);
    }

    f(x, y) = e;
    g(x, y) = e;
    f.vectorize(x, vec_width);

    Image<A> outputg = g.realize(W, H);
    Image<A> outputf = f.realize(W, H);


    double t1 = currentTime();
    g.realize(outputg);
    double t2 = currentTime();
    f.realize(outputf);
    double t3 = currentTime();

    printf("%g %g %g\n", t1, t2, t3);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (outputf(x, y) != outputg(x, y)) {
                printf("%s x %d failed: %d vs %d\n",
                       string_of_type<A>(), vec_width,
                       (int)outputf(x, y),
                       (int)outputg(x, y)
                    );
                return false;
            }            
        }
    }

    printf("Vectorized vs scalar (%s x %d): %1.3gms %1.3gms\n", string_of_type<A>(), vec_width, (t3-t2), (t2-t1));

    if ((t3 - t2) > (t2 - t1)) {
        if (attempts < 1) {
            if (test<A>(vec_width, attempts+1)) return true;
        } else {
            printf("For 5 attempts, vectorizing was slower than not vectorizing: %f vs %f\n",
                   t3-t2, t2-t1);
            return false;
        }                
    } 


    return true;
}

int main(int argc, char **argv) {

    bool ok = true;

    //test<uint8_t>(16);
    //test<uint8_t>(16);


    test<double>(2);
    test<float>(4);
    test<uint8_t>(16);
    test<int8_t>(16);
    test<uint16_t>(8);
    test<int16_t>(8);
    test<uint32_t>(4);
    test<int32_t>(4);
    return 0;

    // We only support power-of-two vector widths for now
    for (int vec_width = 2; vec_width < 32; vec_width*=2) {
        ok = ok && test<float>(vec_width);
        ok = ok && test<double>(vec_width);
        ok = ok && test<uint8_t>(vec_width);
        ok = ok && test<int8_t>(vec_width);
        ok = ok && test<uint16_t>(vec_width);
        ok = ok && test<int16_t>(vec_width);
        ok = ok && test<uint32_t>(vec_width);
        ok = ok && test<int32_t>(vec_width);
    }

    if (!ok) return -1;
    printf("Success!\n");
    return 0;
}